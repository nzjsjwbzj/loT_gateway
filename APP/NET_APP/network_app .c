#include "network_app.h"
#include "protocol_onenet.h" // 引入云端协议分析处理模块
#include "key.h"
#include "atk_mw8266d_uart.h"
#include "atk_mw8266d.h"


// MQTT相关全局变量
static int mqtt_sock = -1;  // MQTT socket ID
static unsigned short mqtt_packet_id = 1;  // MQTT包ID
static unsigned char mqtt_send_buf[512];    // MQTT发送缓冲区
static unsigned char mqtt_recv_buf[512];    // MQTT接收缓冲区
volatile uint8_t g_ota_request = 0;
volatile uint8_t g_mqtt_connected = 0;
bool  link_status; // WiFi 链路状态
bool  con_status;  // MQTT 连接状态
char ip_buf[16];   // IP 地址缓冲区
/**
 * @brief       初始化网络任务上下文结构体
 * @param[out]  ctx: 指向网络任务上下文结构体的指针，将被清零并赋予默认初始值
 * @note        此函数设定了 MQTT 断线重连的初始退避时间、最大退避时间，
 *              以及心跳保活(Ping)的周期间隔和超时阈值。同时对 Paho MQTT 
 *              的结构体进行标准的初始化填充。
 */
void net_ctx_init(NetTaskContext *ctx)
{
    memset(ctx, 0, sizeof(NetTaskContext));
    ctx->backoff_ms = 1000;
    ctx->max_backoff_ms = 32000;
    ctx->ping_interval_tick = pdMS_TO_TICKS(5000);
    ctx->ping_timeout_tick = pdMS_TO_TICKS(2000);
    
    // 使用临时变量初始化复杂结构体
    MQTTPacket_connectData temp_connect = MQTTPacket_connectData_initializer;
    memcpy(&ctx->connect_data, &temp_connect, sizeof(MQTTPacket_connectData));
    
    MQTTString temp_string = MQTTString_initializer;
    memcpy(&ctx->topic_string, &temp_string, sizeof(MQTTString));
    memcpy(&ctx->subscribe_topic, &temp_string, sizeof(MQTTString));
    
    ctx->sub_packet_id = 1;
}

/**
 * @brief       定时打印网络和离线存储的统计信息
 * @param[in]   ctx: 指向网络任务上下文结构体的指针，包含统计计数
 * @param[in]   now: 当前系统滴答时钟时间
 * @note        每隔 5 秒通过串口打印一次当次开机以来的离线数据缓存总数、
 *              成功补传的总数、以及当前 Flash 中待发送的遗留项个数。
 */
static void net_print_stats(NetTaskContext *ctx, TickType_t now)
{
    if ((now - ctx->last_report_tick) > pdMS_TO_TICKS(5000))
    {
        printf("cache_total=%lu, sent_total=%lu, pending=%lu\r\n",
               ctx->cached_total, ctx->sent_total, (uint32_t)flash_valid_count);
        ctx->last_report_tick = now;
    }
}

/**
 * @brief       构建 MQTT 连接配置报文的参数体
 * @param[out]  ctx: 指向网络任务上下文结构体的指针，其中的 connect_data 成员将被填充
 * @note        根据硬件要求和宏定义填充 MQTT 版本(V4)、Client ID、KeepAlive (60s)、
 *              清空会话标志(CleanSession) 以及访问控制的用户名和密码。
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
 * @brief       处理外部按键事件，用于控制网络的连接与断开
 * @param[in,out] ctx: 指向网络任务上下文结构体的指针，用于设置重连标志和退避参数
 * @param[in]   now: 当前系统滴答时钟时间
 * @param[out]  request_connect: 传出标志，若为 1 则表示用户请求立即发起连接
 * @note        - 按下 KEY0: 开启自动重连，并请求立即尝试连接 MQTT Broker。
 *              - 按下 KEY1: 若当前处于连接状态，则发送 Disconnect 报文、切断 TCP 
 *                并清空重连等待时长状态，以启动全新的重连周期。
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
        case KEY1_PRES:
            if (mqtt_sock >= 0 && con_status)
            {
                ctx->packet_len = MQTTSerialize_disconnect(mqtt_send_buf, sizeof(mqtt_send_buf));
                if (ctx->packet_len > 0)
                {
                    transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len);
                }
                transport_close(mqtt_sock);
                mqtt_sock = -1;
                con_status = 0;
                g_mqtt_connected = 0;
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

/**
 * @brief       尝试建立网络与 MQTT 服务器的连接
 * @param[in,out] ctx: 网络任务上下文指针，用于管理重连逻辑和计时器
 * @param[in]   now: 当前的系统滴答时钟(TickTock)
 * @param[in]   request_connect: 外部请求立刻连接的标志（如按键触发）
 * @retval      uint8_t: 1 表示连接成功或已连接；0 表示当前未连接
 * @note        此函数包含了 Wi-Fi 自动重连、传输层 (TCP) 打开、
 *              MQTT CONNECT 报文发送及 CONNACK 校验的完整业务流。
 */
static uint8_t net_try_connect(NetTaskContext *ctx, TickType_t now, uint8_t request_connect)
{
    if (con_status && mqtt_sock >= 0) return 1; // 如果已经处于连接状态且 socket 正常，直接返回 1
    g_mqtt_connected = 0;                       // 标记全局 MQTT 连接状态为未连接
    uint8_t can_try = request_connect;          // 初始化是否允许尝试连接的标志为外部请求标志
    if (!can_try && ctx->auto_reconnect)        // 如果外部没有强制要求，但开启了自动重连
    {
        if (ctx->next_retry_tick == 0 || now >= ctx->next_retry_tick) // 如果没有设置重连时间或者已经到达下一次重连时间
        {
            can_try = 1;                        // 允许尝试
        }
    }
    if (!can_try) return 0;                     // 如果仍然不允许尝试，退出并返回 0

    if (ctx->wifi_rejoin_needed && (ctx->wifi_retry_tick == 0 || now >= ctx->wifi_retry_tick)) // 若需要重新加入WiFi且满足重试时间
    {
        uint8_t join_ret = atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD); // 尝试连接设定的 AP 热点
        if (join_ret == ATK_MW8266D_EOK)        // 如果 Wi-Fi 加入成功
        {
            ctx->wifi_rejoin_needed = 0;        // 清除 Wi-Fi 重连标志
            ctx->wifi_retry_tick = 0;           // 清空下一次 Wi-Fi 重试时间
        }
        else                                    // 加入失败
        {
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000); // 设定下一次尝试加入 Wi-Fi 的时间为 5 秒后
            return 0;                           // 返回未连接
        }
    }
    if (atk_mw8266d_get_ip(ip_buf) != ATK_MW8266D_EOK) // 获取当前模块的 IP，检查是否合法或真正连上网络
    {
        uint8_t join_ret = atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD); // 如果没拿到IP，再次尝试加入 AP 热点
        if (join_ret != ATK_MW8266D_EOK)        // 如果还是失败
        {
            ctx->wifi_rejoin_needed = 1;        // 标记需要重新连接 Wi-Fi
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000); // 设定下一次尝试时间为 5 秒后
            return 0;                           // 返回未连接
        }
    }
    atk_mw8266d_uart_rx_restart();              // 重新启动 ATK 模块的串口接收（清理旧数据）
    mqtt_sock = transport_open(MQTT_BROKER_IP, atoi(MQTT_BROKER_PORT)); // 打开底层传输套接字，连接到 MQTT Broker 的 IP 和端口
    if (mqtt_sock < 0)                          // 如果 TCP 连接建立失败
    {
        printf("Transport Open Failed!\r\n");   // 打印打开传输层失败信息
        con_status = 0;                         // 连接状态置零
        ctx->wifi_rejoin_needed = 1;            // 标记可能 Wi-Fi 不稳定，需要重连
        ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000); // 退避 5 秒
    }
    else                                        // TCP 连接建立成功
    {
        printf("TCP Connected, Entered Transparent Mode.\r\n"); // 打印 TCP 连接成功（通常模块会进入透传模式）
        delay_ms(500);                          // 延时 500ms 等待模块状态稳定
        net_build_connect(ctx);                 // 构造 MQTT 连接配置相关数据结构
        ctx->packet_len = MQTTSerialize_connect(mqtt_send_buf, sizeof(mqtt_send_buf), &ctx->connect_data); // 将连接数据序列化到发送缓冲区中
        if (ctx->packet_len <= 0 || transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len) != ctx->packet_len) // 如果发送缓冲区出错或者发送字节数与预期不符
        {
            transport_close(mqtt_sock);         // 关闭 TCP 会话
            mqtt_sock = -1;                     // 重置套接字句柄
            con_status = 0;                     // MQTT 置为未连接状态
        }
        else                                    // 成功发出了 CONNECT 报文
        {
            ctx->packet_len = MQTTPacket_read(mqtt_recv_buf, sizeof(mqtt_recv_buf), transport_getdata); // 读取服务器的回复到接收缓冲区中
            if (ctx->packet_len > 0 && MQTTDeserialize_connack(&ctx->sessionPresent, &ctx->connack_rc, mqtt_recv_buf, ctx->packet_len)) // 如果读到数据并且能成功反序列化为 CONNACK 回复
            {
                if(ctx->connack_rc == MQTT_CONNECTION_ACCEPTED) // 分析返回码，如果服务端接受了连接
                {
                    printf("MQTT Connected!\r\n"); // 打印成功连接的日志
                    con_status = 1;             // 标识已接通
                    g_mqtt_connected = 1;       // 设置全局已连接标志
                    ctx->backoff_ms = 1000;     // 重置退避重连周期基数为 1000ms
                    ctx->next_retry_tick = 0;   // 清除重连定时
                    ctx->last_rx_tick = now;    // 刷新最后一次收到数据的系统 Tick
                    ctx->last_ping_tick = now;  // 刷新心跳包发送起点 Tick
                    ctx->waiting_pingresp = 0;  // 清除等待 PINGRESP 回复标志
                    ctx->subscribe_topic.cstring = MQTT_TOPIC_SUB; // 绑定要订阅的主题字符串
                    int req_qos[1] = {0};       // 请求的 QoS 服务质量层级，设为 0
                    ctx->packet_len = MQTTSerialize_subscribe(mqtt_send_buf, sizeof(mqtt_send_buf), 0, ctx->sub_packet_id++, 1, &ctx->subscribe_topic, req_qos); // 序列化订阅报文
                    if (ctx->packet_len > 0)    // 序列化成功
                    {
                        transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len); // 发送订阅请求
                        printf("MQTT Subscribe Sent.\r\n"); // 打印已发送订阅报文日志
                    }
                }
                else                            // 如果服务端拒绝了连接要求
                {
                    // 加上这句打印，看看到底是什么原因被踢下线！
                    printf("MQTT Connect Rejected by Server! RC=%d\r\n", ctx->connack_rc); // 打印被拒绝的原因状态码
                    transport_close(mqtt_sock); // 对方拒绝则主动关闭传输层
                    mqtt_sock = -1;             // 清除句柄资源
                    con_status = 0;             // 标记脱机
                    g_mqtt_connected = 0;       // 更新脱机全局标志
                }
            }
            else // 读取超时或者接收包错误
            {
                printf("MQTT Receive CONNACK Timeout or Error!\r\n"); // 提示未获取到回应
                transport_close(mqtt_sock);     // 关闭有异常底层的 socket
                mqtt_sock = -1;                 // 重置套接字句柄
                con_status = 0;                 // 标记脱机状态
                g_mqtt_connected = 0;           // 清理连接全局位
            }
        }
    }
    if (!con_status && ctx->auto_reconnect)     // 如果当前确认断连且系统处于要求自动重连的工作模式
    {
        if (ctx->backoff_ms < ctx->max_backoff_ms) ctx->backoff_ms <<= 1; // 增加下一次测试的退避延时（指数级）
        if (ctx->backoff_ms > ctx->max_backoff_ms) ctx->backoff_ms = ctx->max_backoff_ms; // 封顶上限不超 max_backoff_ms（32000毫秒）
        ctx->next_retry_tick = now + pdMS_TO_TICKS(ctx->backoff_ms); // 核算出下一次进行新一轮尝试滴答事件的卡点数值
    }
    return (con_status && mqtt_sock >= 0) ? 1 : 0; // 最后统一返回当前到底是否已经连接上、底层连接正常与否
}

/**
 * @brief       将离线/失败无法发出的数据缓存到外部 Flash
 * @param[in,out] ctx: 网络任务上下文指针，带有数据转换的缓存区
 * @note        此函数从 FreeRTOS 的传感器消息队列中获取数据，并在封装为 
 *              cJSON 字符串后安全压入环形 Flash 库内部。
 */
static void net_cache_offline_data(NetTaskContext *ctx)
{
    while (xQueueReceive(xAP3216CQueueForMQTT, &ctx->ap_data, 0) == pdTRUE) // 取出队列中的传感器消息对象，如果不空则进入循环进行抽取
    {
        cJSON *root = cJSON_CreateObject();     // 创建一个空的 JSON 根对象
        cJSON_AddNumberToObject(root, "als", ctx->ap_data.als); // 将传感器环境光强度 als 加成一个对象节点
        cJSON_AddNumberToObject(root, "ir", ctx->ap_data.ir);   // 追加红外 ir 参数结构节点于上节点平级的根基
        cJSON_AddNumberToObject(root, "ps", ctx->ap_data.ps);   // 追加近距 ps 参数节点到同一个根目标体对象内部
        if (cJSON_PrintPreallocated(root, ctx->json_buf, sizeof(ctx->json_buf), 0)) // 若把树转换打印序列化成字符输出至 buffer 成功无出界截断
        {
            // 数据入库，启用离线存储（不再忽略）
            flash_store_push_locked(ctx->json_buf, (uint16_t)strlen(ctx->json_buf)); // 使用上锁特性的保护措施压缓冲文体队列至 SPI FLASH 芯片内存储
            ctx->cached_total++;                // 开机以来的缓存入存储芯片动作总数计数器递加（累加）记录
            printf("Offline Data Cached To Flash! (cached_session_total=%lu, flash_pending=%lu)\r\n", 
                   ctx->cached_total, flash_valid_count); // 打印缓存消息以及现有系统中一共有多少存底数据等待发出的统计
        }
        cJSON_Delete(root);                     // 将刚才生成的 cJSON 链表结构在堆上销毁拆解清理、解绑内存条释放占用
    }
}

/**
 * @brief       重发存放在 Flash 中的过往因断网积压的数据记录
 * @param[in,out] ctx: 网络任务上下文，用其发报结构体缓冲区发包
 * @retval      uint8_t: 1 表示不需要重发或重发成功，可以做其他事情；0 表示由于链接失败断连导致重发中断。
 * @note        以 FIFO 先出模式取出记录投递，必须有真实 ACK 或顺利入 TCP 
 *              网络缓存才会将此条旧记录抹除以确保绝对传达到对端云服务。
 */
static uint8_t net_publish_flash_backlog(NetTaskContext *ctx)
{
    // 屏蔽掉重传Flash存余数据的逻辑，直接返回1表示无滞留数据
   // return 1;
    
    if (flash_valid_count == 0) return 1;       // 如果没有有效数据等待发送，则直接提早结束且报通过（1）
    uint16_t cached_len = 0;                    // 先初始化用来寄存被捞数据内容本身字节长宽标尺变量
    uint32_t pop_index = 0;                     // 将用来承载数据弹取头逻辑位号下标数字存储变量设定空档期原始基准 0
    if (!flash_store_peek_locked(ctx->json_buf, sizeof(ctx->json_buf), &cached_len, &pop_index)) return 1; // 偷偷带锁探查窥视队列前端的字块并将实体倒送入缓存。若有毛病则放弃此轮试水回归正常常态。
    printf("Read from Flash: len=%d, pending=%lu\r\n", cached_len, flash_valid_count); // 打入调试信息告诉使用者此刻从哪里拨拉出东西了，到底总数还有多大等待消化
    MQTTString pub_topic = MQTTString_initializer; // 在代码内存当中清空设定好用于指向要刊发上送主题地址对象的字符串类型构件
    pub_topic.cstring = MQTT_TOPIC_PUB;         // 用预先由 C 文件规定的对应设备 Publish 数据端点的常量字带赋值进去
    ctx->packet_len = MQTTSerialize_publish(mqtt_send_buf, sizeof(mqtt_send_buf), 0, 0, 0, 0,
                                            pub_topic, (unsigned char*)ctx->json_buf, cached_len); // 包装上端信息载体：整合 Topic、原始参数报文并进行网络适配转码打扁平，得出发送字宽
    if (ctx->packet_len > 0)                    // 当序列化转化出成效宽带值不是负数异常后
    {
        if (transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len) == ctx->packet_len) // 将组合包丢送给 tcp/套接驱动并且确认它成功吃进去所交代长度的实体封件以后进行肯定反馈流程分支
        {
            printf("Flash Data Sent OK (pending=%lu)\r\n", flash_valid_count); // 把投送捷报打到主控控制展示区表明工作成绩且公示结余剩余
            flash_store_mark_sent_locked(pop_index); // 把那个已经被偷跑走并且已经确实传送到位的扇出条目标记成为“废止不用保留态”，实际上是完成销毁出队列效果流程
            ctx->sent_total++;                  // 成功离线找补数据递交记录统计进行翻一加计计数
            return 1;                           // 报告本函数完成了自己的使命宣告，主线程可往下做活
        }
        printf("Send Flash Data Failed!\r\n");  // 如果驱动不吃不给回面，只能断定底层有问题打出投送翻车警示词语
        transport_close(mqtt_sock);             // 强制斩断现有因为卡壳的底层对接网络线路
        mqtt_sock = -1;                         // 回收并将那个标识连上对接网络的符号句柄初始化置于原位，拔网线效果
        con_status = 0;                         // 系统承认已经彻底失去网络交互资格的地位降格
        g_mqtt_connected = 0;                   // 跟外界声明大连接已经倒掉彻底罢工
        return 0;                               // 无奈向上给任务交一份未完成差事的中断退位票信条作为反馈交代
    }
    return 1;                                   // 假如仅仅是转化包装中出错就不强行拉闸切网，只退回报成功状态，寄希望下一个包件自身没事即可
    
}

/**
 * @brief       MQTT 心跳保活机制与超时检测步进函数
 * @param[in,out] ctx: 网络任务上下文指针，包含心跳状态和时间戳
 * @param[in]   now: 当前系统滴答时钟(Tick)时间
 * @note        采用动态心跳机制：只有当网络空闲（无下发数据）超过规定间隔时才发送 PINGREQ。
 *              发送后若在超时时间内未收到 PINGRESP，则判定为死连接，强制断开并触发重连。
 */
static void net_keepalive_step(NetTaskContext *ctx, TickType_t now)
{
    if (!(con_status && mqtt_sock >= 0)) return; // 如果当前未连接或者 socket 无效，则不需要保活，直接返回
    
    if (ctx->waiting_pingresp) // 状态分支1：如果之前已经发了心跳请求包，目前正在等服务器回复
    {
        if ((now - ctx->last_ping_tick) > ctx->ping_timeout_tick) // 校验等待时间：如果【当前时间 - 发心跳时间 > 超时容忍阈值(通常2秒)】
        {
            printf("MQTT Ping Timeout!\r\n");   // 认定为网络已掉线（假死），打印心跳超时告警
            transport_close(mqtt_sock);         // 强制关闭底层 TCP socket 链接
            mqtt_sock = -1;                     // 回收并重置 socket 句柄
            con_status = 0;                     // 宣告 MQTT 连接业务层断开
            ctx->waiting_pingresp = 0;          // 不再等待回包，复位标志位
            g_mqtt_connected = 0;               // 同步清理对外全局变量声明的接通状态
            ctx->wifi_rejoin_needed = 1;        // 怀疑是底层 Wi-Fi 断开引起，标记下一次需重扫/重连热点
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000); // 退避 5 秒后再做恢复动作
        }
    }
    // 状态分支2：没有在等待回包（处于正常空闲状态），则判断空闲时间
    // 为什么这样写？因为如果有业务数据收发，last_rx_tick会被刷新，这里就不会超限，可以省心跳流量（动态心跳机制）
    else if ((now - ctx->last_rx_tick) > ctx->ping_interval_tick) 
    {
        ctx->packet_len = MQTTSerialize_pingreq(mqtt_send_buf, sizeof(mqtt_send_buf)); // 开始组装 PINGREQ 请求封包放入发送缓存
        if (ctx->packet_len > 0 && transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len) == ctx->packet_len) // 若封配合法，连同调用 TCP 顺利发出
        {
            ctx->waiting_pingresp = 1; // 设置关卡锁扣：进入等服务器回填 PINGRESP 的状态
            ctx->last_ping_tick = now; // 盖个时间戳：以此卡点为起跑线，开始计死线超时
        }
        else // 如果序列化出毛病，或者刚要往底端打却发现 TCP 写不进了（可能已被对端断开或缓冲区死锁）
        {
            printf("MQTT Ping Send Failed!\r\n"); // 打印发心跳失败的提示
            transport_close(mqtt_sock);           // 同样当做不可用处理，直接断开网络
            mqtt_sock = -1;                       // 置空归位套接字
            con_status = 0;                       // 下线 MQTT 会话
            ctx->waiting_pingresp = 0;            // 复位等待标记
            g_mqtt_connected = 0;                 // 关闭暴露给其它任务的连接位灯信
            ctx->wifi_rejoin_needed = 1;          // 申请由 Wi-Fi 层从头开始重塑连接接驳
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000); // 冷却5秒以防无限重试风暴引起看门狗死锁
        }
    }
}
// {
//     "id": "123",
//     "version": "1.0",
//     "params": {
//         "als": { "value": 123 },
//         "ir": { "value": 45 },
//         "ps": { "value": 6 }
//     }
// }
static void net_publish_realtime_data(NetTaskContext *ctx)
{
    if (xQueueReceive(xAP3216CQueueForMQTT, &ctx->ap_data, pdMS_TO_TICKS(50)) != pdTRUE) return;
    
    // Create the OneNET standard 物模型 JSON object
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", "123");
    cJSON_AddStringToObject(root, "version", "1.0");
    
    cJSON *params = cJSON_CreateObject();
    
    // Add als
    cJSON *als_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(als_obj, "value", ctx->ap_data.als);
    cJSON_AddItemToObject(params, "als", als_obj);
    
    // Add ir
    cJSON *ir_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(ir_obj, "value", ctx->ap_data.ir);
    cJSON_AddItemToObject(params, "ir", ir_obj);
    
    // Add ps
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
        int sret = transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len);
        if (sret == ctx->packet_len)
        {
            printf("MQTT Pub AP3216C OK\r\n");
        }
        else
        {
            printf("MQTT Pub Send Failed!\r\n");
            if (con_status)
            {
                transport_close(mqtt_sock);
                mqtt_sock = -1;
                con_status = 0;
                g_mqtt_connected = 0;
            }
        }
    }
    else
    {
        printf("MQTT Pub Serialize Failed!\r\n");
    }
}

/**
 * @brief       接收并解析底端传来的 MQTT 报文数据
 * @param[in,out] ctx: 网络任务上下文指针，用于更新接收时间与等待状态
 * @param[in]   now:  当前系统滴答时钟时间
 * @note        以非阻塞方式读取网络下发的数据。负责处理可能出现的 TCP 粘包现象，
 *              并针对 PUBLISH（云端业务下发指令）、SUBACK（订阅完成返回）以及 
 *              PINGRESP（心跳返回）分类拆包与事件派发。
 */
static void net_receive_mqtt_packets(NetTaskContext *ctx, TickType_t now)
{
    // 非阻塞读取底层的网卡/TCP数据存储到接收缓冲区中，并获取真实收到的长度
    ctx->packet_len = transport_getdatanb(NULL, mqtt_recv_buf, sizeof(mqtt_recv_buf));
    if (ctx->packet_len <= 0) return; // 如果没有数据到达，直接退出不浪费 CPU 资源
    
    ctx->last_rx_tick = now; // 只要收到了网络上的合法报文包，说明链路通畅，刷新底线保活计时器
    int offset = 0;          // 定义游标偏移量，用于在接收数组里循环定位解析，防止一次收到多条“粘包”
    
    while (offset < ctx->packet_len) // 如果游标还没走到本次接收数据的末尾，说明还有包遗留未处理
    {
        int rem_len = 0;     // MQTT 的变长剩余长度 (Remaining Length) 变量
        int multiplier = 1;  // MQTT 变长编码的乘数
        int i = 1;           // 游标本地索引，跳过固定报头所在的第 0 字节，从剩余长度字节开始
        int packet_len = 0;  // 记录“当前正在解析的这一个单包”的整体实际长宽
        unsigned char* curr_buf = mqtt_recv_buf + offset; // 指针指向当前单包开始处的地址
        
        if (ctx->packet_len - offset < 2) break; // 如果剩余没处理的长度低于 2 个字节，构不成最小 MQTT 包也无法解析，强跳出
        
        // 此 Do-While 循环是 MQTT 取 Remaining Length (变长计算) 官方算法
        // 每个字节的高位(bit 7)用作延续位，低7位用作数值存放
        do {
            if (i >= (ctx->packet_len - offset)) { packet_len = 0; break; } // 拿到的包裹被截断，包体不完整
            unsigned char c = curr_buf[i++];   // 提取长度表示字节
            rem_len += (c & 127) * multiplier; // 抹除最高位得出当前数位上的真实权重增加
            multiplier *= 128;                 // 权值的基向上提升进位
            if (multiplier > 2097152) { packet_len = 0; break; } // 数据包超出了MQTT规格极大值，包存在破坏
            if ((c & 128) == 0) {              // 当读出此字节最高位为0，说明这个长度定义到此终结了
                packet_len = i + rem_len;      // 当前单包总长 = 消耗的报头字节(i) + 真实净荷长度(rem_len)
                break;
            }
        } while (1);
        
        if (packet_len <= 0 || packet_len > (ctx->packet_len - offset)) break; // 单体过长或异常说明当前包没收全(TCP粘包分片)，留在下一次接续
        
        // 尝试按照 Publish(应用云端下指令) 格式将其反序列化提取数据
        if (MQTTDeserialize_publish(&ctx->dup, &ctx->qos, &ctx->retained, &ctx->packet_id, &ctx->topic_string,
                                    &ctx->payload, &ctx->payload_len, curr_buf, packet_len))
        {
            // 如果解析通过，说明收到了服务器的数据命令推送，顺带用 printf 展示
            printf("MQTT Recv: Topic=%.*s, Payload=%.*s\r\n",
                   ctx->topic_string.lenstring.len, ctx->topic_string.lenstring.data,
                   ctx->payload_len, ctx->payload);
                   
            // 投递进入专门分离出来的业务解析中心(进行 cJSON 破冰拆解及控制外设)
            data_process_mqtt_msg(ctx->payload, ctx->payload_len);
            
            // 对通信质量做反馈：若协议要求强回应 QoS > 0，则回复告知服务器已收到 (PUBACK)
            if (ctx->qos > 0)
            {
                int ack_len = MQTTSerialize_ack(mqtt_send_buf, sizeof(mqtt_send_buf), PUBACK, 0, ctx->packet_id);
                if (ack_len > 0) transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ack_len);
            }
        }
        // 如果不是云端发的数据，试着解析是不是上次提交 Sub(订阅意向) 后的接纳认证回复报文
        else if (MQTTDeserialize_suback(&ctx->packet_id, 1, &ctx->sub_count, ctx->granted_qos, curr_buf, packet_len))
        {
            printf("MQTT Subscribe ACK Received.\r\n"); // 收到订阅的准许反馈
        }
        else 
        {
            // 对于其它类型（非下发、非Sub回信），做个更底层的控制码探照分类
            uint8_t pkt_type = curr_buf[0] >> 4; // MQTT报文种类的代号都在第一个 Byte 的高四位(移位提取)
            
            if (pkt_type == 13) {                // type 13 就是 0x0D -> 这正是 PINGRESP！！ 
                ctx->waiting_pingresp = 0;       // 关闭心跳遗失红色警报！说明心跳圆满闭环，链接健康
                ctx->last_rx_tick = now;         // 重置基准时钟，给下次长周期超时计时打提前量
            } else if (pkt_type == 9) {          // type 9 = 0x09 为 SUBACK 的兜底确认识别
                printf("MQTT SUBACK Received\r\n");
            } else {
                printf("MQTT RX Ignored (Type=%d, Len=%d)\r\n", pkt_type, packet_len); // 其他例如 PUBACK（发数据回应）先放过不管
            }
        }
        offset += packet_len; // 游标朝后挪动一包的距离。继续在 While 循环里解析 TCP 缓冲区可能多合一挂载过来的下一个短包
    }
}

/**
 * @brief       处理来自底层透传模块 (ATK_MW8266D) 的 UART 异步数据包
 * @param[in,out] ctx: 网络任务上下文指针，用于携带解析包长及数据体
 * @note        以非阻塞的方式监听 UART 队列，当发现有模块返回或透传
 *              的其他非标消息时获取并做后期业务切分。
 */
static void net_handle_uart_rx(NetTaskContext *ctx)
{
    // 探测队列：如果在该队列(xUartRxQueue)没有等到消息立马返回，绝不阻塞 MQTT 主轴心骨
    if (xQueueReceive(xUartRxQueue, &ctx->rx_len, 0) != pdTRUE) return;
    
    // 经过上一句保证队里有货后，提取底层的满帧指针串
    ctx->uart_buf = atk_mw8266d_uart_rx_get_frame();
    
    // 双重保险验证提取出的货并不是无效空指针
    if (ctx->uart_buf != NULL)
    {
        printf("UART Msg: %s", ctx->uart_buf); // 通过调试串口打印出收到的未知/底层信息

        // 【保留接口】后续可在此处挂载针对特定 UART 数据的解析器(例如 AT 响应或特殊传感器分包)
        // data_process_uart_msg(ctx->uart_buf, ctx->rx_len);

        // 重启清空底层串口驱动的接收 DMA或状态机，放行下一波 UART 中断大流进站
        atk_mw8266d_uart_rx_restart(); 
    }
}

/**
 * @brief       FreeRTOS 网络核心大循环任务入口 (The Network Task Thread)
 * @param[in]   pv: FreeRTOS 要求的参数座（通常填 NULL，此处未使用）
 * @note        由于担负着最高层级的外联互动，这是一个拥有绝对统筹权的长时轮询任务。
 *              内部包含了：看门狗喂狗、按键下探、OTA分发、连接试探维持、
 *              Flash离线滞留上报、动态心跳保卫、实时传感器采集上报与网卡数据接收调度。
 */
void net_task(void *pv)
{
    NetTaskContext ctx;    // 在此任务独立栈里开辟一个大结构体变量作为其本任务唯一操作主干
    net_ctx_init(&ctx);    // 进行第一次全面洗地重置和默认参数的设定起航

    // 无穷大轮询，因为是线程，绝对不能 return 或 break 到头
    while (1)
    {
        // 1. 系统底层保活篇：用本任务的生命给系统主看门狗点赞，证明网络线程没死锁卡毙
        wd_heartbeat(WD_BIT_NET);
         
        // 2. 环境感知篇：抓取当前最新物理按键的状态，0号位无阻塞读取
        ctx.key = KEY_Scan(0);
        
        // 记录一下当下起跑发车的系统滴答刻度（毫秒级对账基准）
        TickType_t now = xTaskGetTickCount();
        
        uint8_t request_connect = 0; // 单次循环里的强制连接申诉旗，默认放下

        // 3. 例行杂务篇：每隔5秒上报一次开机以来的统计信息
        net_print_stats(&ctx, now);
        
        // 4. 用户交互篇：看刚才采集到的按键有没有在针对本任务发号施令（开启重连 or 强杀连接）
        net_handle_key(&ctx, now, &request_connect);

        // 5. 紧急高优打断篇：系统触发了 OTA 的全面升级诉求
        if (g_ota_request)
        {
            if (mqtt_sock >= 0 && con_status) // 在开启空中升级前若自己还有连着老网，为了安全起见必须得“先下线"
            {
                ctx.packet_len = MQTTSerialize_disconnect(mqtt_send_buf, sizeof(mqtt_send_buf)); // 打包分手辞别信
                if (ctx.packet_len > 0)
                {
                    transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx.packet_len); // 送达服务器请求正规解约
                }
                transport_close(mqtt_sock);       // 断开TCP通道
                mqtt_sock = -1;                   // 清空凭证
                con_status = 0;                   // 网络挂失
                g_mqtt_connected = 0;             // 跨任务标志下线
            }
            g_ota_request = 0;                    // 吃掉OTA旗帜，表明我看见了并正在做
            OTA_ProcessUpgrade();                 // 启动庞大、耗时的代码刷写重加载机制
            ctx.next_retry_tick = now + pdMS_TO_TICKS(1000); // 如果OTA意外弹回了，冷却 1000ms 后再准许网络业务找回
            vTaskDelay(pdMS_TO_TICKS(20));        // 放弃自身CPU算力交出控制权片刻 为啥？
            continue;                             // 放弃当前圈剩下所有未执行步骤，重新进站检查
        }

        // 6. 网络拓扑接入篇：不断试探直到获取合法 MQTT Socket（重连退避算法包在内了）
        if (!net_try_connect(&ctx, now, request_connect)) // 如果本趟还是没能连通（如WiFi挂了或者退避中）
        {
            net_cache_offline_data(&ctx);         // 调用应急方案：这段时间有新传感器数据的话，直接存外部 SPI FLASH 当存底
            vTaskDelay(pdMS_TO_TICKS(10));        // 不能硬转圈烧 CPU，小睡 10 个系统滴答
            continue;                             // 在没拿到网络资格证之前，下面所有的上传、收信逻辑一概不应该进入
        }

        // 7. 离线数据回补/消化篇：网络既已通畅，先问Flash要陈年旧账发回天上
        if (!net_publish_flash_backlog(&ctx))     // 如果补发出了网络事故被强制退回（底端TCP断掉）
        {
            vTaskDelay(pdMS_TO_TICKS(10));        // 交出CPU切手
            continue;                             // 当前回合必须停止所有后备操作，重新进行下一轮尝试连网逻辑
        }

        // 8. 恒定状态维护篇：没有历史包袱的情况下，看看现在离上一次云端发话隔了多久，久了就上心脏激活起搏器
        net_keepalive_step(&ctx, now);
        
        // 9. 现充数据上报篇：检查有没有即时的当前时间片传来的传感器环境数据，拿来格式化发布出去
        net_publish_realtime_data(&ctx);
        
        // 10. 远端数据拆取篇：听云服务的话，把下达的控制单、回传报等解析处理落实在本地
        net_receive_mqtt_packets(&ctx, now);
        
        // 11. 透传/指令旁路篇：拾取串口剩余不知名帧碎片
        net_handle_uart_rx(&ctx);
        
        // 完美过关结束当前轮次，小延时防资源独霸卡死低优任务
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

