#include "network_app.h"
#include "protocol_onenet.h" // 引入云端协议分析处理模块
#include "key.h"
#include "atk_mw8266d_uart.h"
#include "atk_mw8266d.h"
#include "ota.h"
#include "wdog.h"
#include "delay.h"   // delay_ms 延时函数


// ===== 文件级全局状态 =====
static unsigned char mqtt_send_buf[512];    // 发送缓冲区
static unsigned char mqtt_recv_buf[512];    // 接收缓冲区
volatile uint8_t g_ota_request = 0;         // OTA 升级请求标志（由 protocol_onenet 置位）

/**
 * @brief  MQTT 连接状态（文件内收敛，对外通过 net_is_connected() 查询）
 */
typedef struct
{
    int      sock;       // 套接字句柄，-1 = 未连接
    uint8_t  connected;  // 1 = 已连接（合并了原 con_status 和 g_mqtt_connected 两个全局变量）
} NetState;
static NetState net_state = { -1, 0 };

uint8_t net_is_connected(void)
{
    return net_state.connected;
}

/* 前向声明：这两个函数被靠前的函数（net_handle_key / net_mqtt_handshake）调用 */
static void net_disconnect(NetTaskContext *ctx);
static void net_mark_wifi_rejoin(NetTaskContext *ctx, TickType_t now);

/**
 * @brief       初始化网络任务上下文
 * @param[out]  ctx: 清零并设置默认参数
 * @note        默认：重连退避 1s 起步（最大 32s），心跳空闲阈值 5s、超时 2s
 */
void net_ctx_init(NetTaskContext *ctx)
{
    memset(ctx, 0, sizeof(NetTaskContext));
    ctx->backoff_ms = 1000;
    ctx->max_backoff_ms = 32000;
    ctx->ping_interval_tick = pdMS_TO_TICKS(5000);
    ctx->ping_timeout_tick = pdMS_TO_TICKS(2000);

    // 注意：ARMCC 的 C99 模式不支持 struct = 复合字面量直接赋值，必须用临时变量 + memcpy
    MQTTPacket_connectData temp_connect = MQTTPacket_connectData_initializer;
    memcpy(&ctx->connect_data, &temp_connect, sizeof(MQTTPacket_connectData));

    MQTTString temp_string = MQTTString_initializer;
    memcpy(&ctx->topic_string, &temp_string, sizeof(MQTTString));
    memcpy(&ctx->subscribe_topic, &temp_string, sizeof(MQTTString));

    ctx->sub_packet_id = 1;
}

/**
 * @brief       定时打印统计信息（每 5 秒一次）
 * @param[in]   ctx: 含统计计数
 * @param[in]   now: 当前系统滴答
 * @note        打印：本次开机缓存总数 / 成功补发总数 / Flash 剩余待发数
 */
static void net_print_stats(NetTaskContext *ctx, TickType_t now)
{
    if ((now - ctx->last_report_tick) > pdMS_TO_TICKS(5000))
    {
        printf("[stat] cached=%lu sent=%lu pending=%lu\r\n",
               ctx->cached_total, ctx->sent_total, (uint32_t)flash_valid_count);
        ctx->last_report_tick = now;
    }
}

/**
 * @brief       填充 MQTT CONNECT 报文的连接参数
 * @param[out]  ctx: 填充 connect_data 字段
 * @note        版本 V4、ClientID、KeepAlive 60s、CleanSession、用户名/密码
 */
static void net_build_connect(NetTaskContext *ctx)
{
    ctx->connect_data.MQTTVersion = 4;
    ctx->connect_data.clientID.cstring = MQTT_CLIENT_ID;
    ctx->connect_data.keepAliveInterval = 60;
    ctx->connect_data.cleansession = 1;
    ctx->connect_data.willFlag = 0;
    if (strlen(MQTT_USER_NAME) > 0) ctx->connect_data.username.cstring = MQTT_USER_NAME;
    if (strlen(MQTT_PASSWORD) > 0) ctx->connect_data.password.cstring = MQTT_PASSWORD;
}

/**
 * @brief       处理按键控制网络连接/断开
 * @param[in,out] ctx: 更新重连标志和退避参数
 * @param[in]   now: 当前系统滴答
 * @param[out]  request_connect: 置 1 表示请求立即连接
 * @note        KEY0: 开启自动重连并立即尝试连接。
 *              KEY1: 已连接则发送 DISCONNECT 并断开，1.5 秒后开始新的重连周期。
 *              KEY2: 擦除 SPI Flash 离线数据区（格式化，清空积压数据）。
 */
static void net_handle_key(NetTaskContext *ctx, TickType_t now, uint8_t *request_connect)
{
    switch (ctx->key)
    {
        case KEY0_PRES:
            ctx->auto_reconnect = 1;
            *request_connect = 1;
            printf("Connecting to MQTT Broker...\r\n");
            break;
        case WKUP_PRES:
            printf("WK_UP pressed: erasing SPI Flash offline data area...\r\n");
            flash_store_format_async();   // 委托 flash_writer_task 串行擦除，避免与写入冲突
            break;
        case KEY1_PRES:
            if (net_state.sock >= 0 && net_state.connected)
            {
                ctx->packet_len = MQTTSerialize_disconnect(mqtt_send_buf, sizeof(mqtt_send_buf));
                if (ctx->packet_len > 0)
                {
                    transport_sendPacketBuffer(net_state.sock, mqtt_send_buf, ctx->packet_len);
                }
                net_disconnect(ctx);
                printf("MQTT Disconnected!\r\n");
            }
            ctx->auto_reconnect = 1;
            ctx->backoff_ms = 1000;
            ctx->next_retry_tick = now + pdMS_TO_TICKS(1500);
            break;
        default:
            break;
    }
}

/* ====================================================================
 * MQTT 连接状态机 — 拆分为 4 个职责单一的函数 + 1 个编排者
 * ==================================================================== */

/**
 * @brief  重试门禁：检查是否到了允许重试的时间
 * @retval 1=允许尝试  0=未到时间
 */
static uint8_t net_retry_gate(NetTaskContext *ctx, TickType_t now, uint8_t force)
{
    if (force) return 1;  // 按键强制请求连接，直接放行
    if (!ctx->auto_reconnect) return 0;
    if (ctx->next_retry_tick == 0 || now >= ctx->next_retry_tick) return 1; // 到重试时间了
    return 0;
}

/**
 * @brief  WiFi 层：确保模组已连上 AP 并获取到 IP
 *         WiFi 重连用固定 5 秒间隔（信号问题翻倍无意义）
 * @retval 1=链路就绪  0=WiFi 不通
 */
static uint8_t net_wifi_ensure_link(NetTaskContext *ctx, TickType_t now)
{
    char ip_buf[16]; // 只用来承载 get_ip 的返回值，内容不需要读取

    // 需要重连 WiFi 且到了重试时间 → 尝试连接
    if (ctx->wifi_rejoin_needed && (ctx->wifi_retry_tick == 0 || now >= ctx->wifi_retry_tick))
    {
        if (atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD) == ATK_MW8266D_EOK)
        {
            ctx->wifi_rejoin_needed = 0;
            ctx->wifi_retry_tick    = 0;
        }
        else
        {
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000);
            return 0;
        }
    }

    // WiFi 连着但没拿到 IP（透传模式异常），重连一次
    if (atk_mw8266d_get_ip(ip_buf) != ATK_MW8266D_EOK)
    {
        if (atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD) != ATK_MW8266D_EOK)
        {
            ctx->wifi_rejoin_needed = 1;
            ctx->wifi_retry_tick    = now + pdMS_TO_TICKS(5000);
            return 0;
        }
    }

    atk_mw8266d_uart_rx_restart();  // 清掉串口里残留的旧数据
    return 1;
}

/**
 * @brief  MQTT 握手：TCP 连接 → 发 CONNECT → 收 CONNACK → 发 Subscribe
 *         只做业务步骤，不处理重连退避
 * @retval 1=握手成功  0=失败（已断开）
 */
static uint8_t net_mqtt_handshake(NetTaskContext *ctx, TickType_t now)
{
    // 第一步：建立 TCP 连接（进入透传模式）
    net_state.sock = transport_open(MQTT_BROKER_IP, atoi(MQTT_BROKER_PORT));
    if (net_state.sock < 0)
    {
        printf("Transport Open Failed!\r\n");
        net_state.connected = 0;
        net_mark_wifi_rejoin(ctx, now);
        return 0;
    }

    printf("TCP Connected, Entered Transparent Mode.\r\n");
    delay_ms(500);

    // 第二步：发送 MQTT CONNECT 报文
    net_build_connect(ctx);
    ctx->packet_len = MQTTSerialize_connect(mqtt_send_buf, sizeof(mqtt_send_buf), &ctx->connect_data);
    if (ctx->packet_len <= 0 ||
        transport_sendPacketBuffer(net_state.sock, mqtt_send_buf, ctx->packet_len) != ctx->packet_len)
    {
        printf("MQTT Connect Send Failed!\r\n");
        net_disconnect(ctx);
        return 0;
    }

    // 第三步：接收 CONNACK 回应
    ctx->packet_len = MQTTPacket_read(mqtt_recv_buf, sizeof(mqtt_recv_buf), transport_getdata);
    if (ctx->packet_len <= 0 ||
        !MQTTDeserialize_connack(&ctx->sessionPresent, &ctx->connack_rc, mqtt_recv_buf, ctx->packet_len))
    {
        printf("MQTT CONNACK Timeout or Error!\r\n");
        net_disconnect(ctx);
        return 0;
    }

    // 第四步：检查服务器返回码（0 = 接受连接）
    if (ctx->connack_rc != MQTT_CONNECTION_ACCEPTED)
    {
        printf("MQTT Rejected! RC=%d\r\n", ctx->connack_rc);
        net_disconnect(ctx);
        return 0;
    }

    // 第五步：连接成功，记录状态并发订阅
    printf("MQTT Connected!\r\n");
    net_state.connected = 1;
    ctx->last_rx_tick = now;
    ctx->last_ping_tick   = now;
    ctx->waiting_pingresp = 0;

    ctx->subscribe_topic.cstring = MQTT_TOPIC_SUB;
    int req_qos[1] = {0};
    ctx->packet_len = MQTTSerialize_subscribe(mqtt_send_buf, sizeof(mqtt_send_buf),
                                               0, ctx->sub_packet_id++,
                                               1, &ctx->subscribe_topic, req_qos);
    if (ctx->packet_len > 0)
    {
        transport_sendPacketBuffer(net_state.sock, mqtt_send_buf, ctx->packet_len);
        printf("MQTT Subscribe Sent.\r\n");
    }
    return 1;
}

/**
 * @brief  更新重连退避：连接成功 → 重置 1s；失败 → 翻倍（1→2→4→…→32s 封顶）
 * @note   翻倍后加随机抖动，避免大量设备同时恢复时一起重连冲击服务器（惊群效应）
 */
static void net_backoff_update(NetTaskContext *ctx, TickType_t now)
{
    if (net_state.connected)
    {
        // 连接成功 → 退避清零
        ctx->backoff_ms       = 1000;
        ctx->next_retry_tick  = 0;
    }
    else if (ctx->auto_reconnect)
    {
        // 连接失败 → 退避翻倍（封顶 32s）
        if (ctx->backoff_ms < ctx->max_backoff_ms)
            ctx->backoff_ms <<= 1;
        if (ctx->backoff_ms > ctx->max_backoff_ms)
            ctx->backoff_ms = ctx->max_backoff_ms;

        // 抖动范围 0 ~ backoff_ms/2，用 HAL Tick 低位做简易随机源
        // 例：backoff=4s → jitter∈[0,2s] → 实际等待 4~6s
        uint32_t jitter = HAL_GetTick() % (ctx->backoff_ms / 2U + 1U);
        ctx->next_retry_tick = now + pdMS_TO_TICKS(ctx->backoff_ms + jitter);
    }
}

/**
 * @brief  断开 MQTT 连接：关 socket、清连接状态
 * @note   全文件所有"发送/接收失败 → 断开"路径统一走这里
 */
static void net_disconnect(NetTaskContext *ctx)
{
    if (net_state.sock >= 0)
    {
        transport_close(net_state.sock);
    }
    net_state.sock = -1;
    net_state.connected = 0;
}

/**
 * @brief  标记需要重连 WiFi，并设定 5 秒后的重试时间
 */
static void net_mark_wifi_rejoin(NetTaskContext *ctx, TickType_t now)
{
    ctx->wifi_rejoin_needed = 1;
    ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000);
}

/**
 * @brief  连接编排者（net_task 主循环调用）
 *         ① 已连接？直接返回
 *         ② 到重试时间了吗？（门禁）
 *         ③ WiFi 就绪了吗？
 *         ④ MQTT 握手
 *         ⑤ 更新退避状态
 */
static uint8_t net_try_connect(NetTaskContext *ctx, TickType_t now, uint8_t request_connect)
{
    if (net_state.connected && net_state.sock >= 0) return 1;

    net_state.connected = 0;

    if (!net_retry_gate(ctx, now, request_connect)) return 0;
    if (!net_wifi_ensure_link(ctx, now))             return 0;
    if (!net_mqtt_handshake(ctx, now))
    {
        net_disconnect(ctx);
    }
    net_backoff_update(ctx, now);

    return (net_state.connected && net_state.sock >= 0) ? 1 : 0;
}

/**
 * @brief       断网期间，把传感器数据打包成 JSON 存进 SPI Flash
 * @param[in,out] ctx: 使用 ap_data 和 json_buf
 * @note        从 MQTT 上报队列取数据（非阻塞），序列化后异步写入 Flash
 */
static void net_cache_offline_data(NetTaskContext *ctx)
{
    while (xQueueReceive(xAP3216CQueueForMQTT, &ctx->ap_data, 0) == pdTRUE) // 队列里有数据就取出处理
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "als", ctx->ap_data.als);
        cJSON_AddNumberToObject(root, "ir", ctx->ap_data.ir);
        cJSON_AddNumberToObject(root, "ps", ctx->ap_data.ps);
        if (cJSON_PrintPreallocated(root, ctx->json_buf, sizeof(ctx->json_buf), 0)) // 序列化成功才入库
        {
            flash_store_push_async(ctx->json_buf, (uint16_t)strlen(ctx->json_buf)); // 异步写入，不阻塞本任务
            ctx->cached_total++;
            printf("[offline] stored=%lu pending=%lu\r\n",
                   ctx->cached_total, flash_valid_count);
        }
        cJSON_Delete(root); // 释放 cJSON 内存
    }
}

/**
 * @brief       重连成功后，把断网期间积压在 Flash 里的数据补发到云端
 * @param[in,out] ctx: 用 json_buf 读数据、mqtt_send_buf 发包
 * @retval      1 = 无积压或补发完成；0 = 发送失败已断开连接
 * @note        FIFO 取最旧一条；发送成功（写入 TCP 缓冲）后才标记已发送，保证不丢
 */
static uint8_t net_publish_flash_backlog(NetTaskContext *ctx)
{
    if (flash_valid_count == 0) return 1; // 没有积压数据，直接通过
    uint16_t cached_len = 0;
    uint32_t pop_index = 0;
    if (!flash_store_peek_async(ctx->json_buf, sizeof(ctx->json_buf), &cached_len, &pop_index)) return 1; // 读最旧一条（阻塞等 Flash 任务应答）
    printf("[backlog] read len=%d pending=%lu\r\n", cached_len, flash_valid_count);
    MQTTString pub_topic = MQTTString_initializer;
    pub_topic.cstring = MQTT_TOPIC_PUB;
    ctx->packet_len = MQTTSerialize_publish(mqtt_send_buf, sizeof(mqtt_send_buf), 0, 0, 0, 0,
                                            pub_topic, (unsigned char*)ctx->json_buf, cached_len); // 打包成 MQTT Publish 报文
    if (ctx->packet_len > 0)
    {
        if (transport_sendPacketBuffer(net_state.sock, mqtt_send_buf, ctx->packet_len) == ctx->packet_len) // 写入 TCP 缓冲成功
        {
            printf("[backlog] sent ok, pending=%lu\r\n", flash_valid_count);
            flash_store_mark_sent_async(pop_index); // 发送成功才标记删除这条
            ctx->sent_total++;
            return 1;
        }
        printf("Send Flash Data Failed!\r\n");
        net_disconnect(ctx);
        return 0;
    }
    return 1; // 序列化失败（包太大等），不判死，下轮重试
}

/**
 * @brief       MQTT 心跳保活与断线检测（每轮主循环调用）
 * @param[in,out] ctx: 心跳状态机（等待标志 + 时间戳）
 * @param[in]   now: 当前系统滴答
 * @note        动态心跳：空闲超过 5s 才发 PINGREQ；发出后 2s 无 PINGRESP 判死断开。
 *              有业务数据收发时 last_rx_tick 会被刷新，自动不发心跳（省流量）。
 */
static void net_keepalive_step(NetTaskContext *ctx, TickType_t now)
{
    if (!(net_state.connected && net_state.sock >= 0)) return; // 未连接就不需要保活

    if (ctx->waiting_pingresp) // 分支1：心跳已发出，正在等 PINGRESP
    {
        if ((now - ctx->last_ping_tick) > ctx->ping_timeout_tick) // 超过 2s 没收到回包 → 判死
        {
            printf("MQTT Ping Timeout!\r\n");
            net_disconnect(ctx);
            ctx->waiting_pingresp = 0;
            net_mark_wifi_rejoin(ctx, now);
        }
    }
    // 分支2：没在等回包 → 看空闲时间是否超阈值
    else if ((now - ctx->last_rx_tick) > ctx->ping_interval_tick)
    {
        ctx->packet_len = MQTTSerialize_pingreq(mqtt_send_buf, sizeof(mqtt_send_buf)); // 组装 PINGREQ
        if (ctx->packet_len > 0 && transport_sendPacketBuffer(net_state.sock, mqtt_send_buf, ctx->packet_len) == ctx->packet_len) // 发送成功
        {
            ctx->waiting_pingresp = 1; // 进入等待回包状态
            ctx->last_ping_tick = now;
        }
        else // 心跳都发不出去 → TCP 已死，直接断开
        {
            printf("MQTT Ping Send Failed!\r\n");
            net_disconnect(ctx);
            ctx->waiting_pingresp = 0;
            net_mark_wifi_rejoin(ctx, now);
        }
    }
}

/**
 * @brief       把最新传感器数据实时上报到云端（OneNET 物模型格式）
 * @param[in,out] ctx: 从队列取 ap_data，用 json_buf 序列化后发送
 * @note        非阻塞取队列（50ms 超时）；发送失败则断开连接
 */
static void net_publish_realtime_data(NetTaskContext *ctx)
{
    if (xQueueReceive(xAP3216CQueueForMQTT, &ctx->ap_data, pdMS_TO_TICKS(50)) != pdTRUE) return;

    // 拼 OneNET 物模型 JSON：{"id":"123","version":"1.0","params":{...}}
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", "123");
    cJSON_AddStringToObject(root, "version", "1.0");

    cJSON *params = cJSON_CreateObject();

    cJSON *als_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(als_obj, "value", ctx->ap_data.als);
    cJSON_AddItemToObject(params, "als", als_obj);

    cJSON *ir_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(ir_obj, "value", ctx->ap_data.ir);
    cJSON_AddItemToObject(params, "ir", ir_obj);

    cJSON *ps_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(ps_obj, "value", ctx->ap_data.ps);
    cJSON_AddItemToObject(params, "ps", ps_obj);

    cJSON_AddItemToObject(root, "params", params);

    if (!cJSON_PrintPreallocated(root, ctx->json_buf, sizeof(ctx->json_buf), 0))
    {
        printf("JSON Print Failed!\r\n");
    }
    cJSON_Delete(root);
    MQTTString pub_topic = MQTTString_initializer;
    pub_topic.cstring = MQTT_TOPIC_PUB;
    ctx->packet_len = MQTTSerialize_publish(mqtt_send_buf, sizeof(mqtt_send_buf), 0, 0, 0, 0,
                                            pub_topic, (unsigned char*)ctx->json_buf, strlen(ctx->json_buf));
    if (ctx->packet_len > 0)
    {
        int sret = transport_sendPacketBuffer(net_state.sock, mqtt_send_buf, ctx->packet_len);
        if (sret == ctx->packet_len)
        {
            printf("MQTT Pub AP3216C OK\r\n");
        }
        else
        {
            printf("MQTT Pub Send Failed!\r\n");
            if (net_state.connected)
            {
                net_disconnect(ctx);
            }
        }
    }
    else
    {
        printf("MQTT Pub Serialize Failed!\r\n");
    }
}

/**
 * @brief       接收并解析云平台下发的 MQTT 报文
 * @param[in,out] ctx: 更新接收时间、等待标志，解析出 payload 交给业务处理
 * @param[in]   now: 当前系统滴答
 * @note        非阻塞读取。处理 TCP 粘包（一次收多包）；按类型分发：
 *              PUBLISH（云下发指令）→ data_process_mqtt_msg 处理
 *              SUBACK（订阅确认）、PINGRESP（心跳回复）→ 更新状态
 */
static void net_receive_mqtt_packets(NetTaskContext *ctx, TickType_t now)
{
    // 非阻塞读一次 TCP 数据
    ctx->packet_len = transport_getdatanb(NULL, mqtt_recv_buf, sizeof(mqtt_recv_buf));
    if (ctx->packet_len <= 0) return; // 没数据直接退出

    ctx->last_rx_tick = now; // 收到数据 = 链路活着，刷新保活计时
    int offset = 0;          // 当前解析到缓冲区哪个位置（处理粘包）

    while (offset < ctx->packet_len) // 还有没解析完的包就继续
    {
        int rem_len = 0;     // MQTT 剩余长度（变长编码）
        int multiplier = 1;  // 变长编码乘数
        int i = 1;           // 从第 1 字节开始读剩余长度（跳过第 0 字节的类型头）
        int packet_len = 0;  // 当前这一个包的总长度
        unsigned char* curr_buf = mqtt_recv_buf + offset; // 指向当前包起点

        if (ctx->packet_len - offset < 2) break; // 剩余不足一个最小包（2 字节），等下次

        // MQTT 剩余长度变长编码算法：每个字节 bit7 是延续位，低 7 位是数值
        do {
            if (i >= (ctx->packet_len - offset)) { packet_len = 0; break; } // 数据被截断，包不完整
            unsigned char c = curr_buf[i++];
            rem_len += (c & 127) * multiplier;
            multiplier *= 128;
            if (multiplier > 2097152) { packet_len = 0; break; } // 超过 MQTT 最大长度，数据损坏
            if ((c & 128) == 0) { // 延续位为 0 = 长度编码结束
                packet_len = i + rem_len; // 包总长 = 头部长度 + 剩余长度
                break;
            }
        } while (1);

        if (packet_len <= 0 || packet_len > (ctx->packet_len - offset)) break; // 包没收全（粘包分片），留在下次

        // 尝试按 PUBLISH（云下发指令）解析
        if (MQTTDeserialize_publish(&ctx->dup, &ctx->qos, &ctx->retained, &ctx->packet_id, &ctx->topic_string,
                                    &ctx->payload, &ctx->payload_len, curr_buf, packet_len))
        {
            printf("MQTT Recv: Topic=%.*s, Payload=%.*s\r\n",
                   ctx->topic_string.lenstring.len, ctx->topic_string.lenstring.data,
                   ctx->payload_len, ctx->payload);

            // 交给业务层处理（解析 JSON、控制外设、触发 OTA）
            data_process_mqtt_msg(ctx->payload, ctx->payload_len);

            // QoS > 0 时需要回复 PUBACK 确认收到
            if (ctx->qos > 0)
            {
                int ack_len = MQTTSerialize_ack(mqtt_send_buf, sizeof(mqtt_send_buf), PUBACK, 0, ctx->packet_id);
                if (ack_len > 0) transport_sendPacketBuffer(net_state.sock, mqtt_send_buf, ack_len);
            }
        }
        // 不是 PUBLISH，试试是不是 SUBACK（订阅确认）
        else if (MQTTDeserialize_suback(&ctx->packet_id, 1, &ctx->sub_count, ctx->granted_qos, curr_buf, packet_len))
        {
            printf("MQTT Subscribe ACK Received.\r\n");
        }
        else
        {
            // 其它类型：看首字节高 4 位区分报文类型
            uint8_t pkt_type = curr_buf[0] >> 4;

            if (pkt_type == 13) { // 0x0D = PINGRESP，心跳回复 → 闭环
                ctx->waiting_pingresp = 0;
                ctx->last_rx_tick = now;
            } else if (pkt_type == 9) { // 0x09 = SUBACK 兜底
                printf("MQTT SUBACK Received\r\n");
            } else {
                printf("MQTT RX Ignored (Type=%d, Len=%d)\r\n", pkt_type, packet_len); // 其它（如 PUBACK）先忽略
            }
        }
        offset += packet_len; // 跳到下一个包
    }
}

/**
 * @brief       处理 ESP8266 透传出来的非 MQTT 数据（AT 响应等）
 * @param[in,out] ctx: 接收 rx_len 和 uart_buf
 * @note        非阻塞取 UART 队列；目前只打印，不做业务处理
 */
static void net_handle_uart_rx(NetTaskContext *ctx)
{
    if (xQueueReceive(xUartRxQueue, &ctx->rx_len, 0) != pdTRUE) return; // 没消息立即返回，不阻塞

    ctx->uart_buf = atk_mw8266d_uart_rx_get_frame();

    if (ctx->uart_buf != NULL)
    {
        printf("UART Msg: %s", ctx->uart_buf); // 打印收到的底层信息

        // 重启 UART 接收，放行下一批数据
        atk_mw8266d_uart_rx_restart();
    }
}

/**
 * @brief       执行 OTA 升级流程（由 net_task 主循环在 g_ota_request 置位时调用）
 * @param[in,out] ctx: 断开 MQTT 连接时使用
 * @note        升级期间暂停本任务的看门狗喂狗位，防止升级耗时长被误复位。
 *              成功升级会内部重启系统；失败则恢复喂狗并继续正常流程。
 */
static void net_handle_ota(NetTaskContext *ctx)
{
    printf("Enter OTA , stop NET watchdog assessment to avoid miskill...\r\n");

    extern volatile uint32_t wd_expected_mask;
    taskENTER_CRITICAL();
    wd_expected_mask &= ~WD_BIT_NET; // 升级期间不要求本任务喂狗
    taskEXIT_CRITICAL();

    // 退出透传模式，断开 MQTT，让资源让位给 HTTP 下载
    printf("Exiting MQTT transparent mode for OTA...\r\n");
    atk_mw8266d_exit_unvarnished();
    net_disconnect(ctx);

    // 阻塞执行 OTA 升级（成功后内部会重启系统）
    OTA_ProcessUpgrade();

    // 能跑到这里 = OTA 失败或取消，恢复喂狗并继续正常流程
    taskENTER_CRITICAL();
    wd_expected_mask |= WD_BIT_NET;
    taskEXIT_CRITICAL();

    g_ota_request = 0;
    printf("OTA Failed. Core reset Net Request. Reconnecting to MQTT...\r\n");
}

/**
 * @brief       FreeRTOS 网络任务主循环
 * @param[in]   pv: 任务参数（未使用）
 * @note        每轮循环依次执行：喂狗、按键、统计、OTA、重连、缓存离线数据、
 *              补发积压数据、心跳保活、实时上报、收包解析、UART 旁路处理
 */
void net_task(void *pv)
{
    NetTaskContext ctx;  // 任务私有上下文（栈上分配）
    net_ctx_init(&ctx);  // 初始化默认参数

    while (1)
    {
        // 1. 喂看门狗：证明本任务还活着
        wd_heartbeat(WD_BIT_NET);

        // 2. 读按键状态（非阻塞）
        ctx.key = KEY_Scan(0);

        TickType_t now = xTaskGetTickCount(); // 当前系统滴答

        uint8_t request_connect = 0; // 按键请求强制连接标志

        // 3. 每 5 秒打印一次统计
        net_print_stats(&ctx, now);

        // 4. 处理按键（连接/断开）
        net_handle_key(&ctx, now, &request_connect);

        // 5. 有 OTA 请求 → 执行升级（期间暂停本任务喂狗，防止误复位）
        // if (g_ota_request)
        // {
        //     net_handle_ota(&ctx);
        // }

        // 6. 未连接则尝试连接（WiFi + TCP + MQTT，带退避）
        if (!net_try_connect(&ctx, now, request_connect)) // 连接不成功
        {
            net_cache_offline_data(&ctx);  // 断网期间：传感器数据存 Flash
            vTaskDelay(pdMS_TO_TICKS(10));
            continue; // 未连上，跳过下面的收发逻辑
        }

        // 7. 已连上：先把 Flash 里积压的离线数据补发完
        if (!net_publish_flash_backlog(&ctx)) // 补发时断线了
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue; // 回到重连流程
        }

        // 8. 心跳保活 + 断线检测
        net_keepalive_step(&ctx, now);

        // 9. 有新的传感器数据 → 实时上报
        net_publish_realtime_data(&ctx);

        // 10. 接收并解析云平台下发的报文
        net_receive_mqtt_packets(&ctx, now);

        // 11. 处理 ESP8266 透传的非 MQTT 数据
        net_handle_uart_rx(&ctx);

        // 每轮小延时，让出 CPU 给低优先级任务
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

