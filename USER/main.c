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

#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
/************************************************
 ??????? ALIENTEK ?????? STM32F429 ???????
 DHT11 ?? AP3216C ????????? HAL ??
 ????????: www.openedv.com
 ????: ALIENTEK
************************************************/
#define DEMO_WIFI_SSID          "CMCC-X22b"
#define DEMO_WIFI_PWD           "gp24ghzf"
#define DEMO_ATKCLD_DEV_ID      "63152966615081879519"
#define DEMO_ATKCLD_DEV_PWD     "12345678"

// MQTT ??????????
// #define MQTT_BROKER_IP          "broker.emqx.io"
// ?????? Broker?????? Mosquitto ???? 1883??
#define MQTT_BROKER_IP          "192.168.1.28"
#define MQTT_BROKER_PORT        "1883"
#define MQTT_CLIENT_ID          "STM32_F429_Client_Test"
// ??? Broker ????????/?????????
#define MQTT_USER_NAME          ""
#define MQTT_PASSWORD           ""
#define MQTT_TOPIC_PUB          "test/topic"
#define MQTT_TOPIC_SUB          "test/topic_sub"


/**
 * @brief       ???????????? ATK-MW8266D UART ????
 * @param       is_atkcld: 0: ?????? ATK-CLD
 *                         1: ???? ATK-MW8266D UART ?????????
 * @retval      ??
 */
static void demo_upload_data(uint8_t is_atkcld)
{
    uint8_t *buf;
    
    if (is_atkcld == 1)
    {
        /* ??? ATK-MW8266D UART ???????? */
        buf = atk_mw8266d_uart_rx_get_frame();
        if (buf != NULL)
        {
            printf("%s", buf);
            /* ?????????? UART ??????? */
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

// ?????????
#define START_TASK_PRIO  1
#define INIT_TASK_PRIO  5
#define DHT11_TASK_PRIO  3
#define AP3216C_TASK_PRIO  3
#define LCD_TASK_PRIO 2
#define NET_TASK_PRIO 4
#define RUN_TIME_STATS_TASK_PRIO 2
#define DATA_PROCESS_TASK_PRIO 4  // ????????????????

// ??????????????
#define START_STK_SIZE 128
#define INIT_STK_SIZE 128
#define DHT11_STK_SIZE 128
#define AP3216C_STK_SIZE 128
#define LCD_STK_SIZE 128
#define NET_STK_SIZE 500
#define RUN_TIME_STATS_STK_SIZE 256
#define DATA_PROCESS_STK_SIZE 256 // ?????????????

// ??????
TaskHandle_t StartTask_Handler;
TaskHandle_t InitTask_Handler;
TaskHandle_t DHT11Task_Handler;
TaskHandle_t AP3216CTask_Handler;
TaskHandle_t LCDTask_Handler;
TaskHandle_t NETTask_Handler;
TaskHandle_t RunTimeStatsTask_Handler;
TaskHandle_t DataProcessTask_Handler; // ?????????????

// ?????????
void start_task(void *pv);
void init_task(void *pv);
void dht11_task(void *pv);
void ap3216c_task(void *pv);
void lcd_task(void *pv);
void net_task(void *pv);
void run_time_stats_task(void *pv);
void data_process_task(void *pv); // ???????????

// ??????????
QueueHandle_t xDHT11Queue;
QueueHandle_t xAP3216CQueue;
QueueHandle_t xUartRxQueue; // UART ???????????
SemaphoreHandle_t xLCDMutex;

u8 ret;
char ip_buf[16];
bool  link_status; // WiFi ?????????????????
bool  con_status;  // MQTT ?????????????????

TIM_HandleTypeDef htim2;

/**
 * @brief       ???? FreeRTOS ?????????????? (TIM2)
 *              TIM2 ?? 32 ?????????????????????????
 *              APB1 ???? 45MHz??????????? 90MHz
 *              ???? 20kHz ??? (50us ????)?????????? (1kHz) ?? 20 ??
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
    HAL_Init();                     // ????? HAL ??
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    Stm32_Clock_Init(360,25,2,8);   // ????????????? 180MHz
    delay_init(180);                // ????????????????
    uart_init(115200);              // ????????
    LED_Init();                     // LED ?????
    KEY_Init();                     // ?????????
    SDRAM_Init();                   // SDRAM ?????

    // ??????????????????
    xTaskCreate(start_task,"start_task",START_STK_SIZE,NULL,START_TASK_PRIO,&StartTask_Handler);
    vTaskStartScheduler();  
    
    while(1);

    return 0;
}

void start_task(void *pv)
{
    // ???????????????
    xDHT11Queue = xQueueCreate(5, sizeof(DHT11_Data_t));
    xAP3216CQueue = xQueueCreate(5, sizeof(AP3216C_Data_t));
    xUartRxQueue = xQueueCreate(10, sizeof(uint16_t)); // ?????????????????
    xLCDMutex = xSemaphoreCreateMutex();

    cJSON_Hooks hooks;
    hooks.malloc_fn = pvPortMalloc;
    hooks.free_fn = vPortFree;
    cJSON_InitHooks(&hooks);

    // ???????????????
    if (xDHT11Queue == NULL || xAP3216CQueue == NULL || xLCDMutex == NULL || xUartRxQueue == NULL) {
        printf("????????????????????????????\r\n");
        // ?????????????????????????????????
    }

    // ????????????
    xTaskCreate(init_task,   "init_task",   INIT_STK_SIZE, NULL, INIT_TASK_PRIO, &InitTask_Handler);
    xTaskCreate(dht11_task,  "dht11_task",  DHT11_STK_SIZE, NULL, DHT11_TASK_PRIO, &DHT11Task_Handler);
    xTaskCreate(ap3216c_task,"ap3216c_task",AP3216C_STK_SIZE, NULL, AP3216C_TASK_PRIO, &AP3216CTask_Handler);
    xTaskCreate(lcd_task,    "lcd_task",    LCD_STK_SIZE, NULL, LCD_TASK_PRIO, &LCDTask_Handler);
    xTaskCreate(net_task,    "net_task",    NET_STK_SIZE, NULL, NET_TASK_PRIO, &NETTask_Handler);
    xTaskCreate(run_time_stats_task, "stats_task", RUN_TIME_STATS_STK_SIZE, NULL, RUN_TIME_STATS_TASK_PRIO, &RunTimeStatsTask_Handler);
   // xTaskCreate(data_process_task,   "data_process", DATA_PROCESS_STK_SIZE, NULL, DATA_PROCESS_TASK_PRIO, &DataProcessTask_Handler);

    // ??????????????
    vTaskDelete(NULL);
}

void init_task(void *pv)
{
    u8 ret;
    char ip_buf[16];
    bool  link_status; // WiFi ?????????
    bool  con_status;  // MQTT ?????????

    LCD_Init();                     // LCD ?????
    PCF8574_Init();                 // PCF8574 ???IO?????

    // ??? PCF8574 ???IO??????????? I/O ??????????????????? DHT11 ????????
    PCF8574_ReadBit(BEEP_IO);

    // ????????? AP3216C?????? LCD ??????????
    while(AP3216C_Init())
    {
        LCD_ShowString(30,190,200,16,16,"AP3216C Check Failed!");
        delay_xms(500);
        LCD_ShowString(30,190,200,16,16,"Please Check!        ");
        delay_xms(500);
    }    
    
    // ??????????
    POINT_COLOR=BLUE;
    LCD_ShowString(30,150,200,16,16,"Temp:  C");    
    LCD_ShowString(30,170,200,16,16,"Humi:  %");
    
    POINT_COLOR=RED;
    LCD_ShowString(30,190,200,16,16,"AP3216C Ready!");  
    POINT_COLOR=BLUE;
    LCD_ShowString(30,220,200,16,16," IR:");     
    LCD_ShowString(30,250,200,16,16," PS:");    
    LCD_ShowString(30,280,200,16,16,"ALS:");

    printf("????????...\r\n");

    /* ????? ATK-MW8266D WiFi ??? */
    ret = atk_mw8266d_init(115200);
    if (ret != 0)
    {
        printf("ATK-MW8266D ????????!\r\n");
        while (1)
        {
            LED0=!LED0;
            delay_ms(200);
        }
    }
    
    printf("???????? AP...\r\n");
    ret  = atk_mw8266d_restore();                               /* ??????????? */
    ret += atk_mw8266d_at_test();                               /* ???? AT ??? */
    ret += atk_mw8266d_set_mode(1);                             /* ????? Station ?? */
    ret += atk_mw8266d_sw_reset();                              /* ??????????? */
    ret += atk_mw8266d_ate_config(0);                           /* ?????? */
    ret += atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD);  /* ???? WiFi */
    ret += atk_mw8266d_get_ip(ip_buf);                          /* ??????? IP */
    if (ret != 0)
    {
        printf("???? AP ???!\r\n");
        while (1)
        {
            LED0=!LED0;
            delay_ms(200);
        }
    }

    printf("IP: %s\r\n", ip_buf);
    LCD_ShowString(30,330,200,16,16, ip_buf);
    
    /* ???? ATK-MW8266D UART ???? */
    atk_mw8266d_uart_rx_restart();
    vTaskDelete(NULL);
}

void dht11_task(void *pv)
{
    DHT11_Data_t dht11_data; 
    
    while (1)
    {
        // // ??? PCF8574 ????? DHT11 ???????? IO ??????????? IO ????
        //  PCF8574_ReadBit(BEEP_IO);

        // // ??? DHT11 ?????????
        //  DHT11_Read_Data(&dht11_data.temperature,&dht11_data.humidity);
    
        // // ??????????????
        //  xQueueSend(xDHT11Queue,&dht11_data,0);

        // demo_upload_data(1); // ???? net_task ?????????? ATK-MW8266D UART ????

        vTaskDelay(pdMS_TO_TICKS(2000)); // ????????????????? MQTT ???????????
    }
}

void ap3216c_task(void *pv)
{
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        // ??? AP3216C ?????????????????????????
        AP3216C_ReadData(&ap3216c_data.ir,&ap3216c_data.ps,&ap3216c_data.als);

        // ?????????????????????????????
        xQueueSend(xAP3216CQueue,&ap3216c_data,0);

        vTaskDelay(pdMS_TO_TICKS(2000)); // ??????
    }
}

void lcd_task(void *pv)
{
    DHT11_Data_t dht11_data;
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        // ?? DHT11 ????????????????? LCD
        if(xQueueReceive(xDHT11Queue,&dht11_data,pdMS_TO_TICKS(100))==pdTRUE){
             if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                 // ???????
                 LCD_Fill(30+40, 150, 30+40+16, 150+16, WHITE);
                 LCD_ShowNum(30+40, 150, dht11_data.temperature, 2, 16);
                 
                 // ???????
                 LCD_Fill(30+40, 170, 30+40+16, 170+16, WHITE);
                 LCD_ShowNum(30+40, 170, dht11_data.humidity, 2, 16);
                 
                 xSemaphoreGive(xLCDMutex);
             }
        }

        // ?? AP3216C ????????????????? LCD
        if(xQueueReceive(xAP3216CQueue, &ap3216c_data, pdMS_TO_TICKS(100)) == pdTRUE) {
            if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // ??? IR ?
                LCD_Fill(30+32, 220, 30+32+40, 220+16, WHITE);
                LCD_ShowNum(30+32, 220, ap3216c_data.ir, 5, 16);
                
                // ??? PS ?
                LCD_Fill(30+32, 250, 30+32+40, 250+16, WHITE);
                LCD_ShowNum(30+32, 250, ap3216c_data.ps, 5, 16);
                
                // ??? ALS ?
                LCD_Fill(30+32, 280, 30+32+40, 280+16, WHITE);
                LCD_ShowNum(30+32, 280, ap3216c_data.als, 5, 16);
                
                xSemaphoreGive(xLCDMutex);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));  // ??????
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


    while (1)
    {
        key = KEY_Scan(0);

        switch (key)
        {
            case KEY0_PRES: // ???? MQTT Broker
                printf("Connecting to MQTT Broker...\r\n");
                
                // 0. ??????????????ping
                atk_mw8266d_ping(MQTT_BROKER_IP);

                // 1. ???? MQTT ????????????? scheme 1
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
                
                // 2. ???? Broker
                if (atk_mw8266d_mqtt_conn(MQTT_BROKER_IP, MQTT_BROKER_PORT, 0) == ATK_MW8266D_EOK)
                {
                    printf("MQTT Connected!\r\n");
                    con_status = 1;
                    
                    // 3. ???????????????????????
                    atk_mw8266d_mqtt_sub(MQTT_TOPIC_SUB, 0);
                }
                else
                {
                    printf("MQTT Connect Failed!\r\n");
                    con_status = 0;
                }
                break;

            case KEY1_PRES: // ??? MQTT
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

        // ?????????????????????????

        // // 读取 DHT11 数据
        // if (xQueueReceive(xDHT11Queue, &dht11_data, pdMS_TO_TICKS(50)) == pdTRUE)
        // {
        //     cJSON *root = cJSON_CreateObject();
        //     cJSON_AddNumberToObject(root, "temp", dht11_data.temperature);
        //     cJSON_AddNumberToObject(root, "humi", dht11_data.humidity);
        //
        //     if (!cJSON_PrintPreallocated(root, json_buf, sizeof(json_buf), 0))
        //     {
        //         printf("JSON Print Failed!\r\n");
        //     }
        //     cJSON_Delete(root);
            
        //     // 发布到 MQTT
        //     if(atk_mw8266d_mqtt_pub(MQTT_TOPIC_PUB, json_buf, 0, 0) == ATK_MW8266D_EOK)
        //     {
        //         printf("MQTT Pub DHT11 OK\r\n");
        //     }
        //     else
        //     {
        //         printf("MQTT Pub Failed!\r\n");
        //     }
        // }

        // 读取 AP3216C 数据
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

            if(atk_mw8266d_mqtt_pub(MQTT_TOPIC_PUB, json_buf, 0, 0) == ATK_MW8266D_EOK)
            {
                printf("MQTT Pub AP3216C OK\r\n");
            } 
            else
            {
                printf("MQTT Pub Failed!\r\n");
            }
        }
        
        // ??????????? MQTT ??? (??? UART ???????)
        // demo_upload_data(1); // ????????????? data_process_task ??????
         if (xQueueReceive(xUartRxQueue, &rx_len, portMAX_DELAY) == pdTRUE)
        {
            // ?????????????????????????
            buf = atk_mw8266d_uart_rx_get_frame();
            if (buf != NULL)
            {

                // ?????????????????????       (Protocol ID ??)
                
                // ??????????????????
                printf("MsgPump: %s", buf);
                
                // ???????????? UART ?????????
                atk_mw8266d_uart_rx_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief       FreeRTOS ????????????
 *              ????????????????????CPU?????
 */
void run_time_stats_task(void *pv)
{
    char pcWriteBuffer[512];
    
    while (1)
    {
        
        vTaskGetRunTimeStats(pcWriteBuffer);

        printf("TaskName\tAbsTime\t\tTime%%\r\n%s\r\n", pcWriteBuffer);
        
        // ? 5 ????????
        vTaskDelay(pdMS_TO_TICKS(5000));

    }
}

/**
 * @brief       ??????????? (????)
 *              ??????? UART ???????????????????????????
 */

void data_process_task(void *pv)
{
    uint16_t rx_len;
    uint8_t *buf;

    while (1)
    {
        // ???????? UART ????????????? (?????demo_upload_data????)
       

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}