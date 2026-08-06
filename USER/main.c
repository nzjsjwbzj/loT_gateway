#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "led.h"
#include "key.h"
#include "lcd.h"
#include "sdram.h"
#include "dht11.h"
#include "pcf8574.h"
#include "wdog.h"
#include "atk_mw8266d.h"
#include "atk_mw8266d_uart.h"
#include "cJSON.h"
#include "w25qxx.h"
#include "../OTA/ota.h"
#include "flash.h"
#include "store_flash.h"
#include "ap3216c.h"
#include "sensor_app.h"
#include "network_app.h"

#include "MQTTPacket.h"
#include "MQTTConnect.h"
#include "MQTTPublish.h"
#include "MQTTSubscribe.h"
#include "transport.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

// 任务优先级定义
#define START_TASK_PRIO  1
#define INIT_TASK_PRIO  5
#define DHT11_TASK_PRIO  3
#define AP3216C_TASK_PRIO  3
#define LCD_TASK_PRIO 2
#define NET_TASK_PRIO 4
#define RUN_TIME_STATS_TASK_PRIO 2
#define DATA_PROCESS_TASK_PRIO 4  // 数据处理任务优先级
#define FLASH_WRITER_TASK_PRIO 2
#define WATCHDOG_TASK_PRIO 6

// 任务堆栈大小定义
#define START_STK_SIZE 128
#define INIT_STK_SIZE 256
#define DHT11_STK_SIZE 128
#define AP3216C_STK_SIZE 256
#define LCD_STK_SIZE 128
#define NET_STK_SIZE 500
#define RUN_TIME_STATS_STK_SIZE 256
#define DATA_PROCESS_STK_SIZE 256 // 数据处理任务堆栈大小
#define WATCHDOG_STK_SIZE 128
#define FLASH_WRITER_STK_SIZE 256

// 任务句柄
TaskHandle_t StartTask_Handler;
TaskHandle_t InitTask_Handler;
TaskHandle_t DHT11Task_Handler;
TaskHandle_t AP3216CTask_Handler;
TaskHandle_t LCDTask_Handler;
TaskHandle_t NETTask_Handler;
TaskHandle_t RunTimeStatsTask_Handler;
//TaskHandle_t DataProcessTask_Handler; // 数据处理任务句柄
TaskHandle_t WatchdogTask_Handler;
TaskHandle_t FlashWriterTask_Handler;

// 任务函数声明
void start_task(void *pv);
void init_task(void *pv);
void dht11_task(void *pv);
void ap3216c_task(void *pv);
void lcd_task(void *pv);
void net_task(void *pv);
void run_time_stats_task(void *pv);
//void data_process_task(void *pv); // 数据处理任务函数
void watchdog_task(void *pv);


QueueHandle_t xUartRxQueue; // UART 接收队列
SemaphoreHandle_t xLCDMutex;

TIM_HandleTypeDef htim2;

/**
 * @brief       配置 FreeRTOS 运行时间统计所用的定时器 (TIM2)
 *              TIM2 是 32 位定时器，非常适合用来做长时间的高精度运行统计
 *              APB1 定时器时钟频率为 45MHz，经过倍频后为 90MHz
 *              我们设置 20kHz 的频率 (50us 周期)，这样它的精度比系统滴答心跳 (1kHz) 高 20 倍
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
 * @brief       获取高精度定时器的当前计数值
 * @retval      当前定时器计数值
 */
uint32_t GetTimerCounterValue(void)
{
    return __HAL_TIM_GET_COUNTER(&htim2);
}

int main(void)
{
    SCB->VTOR=Application_1_Addr ; // 确保中断向量表正确设置
    HAL_Init();                     // 初始化 HAL 库
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    Stm32_Clock_Init(360,25,2,8);   // 设置时钟为 180MHz
    delay_init(180);                // 初始化延时函数
    uart_init(115200);              // 初始化串口波特率
    LED_Init();                      // 初始化 LED
    KEY_Init();                     // 初始化按键按键
    SDRAM_Init();                   // 初始化 SDRAM 外部内存
	
	printf("reset  v5.7\r\n");

    W25QXX_Init();
    uint16_t flash_id = W25QXX_ReadID();
    printf("W25Q ID: 0x%04X\r\n", flash_id);
    if (flash_id == W25Q256 || flash_id == W25Q128 || flash_id == W25Q64)
    {
        flash_ready = 1; // 标记 Flash 硬件可用，flash_writer_task 初始化时会用到
    }
     flash_store_init();//不能在FreeRTOS任务中，不然读取的第一个字节会被RTOS的堆覆盖成A5，导致flash_store_rescan()判断错误
    // 创建启动任务并启动调度器
    xTaskCreate(start_task,"start_task",START_STK_SIZE,NULL,START_TASK_PRIO,&StartTask_Handler);
    vTaskStartScheduler();  
    
    while(1);

    return 0;
}

void start_task(void *pv)
{
    // 创建各个传感器和外设交互的消息队列
    xDHT11Queue = xQueueCreate(5, sizeof(DHT11_Data_t));
    xAP3216CQueue = xQueueCreate(5, sizeof(AP3216C_Data_t));
    xAP3216CQueueForMQTT = xQueueCreate(5, sizeof(AP3216C_Data_t)); // 专用于 MQTT 上报的队列
    xUartRxQueue = xQueueCreate(10, sizeof(uint16_t)); // UART 数据接收队列
    xLCDMutex = xSemaphoreCreateMutex();
    xFlashReqQueue = xQueueCreate(FLASH_REQ_QUEUE_LEN, sizeof(FlashRequest)); // Flash 写任务请求队列

    cJSON_Hooks hooks;
    hooks.malloc_fn = pvPortMalloc;
    hooks.free_fn = vPortFree;
    cJSON_InitHooks(&hooks);

    // 检查队列及互斥锁是否成功创建
    if (xDHT11Queue == NULL || xAP3216CQueue == NULL || xLCDMutex == NULL ||
        xUartRxQueue == NULL || xFlashReqQueue == NULL) {
        printf("队列/互斥锁创建失败\r\n");
    }

    // 创建系统的核心工作任务
    xTaskCreate(init_task,   "init_task",   INIT_STK_SIZE, NULL, INIT_TASK_PRIO, &InitTask_Handler);
    xTaskCreate(flash_writer_task, "flash_wr", FLASH_WRITER_STK_SIZE, NULL, FLASH_WRITER_TASK_PRIO, &FlashWriterTask_Handler);
    xTaskCreate(dht11_task,  "dht11_task",  DHT11_STK_SIZE, NULL, DHT11_TASK_PRIO, &DHT11Task_Handler);
    xTaskCreate(ap3216c_task,"ap3216c_task",AP3216C_STK_SIZE, NULL, AP3216C_TASK_PRIO, &AP3216CTask_Handler);
    xTaskCreate(lcd_task,    "lcd_task",    LCD_STK_SIZE, NULL, LCD_TASK_PRIO, &LCDTask_Handler);
    xTaskCreate(net_task,    "net_task",    NET_STK_SIZE, NULL, NET_TASK_PRIO, &NETTask_Handler);
    //xTaskCreate(run_time_stats_task, "stats_task", RUN_TIME_STATS_STK_SIZE, NULL, RUN_TIME_STATS_TASK_PRIO, &RunTimeStatsTask_Handler);
   // xTaskCreate(watchdog_task, "watchdog", WATCHDOG_STK_SIZE, NULL, WATCHDOG_TASK_PRIO, &WatchdogTask_Handler);


    // 启动任务创建完其他任务后，删除自身释放资源
    vTaskDelete(NULL);
}

void init_task(void *pv)
{
    char ip_buf[16];
    bool  link_status; // WiFi 链路状态
    // 删除：bool con_status;  (这是全局变量，不应该在这里重新声明！)

    LCD_Init();                     // LCD 屏幕初始化
    PCF8574_Init();                 // PCF8574 扩展 IO 芯片初始化


    // 获取 PCF8574 扩展IO 状态以初始化 DHT11 相关引脚
    PCF8574_ReadBit(BEEP_IO);

    // 初始化 AP3216C 光照传感器并在 LCD 报错提示
    while(AP3216C_Init())
    {
        LCD_ShowString(30,190,200,16,16,"AP3216C Check Failed!");
        delay_xms(500);
        LCD_ShowString(30,190,200,16,16,"Please Check!        ");
        delay_xms(500);
    }    
    
    // 绘制 LCD 静态框架和默认文字
    POINT_COLOR=BLUE;
    // LCD_ShowString(30,150,200,16,16,"Temp:  C");    
    // LCD_ShowString(30,170,200,16,16,"Humi:  %");
    
    POINT_COLOR=RED;
    LCD_ShowString(30,190,200,16,16,"AP3216C Ready!");  
    POINT_COLOR=BLUE;
    LCD_ShowString(30,220,200,16,16," IR:");     
    LCD_ShowString(30,250,200,16,16," PS:");    
    LCD_ShowString(30,280,200,16,16,"ALS:");

    wifi_net_init(DEMO_WIFI_SSID, DEMO_WIFI_PWD, ip_buf);

    /* 开启 ATK-MW8266D UART 接收中断以便透传数据 */
    atk_mw8266d_uart_rx_restart();
    vTaskDelete(NULL);
}


void lcd_task(void *pv)
{
    DHT11_Data_t dht11_data;
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        wd_heartbeat(WD_BIT_LCD);
        // 读取 DHT11 队列数据并刷新到 LCD 显示屏
        // if(xQueueReceive(xDHT11Queue,&dht11_data,pdMS_TO_TICKS(100))==pdTRUE){
        //      if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        //          // 刷新温度值 (用白底覆盖旧字)
        //          LCD_Fill(30+40, 150, 30+40+16, 150+16, WHITE);
        //          LCD_ShowNum(30+40, 150, dht11_data.temperature, 2, 16);
                 
        //          // 刷新湿度值 (用白底覆盖旧字)
        //          LCD_Fill(30+40, 170, 30+40+16, 170+16, WHITE);
        //          LCD_ShowNum(30+40, 170, dht11_data.humidity, 2, 16);
                 
        //          xSemaphoreGive(xLCDMutex);
        //      }
        // }

        // 读取 AP3216C 队列数据并刷新到 LCD 显示屏
        if(xQueueReceive(xAP3216CQueue, &ap3216c_data, pdMS_TO_TICKS(100)) == pdTRUE) {
            if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // 刷新红外 IR 强度
                LCD_Fill(30+32, 220, 30+32+40, 220+16, WHITE);
                LCD_ShowNum(30+32, 220, ap3216c_data.ir, 5, 16);
                
                // 刷新接近距离 PS
                LCD_Fill(30+32, 250, 30+32+40, 250+16, WHITE);
                LCD_ShowNum(30+32, 250, ap3216c_data.ps, 5, 16);
                
                // 刷新环境光照 ALS 强度
                LCD_Fill(30+32, 280, 30+32+40, 280+16, WHITE);
                LCD_ShowNum(30+32, 280, ap3216c_data.als, 5, 16);
                
                xSemaphoreGive(xLCDMutex);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));  // 延时 50ms 起到防抖和释放 CPU 的作用
    }
}

/**
 * @brief       FreeRTOS 运行时间统计任务
 *              定时打印当前系统中各个任务的 CPU 占用率和绝对运行时间
 */                                                                                                   
void run_time_stats_task(void *pv)
{
    char pcWriteBuffer[512];
    
    while (1)
    {
       // wd_heartbeat(WD_BIT_STAT);
        
        vTaskGetRunTimeStats(pcWriteBuffer);

        printf("TaskName\tAbsTime\t\tTime%%\r\n%s\r\n", pcWriteBuffer);
        
        // 每 5 秒钟打印一次统计信息
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}






void PreSleepProcessing(uint32_t *ulExpectedIdleTime)
{
    if(*ulExpectedIdleTime < 5)
    {
        *ulExpectedIdleTime = 0;
        return;
    }
    
    // 限制最大休眠时间，以免看门狗喂狗超时 (7秒看门狗，这里限制最多睡5秒)
    if (*ulExpectedIdleTime > 5000)
    {
        *ulExpectedIdleTime = 5000;
    }

    // 这里通常不应该去关闭/开启屏幕和LED
    // 因为这只代表RTOS进入了空闲几毫秒~几秒钟的状态
    // 如果频繁关闭再打开LCD，就会导致极其明显的闪屏
    // 直接让系统默认执行 __WFI() 或进入浅睡眠即可
}

void PostSleepProcessing(uint32_t *ulExpectedIdleTime)
{
    // 恢复操作保持为空
}
