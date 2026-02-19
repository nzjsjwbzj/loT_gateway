#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "led.h"
#include "key.h"
#include "lcd.h"
#include "sdram.h"
#include "dht11.h"
#include "pcf8574.h"
#include "ap3216c.h"
#include "atk_mw8266d.h"
#include "atk_mw8266d_uart.h"
#include "cJSON.h"

// MQTT相关头文件
#include "MQTTPacket.h"
#include "MQTTConnect.h"
#include "MQTTPublish.h"
#include "MQTTSubscribe.h"
#include "transport.h"

#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
/************************************************
 开发板: ALIENTEK 阿波罗 STM32F429 开发板
 DHT11 / AP3216C 采集程序 HAL 库版
 技术论坛: www.openedv.com
 创建日期: ALIENTEK
************************************************/
#define DEMO_WIFI_SSID          "CMCC-X22b"
#define DEMO_WIFI_PWD           "gp24ghzf"
#define DEMO_ATKCLD_DEV_ID      "63152966615081879519"
#define DEMO_ATKCLD_DEV_PWD     "12345678"

// MQTT 服务器配置
// #define MQTT_BROKER_IP          "broker.emqx.io"
// 如果使用本地 Broker（如 Mosquitto），端口通常是 1883
#define MQTT_BROKER_IP          "192.168.1.28"
#define MQTT_BROKER_PORT        "1883"
#define MQTT_CLIENT_ID          "STM32_F429_Client_Test"
// 如果 Broker 需要用户名/密码，在这里配置
#define MQTT_USER_NAME          ""
#define MQTT_PASSWORD           ""
#define MQTT_TOPIC_PUB          "test/topic"
#define MQTT_TOPIC_SUB          "test/topic_sub"


/**
 * @brief       演示上传数据，或处理 ATK-MW8266D UART 数据
 * @param       is_atkcld: 0: 上传到 ATK-CLD
 *                         1: 处理 ATK-MW8266D UART 接收到的数据
 * @retval      无
 */
static void demo_upload_data(uint8_t is_atkcld)
{
    uint8_t *buf;
    
    if (is_atkcld == 1)
    {
        /* 获取 ATK-MW8266D UART 接收帧 */
        buf = atk_mw8266d_uart_rx_get_frame();
        if (buf != NULL)
        {
            printf("%s", buf);
            /* 重启 ATK-MW8266D UART 接收 */
            atk_mw8266d_uart_rx_restart();

        }
    }
}

typedef struct 
{
    u8 temperature;
    u8 humidity;
} DHT11_Data_t;

typedef struct{
    u16 ir;
    u16 ps;
    u16 als;
} AP3216C_Data_t;

// 任务优先级
#define START_TASK_PRIO  1
#define INIT_TASK_PRIO  5
#define DHT11_TASK_PRIO  3
#define AP3216C_TASK_PRIO  3
#define LCD_TASK_PRIO 2
#define NET_TASK_PRIO 4
#define RUN_TIME_STATS_TASK_PRIO 2
#define DATA_PROCESS_TASK_PRIO 4  // 数据处理任务优先级

// 任务堆栈大小
#define START_STK_SIZE 128
#define INIT_STK_SIZE 128
#define DHT11_STK_SIZE 128
#define AP3216C_STK_SIZE 128
#define LCD_STK_SIZE 128
#define NET_STK_SIZE 500
#define RUN_TIME_STATS_STK_SIZE 256
#define DATA_PROCESS_STK_SIZE 256 // 数据处理任务堆栈大小

// 任务句柄
TaskHandle_t StartTask_Handler;
TaskHandle_t InitTask_Handler;
TaskHandle_t DHT11Task_Handler;
TaskHandle_t AP3216CTask_Handler;
TaskHandle_t LCDTask_Handler;
TaskHandle_t NETTask_Handler;
TaskHandle_t RunTimeStatsTask_Handler;
TaskHandle_t DataProcessTask_Handler; // 数据处理任务句柄

// 函数声明
void start_task(void *pv);
void init_task(void *pv);
void dht11_task(void *pv);
void ap3216c_task(void *pv);
void lcd_task(void *pv);
void net_task(void *pv);
void run_time_stats_task(void *pv);
void data_process_task(void *pv); // 数据处理任务

// 队列句柄
QueueHandle_t xDHT11Queue;
QueueHandle_t xAP3216CQueue;
QueueHandle_t xUartRxQueue; // UART 接收数据队列
SemaphoreHandle_t xLCDMutex;

u8 ret;
char ip_buf[16];
bool  link_status; // WiFi 连接状态
bool  con_status;  // MQTT 连接状态

// MQTT全局变量
static int mqtt_sock = -1;  // MQTT socket ID
static unsigned short mqtt_packet_id = 1;  // MQTT包ID
static unsigned char mqtt_send_buf[512];    // MQTT发送缓冲区
static unsigned char mqtt_recv_buf[512];    // MQTT接收缓冲区

TIM_HandleTypeDef htim2;

/**
 * @brief       配置 FreeRTOS 运行时统计时钟 (TIM2)
 *              TIM2 是 32 位定时器，适合作为运行时统计时基
 *              APB1 时钟为 45MHz，定时器时钟为 90MHz
 *              配置为 20kHz 频率 (50us 周期)，比系统节拍 (1kHz) 快 20 倍
 */
void ConfigureTimeForRunTimeStats(void)
{
    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};

    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance = TIM2;
    htim2.Init.Prescaler = 4499; // 90MHz / (4499+1) = 20kHz
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = 0xFFFFFFFF; // 32??????
    //htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    
    HAL_TIM_Base_Init(&htim2);
    
    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig);

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig);

    HAL_TIM_Base_Start(&htim2);
}

/**
 * @brief       ????????????????
 * @retval      ??????????
 */
uint32_t GetTimerCounterValue(void)
{
    return __HAL_TIM_GET_COUNTER(&htim2);
}

int main(void)
{
    HAL_Init();                     // 初始化 HAL 库
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    Stm32_Clock_Init(360,25,2,8);   // 设置系统时钟 180MHz
    delay_init(180);                // 初始化延时函数
    uart_init(115200);              // 初始化串口
    LED_Init();                     // 初始化 LED
    KEY_Init();                     // 初始化按键
    SDRAM_Init();                   // 初始化 SDRAM

    // 创建开始任务
    xTaskCreate(start_task,"start_task",START_STK_SIZE,NULL,START_TASK_PRIO,&StartTask_Handler);
    vTaskStartScheduler();  
    
    while(1);

    return 0;
}

void start_task(void *pv)
{
    // 创建队列和信号量
    xDHT11Queue = xQueueCreate(5, sizeof(DHT11_Data_t));
    xAP3216CQueue = xQueueCreate(5, sizeof(AP3216C_Data_t));
    xUartRxQueue = xQueueCreate(10, sizeof(uint16_t)); // UART 接收队列
    xLCDMutex = xSemaphoreCreateMutex();

    cJSON_Hooks hooks;
    hooks.malloc_fn = pvPortMalloc;
    hooks.free_fn = vPortFree;
    cJSON_InitHooks(&hooks);

    // 检查创建是否成功
    if (xDHT11Queue == NULL || xAP3216CQueue == NULL || xLCDMutex == NULL || xUartRxQueue == NULL) {
        printf("队列/信号量创建失败\r\n");
        // 可以在这里添加错误处理
    }

    // 创建其他任务
    xTaskCreate(init_task,   "init_task",   INIT_STK_SIZE, NULL, INIT_TASK_PRIO, &InitTask_Handler);
    xTaskCreate(dht11_task,  "dht11_task",  DHT11_STK_SIZE, NULL, DHT11_TASK_PRIO, &DHT11Task_Handler);
    xTaskCreate(ap3216c_task,"ap3216c_task",AP3216C_STK_SIZE, NULL, AP3216C_TASK_PRIO, &AP3216CTask_Handler);
    xTaskCreate(lcd_task,    "lcd_task",    LCD_STK_SIZE, NULL, LCD_TASK_PRIO, &LCDTask_Handler);
    xTaskCreate(net_task,    "net_task",    NET_STK_SIZE, NULL, NET_TASK_PRIO, &NETTask_Handler);
    xTaskCreate(run_time_stats_task, "stats_task", RUN_TIME_STATS_STK_SIZE, NULL, RUN_TIME_STATS_TASK_PRIO, &RunTimeStatsTask_Handler);
   // xTaskCreate(data_process_task,   "data_process", DATA_PROCESS_STK_SIZE, NULL, DATA_PROCESS_TASK_PRIO, &DataProcessTask_Handler);

    // 删除开始任务
    vTaskDelete(NULL);
}

void init_task(void *pv)
{
    u8 ret;
    char ip_buf[16];
    bool  link_status; // WiFi 连接状态
    bool  con_status;  // MQTT 连接状态

    LCD_Init();                     // LCD 初始化
    PCF8574_Init();                 // PCF8574 扩展IO初始化

    // 检查 PCF8574 扩展IO 是否正常，DHT11 初始化需要
    PCF8574_ReadBit(BEEP_IO);

    // 初始化 AP3216C，失败则 LCD 显示错误
    while(AP3216C_Init())
    {
        LCD_ShowString(30,190,200,16,16,"AP3216C Check Failed!");
        delay_xms(500);
        LCD_ShowString(30,190,200,16,16,"Please Check!        ");
        delay_xms(500);
    }    
    
    // 显示静态文本
    POINT_COLOR=BLUE;
    LCD_ShowString(30,150,200,16,16,"Temp:  C");    
    LCD_ShowString(30,170,200,16,16,"Humi:  %");
    
    POINT_COLOR=RED;
    LCD_ShowString(30,190,200,16,16,"AP3216C Ready!");  
    POINT_COLOR=BLUE;
    LCD_ShowString(30,220,200,16,16," IR:");     
    LCD_ShowString(30,250,200,16,16," PS:");    
    LCD_ShowString(30,280,200,16,16,"ALS:");

    printf("正在初始化 WiFi...\r\n");

    /* 初始化 ATK-MW8266D WiFi 模块 */
    ret = atk_mw8266d_init(115200);
    if (ret != 0)
    {
        printf("ATK-MW8266D error\r\n");

        while (1)
        {
            ret = atk_mw8266d_init(115200);
            if(ret==0){
                break;
            }
            else{
                printf("ATK-MW8266D error\r\n");

            }
            LED0=!LED0;
            delay_ms(200);
        }
    }
    
    printf("正在连接 AP...\r\n");
    ret  = atk_mw8266d_restore();                               /* 恢复出厂设置 */
    ret += atk_mw8266d_at_test();                               /* AT 测试 */
    ret += atk_mw8266d_set_mode(1);                             /* 设置 Station 模式 */
    ret += atk_mw8266d_sw_reset();                              /* 软件复位 */
    ret += atk_mw8266d_ate_config(0);                           /* 关闭回显 */
    ret += atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD);  /* 连接 WiFi */
    ret += atk_mw8266d_get_ip(ip_buf);                          /* 获取 IP 地址 */
    if (ret != 0)
    {
        printf("连接 AP 失败!\r\n");
        while (1)
        {
            LED0=!LED0;
            delay_ms(200);
        }
    }

    printf("IP: %s\r\n", ip_buf);
    LCD_ShowString(30,330,200,16,16, ip_buf);
    
    /* 重启 ATK-MW8266D UART 接收 */
    atk_mw8266d_uart_rx_restart();
    vTaskDelete(NULL);
}

void dht11_task(void *pv)
{
    DHT11_Data_t dht11_data; 
    
    while (1)
    {
        // // 检查 PCF8574 扩展 IO，确保 DHT11 所需的 IO 正常
        //  PCF8574_ReadBit(BEEP_IO);

        // // 读取 DHT11 温湿度数据
        //  DHT11_Read_Data(&dht11_data.temperature,&dht11_data.humidity);
    
        // // 发送到队列
        //  xQueueSend(xDHT11Queue,&dht11_data,0);

        // demo_upload_data(1); // 已在 net_task 中处理 ATK-MW8266D UART 数据

        vTaskDelay(pdMS_TO_TICKS(2000)); // 延时 2 秒，避免 MQTT 发送过于频繁
    }
}

void ap3216c_task(void *pv)
{
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        // 读取 AP3216C 光照、红外、距离数据
        AP3216C_ReadData(&ap3216c_data.ir,&ap3216c_data.ps,&ap3216c_data.als);

        // 发送到队列
        xQueueSend(xAP3216CQueue,&ap3216c_data,0);

        vTaskDelay(pdMS_TO_TICKS(2000)); // 延时 2 秒
    }
}

void lcd_task(void *pv)
{
    DHT11_Data_t dht11_data;
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        // 接收 DHT11 数据并显示在 LCD
        if(xQueueReceive(xDHT11Queue,&dht11_data,pdMS_TO_TICKS(100))==pdTRUE){
             if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                 // 显示温度
                 LCD_Fill(30+40, 150, 30+40+16, 150+16, WHITE);
                 LCD_ShowNum(30+40, 150, dht11_data.temperature, 2, 16);
                 
                 // 显示湿度
                 LCD_Fill(30+40, 170, 30+40+16, 170+16, WHITE);
                 LCD_ShowNum(30+40, 170, dht11_data.humidity, 2, 16);
                 
                 xSemaphoreGive(xLCDMutex);
             }
        }

        // 接收 AP3216C 数据并显示在 LCD
        if(xQueueReceive(xAP3216CQueue, &ap3216c_data, pdMS_TO_TICKS(100)) == pdTRUE) {
            if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // 显示 IR 值
                LCD_Fill(30+32, 220, 30+32+40, 220+16, WHITE);
                LCD_ShowNum(30+32, 220, ap3216c_data.ir, 5, 16);
                
                // 显示 PS 值
                LCD_Fill(30+32, 250, 30+32+40, 250+16, WHITE);
                LCD_ShowNum(30+32, 250, ap3216c_data.ps, 5, 16);
                
                // 显示 ALS 值
                LCD_Fill(30+32, 280, 30+32+40, 280+16, WHITE);
                LCD_ShowNum(30+32, 280, ap3216c_data.als, 5, 16);
                
                xSemaphoreGive(xLCDMutex);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));  // 延时 50ms
    }
}

void net_task(void *pv)
{
    DHT11_Data_t dht11_data;
    AP3216C_Data_t ap3216c_data;
    bool link_status;
    bool con_status = 0;
    u8 key;
    char json_buf[256];

    uint16_t rx_len;
    uint8_t *buf;
    
    // MQTT连接参数
    MQTTPacket_connectData connect_data = MQTTPacket_connectData_initializer;
    unsigned char sessionPresent;
    unsigned char connack_rc;
    int len;
    MQTTString topicString = MQTTString_initializer;
    MQTTString subscribe_topic = MQTTString_initializer;
    int grantedQoS[1];
    int sub_count;
    unsigned short sub_packetid = 1;
    
    // 解析MQTT数据包变量
    unsigned char dup;
    int qos;
    unsigned char retained;
    unsigned short packetid;
    unsigned char *payload;
    int payloadlen;

    while (1)
    {
        key = KEY_Scan(0);

        switch (key)
        {
            case KEY0_PRES: // 连接MQTT Broker
                printf("Connecting to MQTT Broker...\r\n");
                
                // 0. 连接前先ping
                atk_mw8266d_ping(MQTT_BROKER_IP);

                // 1. 建立TCP连接并进入透传模式
                mqtt_sock = transport_open(MQTT_BROKER_IP, atoi(MQTT_BROKER_PORT));
                if (mqtt_sock < 0)
                {
                    printf("Transport Open Failed!\r\n");
                    con_status = 0;
                    break;
                }
                printf("TCP Connected, Entered Transparent Mode.\r\n");
                
                delay_ms(500); // 延时等待透传就绪
                
                // 2. 填充MQTT CONNECT包
                connect_data.MQTTVersion = 4; // MQTT 3.1.1
                connect_data.clientID.cstring = MQTT_CLIENT_ID;
                connect_data.keepAliveInterval = 60;
                connect_data.cleansession = 1;
                connect_data.willFlag = 0;
                
                // 用户名和密码
                if (strlen(MQTT_USER_NAME) > 0)
                {
                    connect_data.username.cstring = MQTT_USER_NAME;
                }
                if (strlen(MQTT_PASSWORD) > 0)
                {
                    connect_data.password.cstring = MQTT_PASSWORD;
                }
                
                // 3. 序列化并发送CONNECT包
                len = MQTTSerialize_connect(mqtt_send_buf, sizeof(mqtt_send_buf), &connect_data);
                if (len <= 0)
                {
                    printf("MQTT Connect Serialize Failed!\r\n");
                    transport_close(mqtt_sock);
                    mqtt_sock = -1;
                    con_status = 0;
                    break;
                }
                
                if (transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, len) != len)
                {
                    printf("MQTT Connect Send Failed!\r\n");
                    transport_close(mqtt_sock);
                    mqtt_sock = -1;
                    con_status = 0;
                    break;
                }
                
                // 4. 等待CONNACK包
                len = MQTTPacket_read(mqtt_recv_buf, sizeof(mqtt_recv_buf), transport_getdata);
                if (len > 0)
                {
                    if (MQTTDeserialize_connack(&sessionPresent, &connack_rc, mqtt_recv_buf, len))
                    {
                        if (connack_rc == MQTT_CONNECTION_ACCEPTED)
                        {
                            printf("MQTT Connected!\r\n");
                            con_status = 1;
                            
                            // 5. 订阅主题
                            subscribe_topic.cstring = MQTT_TOPIC_SUB;
                            int reqQoS[1] = {0};
                            len = MQTTSerialize_subscribe(mqtt_send_buf, sizeof(mqtt_send_buf), 0, sub_packetid++, 1, &subscribe_topic, reqQoS);
                            if (len > 0)
                            {
                                transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, len);
                                printf("MQTT Subscribe Sent.\r\n");
                            }
                        }
                        else
                        {
                            printf("MQTT Connect Rejected: %d\r\n", connack_rc);
                            transport_close(mqtt_sock);
                            mqtt_sock = -1;
                            con_status = 0;
                        }
                    }
                    else
                    {
                        printf("MQTT Connack Deserialize Failed!\r\n");
                        transport_close(mqtt_sock);
                        mqtt_sock = -1;
                        con_status = 0;
                    }
                }
                else
                {
                    printf("MQTT Connack Receive Timeout!\r\n");
                    transport_close(mqtt_sock);
                    mqtt_sock = -1;
                    con_status = 0;
                }
                break;

            case KEY1_PRES: // 断开MQTT
                if (mqtt_sock >= 0 && con_status)
                {
                    // 发送DISCONNECT包
                    len = MQTTSerialize_disconnect(mqtt_send_buf, sizeof(mqtt_send_buf));
                    if (len > 0)
                    {
                        transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, len);
                    }
                    
                    // 关闭连接
                    transport_close(mqtt_sock);
                    mqtt_sock = -1;
                    con_status = 0;
                    printf("MQTT Disconnected!\r\n");
                }
                break;

            default:
                break;
        }

        if (!con_status || mqtt_sock < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // 获取 AP3216C 数据并发布到 MQTT
        if (xQueueReceive(xAP3216CQueue, &ap3216c_data, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "als", ap3216c_data.als);
            cJSON_AddNumberToObject(root, "ir", ap3216c_data.ir);
            cJSON_AddNumberToObject(root, "ps", ap3216c_data.ps);

            if (!cJSON_PrintPreallocated(root, json_buf, sizeof(json_buf), 0))
            {
                printf("JSON Print Failed!\r\n");
            }
            cJSON_Delete(root);

            // 填充 MQTT 发布消息
            topicString.cstring = MQTT_TOPIC_PUB;
            len = MQTTSerialize_publish(mqtt_send_buf, sizeof(mqtt_send_buf), 0, 0, 0, 0, 
                                        topicString, (unsigned char*)json_buf, strlen(json_buf));
            if (len > 0)
            {
                if (transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, len) == len)
                {
                    printf("MQTT Pub AP3216C OK\r\n");
                }
                else
                {
                    printf("MQTT Pub Send Failed!\r\n");
                }
            }
            else
            {
                printf("MQTT Pub Serialize Failed!\r\n");
            }
        }
        
        // 检查 MQTT 接收缓冲区数据
        len = transport_getdatanb(NULL, mqtt_recv_buf, sizeof(mqtt_recv_buf));
        if (len > 0)
        {
            // 尝试解析 PUBLISH 包
            if (MQTTDeserialize_publish(&dup, &qos, &retained, &packetid, &topicString, 
                                        &payload, &payloadlen, mqtt_recv_buf, len))
            {
                // 打印接收到的消息
                printf("MQTT Recv: Topic=%.*s, Payload=%.*s\r\n", 
                       topicString.lenstring.len, topicString.lenstring.data,
                       payloadlen, payload);
                
                // 如果 QoS>0，需要回复 PUBACK
                if (qos > 0)
                {
                    len = MQTTSerialize_ack(mqtt_send_buf, sizeof(mqtt_send_buf), PUBACK, 0, packetid);
                    if (len > 0)
                    {
                        transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, len);
                    }
                }
            }
            // 尝试解析 SUBACK 包
            else if (MQTTDeserialize_suback(&packetid, 1, &sub_count, grantedQoS, mqtt_recv_buf, len))
            {
                printf("MQTT Subscribe ACK Received.\r\n");
            }
        }
        
        // 检查 UART 接收队列（其他数据）
        if (xQueueReceive(xUartRxQueue, &rx_len, 0) == pdTRUE)
        {
            buf = atk_mw8266d_uart_rx_get_frame();
            if (buf != NULL)
            {
                // 打印其他 UART 消息
                printf("UART Msg: %s", buf);
                atk_mw8266d_uart_rx_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief       FreeRTOS 运行时统计任务
 *              获取任务运行时间信息，打印CPU使用率
 */
void run_time_stats_task(void *pv)
{
    char pcWriteBuffer[512];
    
    while (1)
    {
        
        vTaskGetRunTimeStats(pcWriteBuffer);

        printf("TaskName\tAbsTime\t\tTime%%\r\n%s\r\n", pcWriteBuffer);
        
        // 每 5 秒打印一次
        vTaskDelay(pdMS_TO_TICKS(5000));

    }
}

/**
 * @brief       数据处理任务 (预留)
 *              用于处理 UART 接收到的数据，实现消息泵机制
 */

void data_process_task(void *pv)
{
    uint16_t rx_len;
    uint8_t *buf;

    while (1)
    {
        // 可以在这里处理 UART 数据队列中的数据 (类似于demo_upload_data的功能)
       

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}