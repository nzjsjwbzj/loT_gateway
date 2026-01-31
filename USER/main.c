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

#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
/************************************************
 例程基于 ALIENTEK 开发板 STM32F429 适配移植
 DHT11 与 AP3216C 演示（基于 HAL 库）
 参考资料: www.openedv.com
 作者: ALIENTEK
************************************************/
#define DEMO_WIFI_SSID          "CMCC-X22b"
#define DEMO_WIFI_PWD           "gp24ghzf"
#define DEMO_ATKCLD_DEV_ID      "63152966615081879519"
#define DEMO_ATKCLD_DEV_PWD     "12345678"

// MQTT 代理地址示例
// #define MQTT_BROKER_IP          "broker.emqx.io"
// 使用本地 Broker（例如 Mosquitto 监听 1883）
#define MQTT_BROKER_IP          "192.168.1.28"
#define MQTT_BROKER_PORT        "1883"
#define MQTT_CLIENT_ID          "STM32_F429_Client_Test"
// 如果 Broker 要求用户名/密码可填写
#define MQTT_USER_NAME          ""
#define MQTT_PASSWORD           ""
#define MQTT_TOPIC_PUB          "test/topic"

/**
 * @brief       处理接收并打印 ATK-MW8266D UART 数据
 * @param       is_atkcld: 0: 不处理 ATK-CLD
 *                         1: 处理 ATK-MW8266D UART 收到的数据
 * @retval      无
 */
static void demo_upload_data(uint8_t is_atkcld)
{
    uint8_t *buf;
    
    if (is_atkcld == 1)
    {
        /* 获取 ATK-MW8266D UART 接收缓冲帧 */
        buf = atk_mw8266d_uart_rx_get_frame();
        if (buf != NULL)
        {
            printf("%s", buf);
            /* 处理后重启 UART 接收缓冲 */
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

// 任务栈大小（字）
#define START_STK_SIZE 128
#define INIT_STK_SIZE 128
#define DHT11_STK_SIZE 128
#define AP3216C_STK_SIZE 128
#define LCD_STK_SIZE 128
#define NET_STK_SIZE 500

// 任务句柄
TaskHandle_t StartTask_Handler;
TaskHandle_t InitTask_Handler;
TaskHandle_t DHT11Task_Handler;
TaskHandle_t AP3216CTask_Handler;
TaskHandle_t LCDTask_Handler;
TaskHandle_t NETTask_Handler;

// 任务函数原型
void start_task(void *pv);
void init_task(void *pv);
void dht11_task(void *pv);
void ap3216c_task(void *pv);
void lcd_task(void *pv);
void net_task(void *pv);

// 队列与互斥量
QueueHandle_t xDHT11Queue;
QueueHandle_t xAP3216CQueue;
SemaphoreHandle_t xLCDMutex;

u8 ret;
char ip_buf[16];
bool  link_status; // WiFi 链路状态（示例变量）
bool  con_status;  // MQTT 连接状态（示例变量）

int main(void)
{
    HAL_Init();                     // 初始化 HAL 库
    Stm32_Clock_Init(360,25,2,8);   // 配置系统时钟，目标 180MHz
    delay_init(180);                // 延时初始化（按时钟）
    uart_init(115200);              // 串口初始化
    LED_Init();                     // LED 初始化
    KEY_Init();                     // 按键初始化
    SDRAM_Init();                   // SDRAM 初始化

    // 创建启动任务并开始调度
    xTaskCreate(start_task,"start_task",START_STK_SIZE,NULL,START_TASK_PRIO,&StartTask_Handler);
    vTaskStartScheduler();  
    
    while(1);

    return 0;
}

void start_task(void *pv)
{
    // 创建队列和互斥量
    xDHT11Queue = xQueueCreate(5, sizeof(DHT11_Data_t));
    xAP3216CQueue = xQueueCreate(5, sizeof(AP3216C_Data_t));
    xLCDMutex = xSemaphoreCreateMutex();

    // 检查资源创建是否成功
    if (xDHT11Queue == NULL || xAP3216CQueue == NULL || xLCDMutex == NULL) {
        printf("资源创建失败，队列或互斥量创建出错\r\n");
        // 这里可以根据需要处理错误（如重启或断言）
    }

    // 创建其他任务
    xTaskCreate(init_task,   "init_task",   INIT_STK_SIZE, NULL, INIT_TASK_PRIO, &InitTask_Handler);
    xTaskCreate(dht11_task,  "dht11_task",  DHT11_STK_SIZE, NULL, DHT11_TASK_PRIO, &DHT11Task_Handler);
    xTaskCreate(ap3216c_task,"ap3216c_task",AP3216C_STK_SIZE, NULL, AP3216C_TASK_PRIO, &AP3216CTask_Handler);
    xTaskCreate(lcd_task,    "lcd_task",    LCD_STK_SIZE, NULL, LCD_TASK_PRIO, &LCDTask_Handler);
    xTaskCreate(net_task,    "net_task",    NET_STK_SIZE, NULL, NET_TASK_PRIO, &NETTask_Handler);

    // 删除当前启动任务
    vTaskDelete(NULL);
}

void init_task(void *pv)
{
    u8 ret;
    char ip_buf[16];
    bool  link_status; // WiFi 链路状态（局部）
    bool  con_status;  // 连接状态（局部）

    LCD_Init();                     // LCD 初始化
    PCF8574_Init();                 // PCF8574 初始化（扩展 IO）

    // 读取 PCF8574 上某位以确保 I/O 已就绪（例如用于 DHT11 的引脚）
    PCF8574_ReadBit(BEEP_IO);

    // 初始化 AP3216C，失败时在 LCD 上提示并重试
    while(AP3216C_Init())
    {
        LCD_ShowString(30,190,200,16,16,"AP3216C Check Failed!");
        delay_xms(500);
        LCD_ShowString(30,190,200,16,16,"Please Check!        ");
        delay_xms(500);
    }    
    
    // 显示初始界面
    POINT_COLOR=BLUE;
    LCD_ShowString(30,150,200,16,16,"Temp:  C");    
    LCD_ShowString(30,170,200,16,16,"Humi:  %");
    
    POINT_COLOR=RED;
    LCD_ShowString(30,190,200,16,16,"AP3216C Ready!");  
    POINT_COLOR=BLUE;
    LCD_ShowString(30,220,200,16,16," IR:");     
    LCD_ShowString(30,250,200,16,16," PS:");    
    LCD_ShowString(30,280,200,16,16,"ALS:");

    printf("初始化中...\r\n");

    /* 初始化 ATK-MW8266D WiFi 模块 */
    ret = atk_mw8266d_init(115200);
    if (ret != 0)
    {
        printf("ATK-MW8266D 初始化失败!\r\n");
        while (1)
        {
            LED0=!LED0;
            delay_ms(200);
        }
    }
    
    printf("正在加入 AP...\r\n");
    ret  = atk_mw8266d_restore();                               /* 恢复出厂或初始化状态 */
    ret += atk_mw8266d_at_test();                               /* 测试 AT 指令 */
    ret += atk_mw8266d_set_mode(1);                             /* 设置为 Station 模式 */
    ret += atk_mw8266d_sw_reset();                              /* 软件复位模块 */
    ret += atk_mw8266d_ate_config(0);                           /* 关闭回显 */
    ret += atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD);  /* 连接 WiFi */
    ret += atk_mw8266d_get_ip(ip_buf);                          /* 获取分配的 IP */
    if (ret != 0)
    {
        printf("加入 AP 失败!\r\n");
        while (1)
        {
            LED0=!LED0;
            delay_ms(200);
        }
    }

    printf("IP: %s\r\n", ip_buf);
    LCD_ShowString(30,330,200,16,16, ip_buf);
    
    /* 启动 ATK-MW8266D UART 接收 */
    atk_mw8266d_uart_rx_restart();
    vTaskDelete(NULL);
}

void dht11_task(void *pv)
{
    DHT11_Data_t dht11_data; 
    
    while (1)
    {
        // 通过 PCF8574 触发或准备 DHT11 的 IO（如果使用扩展 IO）
        PCF8574_ReadBit(BEEP_IO);

        // 读取 DHT11 数据（温度和湿度）
        DHT11_Read_Data(&dht11_data.temperature,&dht11_data.humidity);
    
        // 将数据发送到队列
        xQueueSend(xDHT11Queue,&dht11_data,0);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void ap3216c_task(void *pv)
{
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        // 读取 AP3216C 数据（红外、接近、环境光）
        AP3216C_ReadData(&ap3216c_data.ir,&ap3216c_data.ps,&ap3216c_data.als);

        // 发送到队列供显示或网络任务使用
        xQueueSend(xAP3216CQueue,&ap3216c_data,0);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void lcd_task(void *pv)
{
    DHT11_Data_t dht11_data;
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        // 从 DHT11 队列接收并更新 LCD
        if(xQueueReceive(xDHT11Queue,&dht11_data,pdMS_TO_TICKS(100))==pdTRUE){
             if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                 // 更新温度显示
                 LCD_Fill(30+40, 150, 30+40+16, 150+16, WHITE);
                 LCD_ShowNum(30+40, 150, dht11_data.temperature, 2, 16);
                 
                 // 更新湿度显示
                 LCD_Fill(30+40, 170, 30+40+16, 170+16, WHITE);
                 LCD_ShowNum(30+40, 170, dht11_data.humidity, 2, 16);
                 
                 xSemaphoreGive(xLCDMutex);
             }
        }

        // 从 AP3216C 队列接收并更新 LCD
        if(xQueueReceive(xAP3216CQueue, &ap3216c_data, pdMS_TO_TICKS(100)) == pdTRUE) {
            if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // 更新 IR
                LCD_Fill(30+32, 220, 30+32+40, 220+16, WHITE);
                LCD_ShowNum(30+32, 220, ap3216c_data.ir, 5, 16);
                
                // 更新 PS
                LCD_Fill(30+32, 250, 30+32+40, 250+16, WHITE);
                LCD_ShowNum(30+32, 250, ap3216c_data.ps, 5, 16);
                
                // 更新 ALS
                LCD_Fill(30+32, 280, 30+32+40, 280+16, WHITE);
                LCD_ShowNum(30+32, 280, ap3216c_data.als, 5, 16);
                
                xSemaphoreGive(xLCDMutex);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));  // 界面刷新延时
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

    while (1)
    {
        key = KEY_Scan(0);

        switch (key)
        {
            case KEY0_PRES: // 尝试连接到 MQTT Broker
                printf("Connecting to MQTT Broker...\r\n");
                
                // 0. 测试连通性（ping）
                atk_mw8266d_ping(MQTT_BROKER_IP);

                // 1. 配置 MQTT 用户信息（尝试不同 scheme）
                if (atk_mw8266d_mqtt_usercfg(1, MQTT_CLIENT_ID, MQTT_USER_NAME, MQTT_PASSWORD) == ATK_MW8266D_EOK)
                {
                    printf("MQTT User Configured.\r\n");
                }
                else
                {
                    printf("MQTT User Config Failed! Trying scheme 0...\r\n");
                    if (atk_mw8266d_mqtt_usercfg(0, MQTT_CLIENT_ID, MQTT_USER_NAME, MQTT_PASSWORD) == ATK_MW8266D_EOK)
                    {
                        printf("MQTT User Configured (scheme 0).\r\n");
                    }
                    else
                    {
                        printf("MQTT User Config Failed (both schemes)!\r\n");
                    }
                }
                
                // 2. 连接 Broker
                if (atk_mw8266d_mqtt_conn(MQTT_BROKER_IP, MQTT_BROKER_PORT, 0) == ATK_MW8266D_EOK)
                {
                    printf("MQTT Connected!\r\n");
                    con_status = 1;
                    
                    // 3. 订阅主题（如果需要）
                    atk_mw8266d_mqtt_sub(MQTT_TOPIC_PUB, 0);
                }
                else
                {
                    printf("MQTT Connect Failed!\r\n");
                    con_status = 0;
                }
                break;

            case KEY1_PRES: // 断开 MQTT
                if (atk_mw8266d_mqtt_clean() == ATK_MW8266D_EOK)
                {
                    printf("MQTT Disconnected!\r\n");
                    con_status = 0;
                }
                else
                {
                    printf("Error disconnect MQTT!\r\n");
                }
                break;

            default:
                break;
        }

        if (!con_status)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // 如果连接已建立，采集并发布传感器数据

        // 发布 DHT11 数据
        if (xQueueReceive(xDHT11Queue, &dht11_data, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            // 构建 JSON 字符串
            snprintf(json_buf, sizeof(json_buf),
                     "{\"temp\":%d,\"humi\":%d}",
                     dht11_data.temperature,
                     dht11_data.humidity);
            
            // 发布到 MQTT
            if(atk_mw8266d_mqtt_pub(MQTT_TOPIC_PUB, json_buf, 0, 0) == ATK_MW8266D_EOK)
            {
                printf("MQTT Pub DHT11 OK\r\n");
            }
            else
            {
                printf("MQTT Pub Failed!\r\n");
            }
        }

        // 发布 AP3216C 数据
        if (xQueueReceive(xAP3216CQueue, &ap3216c_data, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            snprintf(json_buf, sizeof(json_buf),
                     "{\"als\":%d,\"ir\":%d,\"ps\":%d}",
                     ap3216c_data.als,
                     ap3216c_data.ir,
                     ap3216c_data.ps);

            if(atk_mw8266d_mqtt_pub(MQTT_TOPIC_PUB, json_buf, 0, 0) == ATK_MW8266D_EOK)
            {
                printf("MQTT Pub AP3216C OK\r\n");
            }
            else
            {
                printf("MQTT Pub Failed!\r\n");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
