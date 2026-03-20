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
#include "w25qxx.h"

// MQTT???????
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
 ??????: ALIENTEK ?????? STM32F429 ??????
 DHT11 / AP3216C ??????? HAL ???
 ???????: www.openedv.com
 ????????: ALIENTEK
************************************************/
#define DEMO_WIFI_SSID          "ari"
#define DEMO_WIFI_PWD           "zhanglanxiong"
#define DEMO_ATKCLD_DEV_ID      "63152966615081879519"
#define DEMO_ATKCLD_DEV_PWD     "12345678"

//MQTT ??????????
//#define MQTT_BROKER_IP          "broker.emqx.io"
//????????? Broker???? Mosquitto???????????? 1883

// #define MQTT_BROKER_IP          "192.168.87.199"
// #define MQTT_BROKER_PORT        "1883"
// #define MQTT_CLIENT_ID          "STM32_F429_Client_Test"
// // ??? Broker ????????/??????????????
// #define MQTT_USER_NAME          ""
// #define MQTT_PASSWORD           ""
// // ???????¦·? Topic?????????????õ��????
// #define MQTT_TOPIC_PUB          "user/dev1/data"
// #define MQTT_TOPIC_SUB          "user/dev1/control"

#define MQTT_BROKER_IP          "studio-mqtt.heclouds.com"
#define MQTT_BROKER_PORT        "1883"
#define MQTT_CLIENT_ID          "d0"//设备名称
#define MQTT_USER_NAME          "XHm5ltr9qA"//产品ID
#define MQTT_PASSWORD           "version=2018-10-31&res=products%2FXHm5ltr9qA%2Fdevices%2Fd0&et=2089352870&method=sha1&sign=g0Ci5BoZ25In%2FlxnVt8zRF9IyLg%3D"
#define MQTT_TOPIC_PUB          "$sys/XHm5ltr9qA/d0/thing/property/post"
#define MQTT_TOPIC_SUB          "$sys/XHm5ltr9qA/d0/thing/property/set"

/**
 * @brief       ??????????????? ATK-MW8266D UART ????
 * @param       is_atkcld: 0: ????? ATK-CLD
 *                         1: ???? ATK-MW8266D UART ???????????
 * @retval      ??
 */
static void demo_upload_data(uint8_t is_atkcld)
{
    uint8_t *buf;
    
    if (is_atkcld == 1)
    {
        /* ??? ATK-MW8266D UART ????? */
        buf = atk_mw8266d_uart_rx_get_frame();
        if (buf != NULL)
        {
            printf("%s", buf);
            /* ???? ATK-MW8266D UART ???? */
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
#define WATCHDOG_TASK_PRIO 6

// ??????????
#define START_STK_SIZE 128
#define INIT_STK_SIZE 256
#define DHT11_STK_SIZE 128
#define AP3216C_STK_SIZE 256
#define LCD_STK_SIZE 128
#define NET_STK_SIZE 500
#define RUN_TIME_STATS_STK_SIZE 256
#define DATA_PROCESS_STK_SIZE 256 // ?????????????????
#define WATCHDOG_STK_SIZE 128

// ??????
TaskHandle_t StartTask_Handler;
TaskHandle_t InitTask_Handler;
TaskHandle_t DHT11Task_Handler;
TaskHandle_t AP3216CTask_Handler;
TaskHandle_t LCDTask_Handler;
TaskHandle_t NETTask_Handler;
TaskHandle_t RunTimeStatsTask_Handler;
TaskHandle_t DataProcessTask_Handler; // ?????????????
TaskHandle_t WatchdogTask_Handler;

// ????????
void start_task(void *pv);
void init_task(void *pv);
void dht11_task(void *pv);
void ap3216c_task(void *pv);
void lcd_task(void *pv);
void net_task(void *pv);
void run_time_stats_task(void *pv);
void data_process_task(void *pv); // ???????????
void watchdog_task(void *pv);

// ???????
QueueHandle_t xDHT11Queue;
QueueHandle_t xAP3216CQueue;
QueueHandle_t xAP3216CQueueForMQTT; // MQTT??????
QueueHandle_t xUartRxQueue; // UART ???????????
SemaphoreHandle_t xLCDMutex;
SemaphoreHandle_t xFlashMutex;

u8 ret;
char ip_buf[16];
bool  link_status; // WiFi ??????
bool  con_status;  // MQTT ??????
volatile uint8_t g_mqtt_connected = 0;

// MQTT??????
static int mqtt_sock = -1;  // MQTT socket ID
static unsigned short mqtt_packet_id = 1;  // MQTT??ID
static unsigned char mqtt_send_buf[512];    // MQTT?????????
static unsigned char mqtt_recv_buf[512];    // MQTT?????????

typedef struct
{
    uint8_t flag;
    uint16_t len;
    char data[253];
} FlashRecord;

#define FLASH_DATA_START      0x00100000
#define FLASH_DATA_SIZE       (1024 * 1024)
#define FLASH_SECTOR_SIZE     4096
#define FLASH_RECORD_SIZE     256
#define FLASH_RECORD_COUNT    (FLASH_DATA_SIZE / FLASH_RECORD_SIZE)
#define FLASH_RECORDS_PER_SECTOR (FLASH_SECTOR_SIZE / FLASH_RECORD_SIZE)
#define FLASH_FLAG_EMPTY      0xFF
#define FLASH_FLAG_VALID      0xA5
#define FLASH_FLAG_SENT       0x00

static uint32_t flash_read_index = 0;
static uint32_t flash_write_index = 0;
static uint32_t flash_valid_count = 0;
static uint8_t flash_ready = 0;
volatile uint32_t g_ap_sample_period_ms = 5000;
#define AP_SAMPLE_PERIOD_MIN_MS 200
#define AP_SAMPLE_PERIOD_MAX_MS 60000

static uint32_t flash_record_addr(uint32_t index)
{
    return FLASH_DATA_START + (index * FLASH_RECORD_SIZE);
}

static void flash_store_rescan(void) 
{
    uint8_t flag = 0;
    uint32_t first_empty = 0xFFFFFFFF;
    uint32_t first_valid = 0xFFFFFFFF;
    uint32_t valid_count = 0;  // 临时变量，先计算有多少条有效数据
    
    // ✅ 第一遍扫描：统计有效数据和找关键位置
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        W25QXX_Read(&flag, flash_record_addr(i), 1);
        if (flag == FLASH_FLAG_VALID)
        {
            valid_count++;
            if (first_valid == 0xFFFFFFFF) first_valid = i;
        }
        if ((flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT) && first_empty == 0xFFFFFFFF)
        {
            first_empty = i;
        }
    }
    
    flash_valid_count = valid_count;  // 设置最终的计数
    
    if (first_valid == 0xFFFFFFFF)
    {
        // 没有有效数据，Flash 是空的或全是已发送标记
        flash_read_index = (first_empty == 0xFFFFFFFF) ? 0 : first_empty;
        flash_write_index = flash_read_index;
        printf("DEBUG flash_store_rescan: EMPTY (first_empty=%lu)\r\n", first_empty);
    }
    else
    {
        // 有有效数据
        flash_read_index = first_valid;  // ✅ 指向第一条有效数据
        flash_write_index = (first_empty == 0xFFFFFFFF) ? first_valid : first_empty;
        printf("DEBUG flash_store_rescan: first_valid=%lu, first_empty=%lu, valid_count=%lu\r\n",
               first_valid, first_empty, valid_count);
    }
}

// ✅ 新增：Flash 格式化函数
static void flash_store_format(void)
{
    if (!flash_ready) return;
    
    printf("DEBUG: Formatting Flash (erasing all sectors)...\r\n");
    
    // 擦除所有扇区
    for (uint32_t sector = 0; sector < (FLASH_DATA_SIZE / FLASH_SECTOR_SIZE); sector++)
    {
        uint32_t addr = FLASH_DATA_START + sector * FLASH_SECTOR_SIZE;
        W25QXX_Erase_Sector((addr - FLASH_DATA_START) / FLASH_SECTOR_SIZE);
        
        if ((sector + 1) % 64 == 0)
        {
            printf("  Erased %lu sectors...\r\n", sector + 1);
        }
    }
    
    printf("DEBUG: Flash format complete!\r\n");
    
    // 重置指针
    flash_read_index = 0;
    flash_write_index = 0;
    flash_valid_count = 0;
}

static void flash_store_init(void)
{
    if (!flash_ready)
    {
        flash_read_index = 0;
        flash_write_index = 0;
        flash_valid_count = 0;
        return;
    }
    
    // ✅ 新增：检查 Flash 第一个位置的标志
    // 如果全是垃圾数据（混合 0x00 和 0xA5），则格式化
    uint8_t first_flag = 0xFF;
    W25QXX_Read(&first_flag, FLASH_DATA_START, 1);
    
    // ✅ 检查：如果第一个位置既不是 0xFF（空），也不是 0xA5（有效），也不是 0x00（已发送）
    // 或者整个 Flash 看起来很混乱，则格式化
    if (first_flag != FLASH_FLAG_EMPTY && first_flag != FLASH_FLAG_VALID && first_flag != FLASH_FLAG_SENT)
    {
        printf("DEBUG: Flash appears corrupted (first_flag=0x%02X), formatting...\r\n", first_flag);
        flash_store_format();
        return;
    }
    
    flash_store_rescan();
    printf("Flash Init Done: read_idx=%lu, write_idx=%lu, valid_count=%lu\r\n",
           flash_read_index, flash_write_index, flash_valid_count);
}

static void flash_store_erase_sector(uint32_t index)
{
    if (!flash_ready) return;
    uint32_t sector_index = (index / FLASH_RECORDS_PER_SECTOR);
    uint32_t sector_addr = FLASH_DATA_START + sector_index * FLASH_SECTOR_SIZE;
    W25QXX_Erase_Sector((sector_addr - FLASH_DATA_START) / FLASH_SECTOR_SIZE);
}

static void flash_store_push(const char *data, uint16_t len)
{
    if (!flash_ready) return;
    
    uint8_t flag = 0;
    uint32_t addr = 0;
    uint32_t empty_idx = 0xFFFFFFFF;
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t idx = (flash_write_index + i) % FLASH_RECORD_COUNT;
        W25QXX_Read(&flag, flash_record_addr(idx), 1);
        if (flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT)
        {
            empty_idx = idx;
            break;
        }
    }

    if (empty_idx != 0xFFFFFFFF)
    {
        flash_write_index = empty_idx;
    }
    else
    {
        uint32_t sector_start = (flash_write_index / FLASH_RECORDS_PER_SECTOR) * FLASH_RECORDS_PER_SECTOR;
        uint32_t sector_end = sector_start + FLASH_RECORDS_PER_SECTOR;

        flash_store_erase_sector(flash_write_index);

        if (flash_read_index >= sector_start && flash_read_index < sector_end)
        {
            flash_store_rescan();
        }
        flash_write_index = sector_start;
    }

    addr = flash_record_addr(flash_write_index);
    if (len > sizeof(((FlashRecord *)0)->data)) len = sizeof(((FlashRecord *)0)->data);
    uint8_t buf[FLASH_RECORD_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    buf[0] = FLASH_FLAG_VALID;
    buf[1] = (uint8_t)(len & 0xFF);
    buf[2] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(&buf[3], data, len);
    W25QXX_Write(buf, addr, FLASH_RECORD_SIZE);
    
    flash_write_index = (flash_write_index + 1) % FLASH_RECORD_COUNT;
    if (flash_valid_count < FLASH_RECORD_COUNT) flash_valid_count++;
}

static int flash_store_peek(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index)
{
    if (!flash_ready) return 0;
    if (flash_valid_count == 0) return 0;
    
    uint8_t flag = 0;
    uint32_t start = flash_read_index;
    uint32_t idx = flash_read_index;
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t addr = flash_record_addr(idx);
        W25QXX_Read(&flag, addr, 1);
        if (flag == FLASH_FLAG_VALID)
        {
            uint8_t len_bytes[2];
            W25QXX_Read(len_bytes, addr + 1, 2);
            uint16_t len = (uint16_t)(len_bytes[0] | (len_bytes[1] << 8));
            if (len >= max_len) len = max_len - 1;
            W25QXX_Read((uint8_t *)out, addr + 3, len);
            out[len] = '\0';
            *out_len = len;
            *out_index = idx;
            return 1;
        }
        idx = (idx + 1) % FLASH_RECORD_COUNT;
        if (idx == start)
        {
            break;
        }
    }
    flash_store_rescan();
    return 0;
}

static void flash_store_mark_sent(uint32_t index)
{
    if (!flash_ready) return;
    uint8_t sent_flag = FLASH_FLAG_SENT;
    W25QXX_Write(&sent_flag, flash_record_addr(index), 1);
    if (flash_valid_count > 0) flash_valid_count--;
    flash_read_index = (index + 1) % FLASH_RECORD_COUNT;
}

static void flash_store_push_locked(const char *data, uint16_t len)
{
    if (xFlashMutex && xSemaphoreTake(xFlashMutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        flash_store_push(data, len);
        xSemaphoreGive(xFlashMutex);
    }
    else
    {
        flash_store_push(data, len);
    }
}

static int flash_store_peek_locked(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index)
{
    int ret = 0;
    if (xFlashMutex && xSemaphoreTake(xFlashMutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        ret = flash_store_peek(out, max_len, out_len, out_index);
        xSemaphoreGive(xFlashMutex);
        return ret;
    }
    return flash_store_peek(out, max_len, out_len, out_index);
}

static void flash_store_mark_sent_locked(uint32_t index)
{
    if (xFlashMutex && xSemaphoreTake(xFlashMutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        flash_store_mark_sent(index);
        xSemaphoreGive(xFlashMutex);
    }
    else
    {
        flash_store_mark_sent(index);
    }
}

static IWDG_HandleTypeDef hiwdg;
static volatile uint32_t wd_expected_mask = 0;
static volatile uint32_t wd_heartbeat_mask = 0;
static volatile uint8_t wd_started = 0;

static inline void wd_set_expected(uint32_t mask)
{
    taskENTER_CRITICAL();
    wd_expected_mask |= mask;
    taskEXIT_CRITICAL();
}

static inline void wd_heartbeat(uint32_t mask)
{
    taskENTER_CRITICAL();
    wd_heartbeat_mask |= mask;
    taskEXIT_CRITICAL();
}

#define WD_BIT_NET   (1U << 0)
#define WD_BIT_AP    (1U << 1)
#define WD_BIT_LCD   (1U << 2)
#define WD_BIT_STAT  (1U << 3)
#define WD_BIT_DHT   (1U << 4)

TIM_HandleTypeDef htim2;

/**
 * @brief       ???? FreeRTOS ??????????? (TIM2)
 *              TIM2 ?? 32 ??????????????????????????
 *              APB1 ???? 45MHz??????????? 90MHz
 *              ????? 20kHz ??? (50us ????)?????????? (1kHz) ?? 20 ??
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

    Stm32_Clock_Init(360,25,2,8);   // ????????? 180MHz
    delay_init(180);                // ????????????
    uart_init(115200);              // ?????????
    LED_Init();                      // ????? LED
    KEY_Init();                     // ?????????
    SDRAM_Init();                   // ????? SDRAM
	
	printf("reset\r\n");

    // ???????????
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
    xAP3216CQueueForMQTT = xQueueCreate(5, sizeof(AP3216C_Data_t)); // ???? MQTT ??????
    xUartRxQueue = xQueueCreate(10, sizeof(uint16_t)); // UART ???????
    xLCDMutex = xSemaphoreCreateMutex();
    xFlashMutex = xSemaphoreCreateMutex();

    cJSON_Hooks hooks;
    hooks.malloc_fn = pvPortMalloc;
    hooks.free_fn = vPortFree;
    cJSON_InitHooks(&hooks);

    // ???????????
    if (xDHT11Queue == NULL || xAP3216CQueue == NULL || xLCDMutex == NULL || xUartRxQueue == NULL || xFlashMutex == NULL) {
        printf("????/????????????\r\n");
        // ???????????????????
    }

    // ????????????
    xTaskCreate(init_task,   "init_task",   INIT_STK_SIZE, NULL, INIT_TASK_PRIO, &InitTask_Handler);
    xTaskCreate(dht11_task,  "dht11_task",  DHT11_STK_SIZE, NULL, DHT11_TASK_PRIO, &DHT11Task_Handler);
    xTaskCreate(ap3216c_task,"ap3216c_task",AP3216C_STK_SIZE, NULL, AP3216C_TASK_PRIO, &AP3216CTask_Handler);
    xTaskCreate(lcd_task,    "lcd_task",    LCD_STK_SIZE, NULL, LCD_TASK_PRIO, &LCDTask_Handler);
    xTaskCreate(net_task,    "net_task",    NET_STK_SIZE, NULL, NET_TASK_PRIO, &NETTask_Handler);
    xTaskCreate(run_time_stats_task, "stats_task", RUN_TIME_STATS_STK_SIZE, NULL, RUN_TIME_STATS_TASK_PRIO, &RunTimeStatsTask_Handler);
   // xTaskCreate(watchdog_task, "watchdog", WATCHDOG_STK_SIZE, NULL, WATCHDOG_TASK_PRIO, &WatchdogTask_Handler);
   // xTaskCreate(data_process_task,   "data_process", DATA_PROCESS_STK_SIZE, NULL, DATA_PROCESS_TASK_PRIO, &DataProcessTask_Handler);

    // ??????????
    vTaskDelete(NULL);
}

void init_task(void *pv)
{
    u8 ret;
    char ip_buf[16];
    bool  link_status; // WiFi ??????
    // ✅ 删除：bool con_status;  (这是全局变量，不应该在这里重新声明！)

    LCD_Init();                     // LCD ?????
    PCF8574_Init();                 // PCF8574 ???IO?????

    W25QXX_Init();
    uint16_t flash_id = W25QXX_ReadID();
    printf("W25Q ID: 0x%04X\r\n", flash_id);
    if (flash_id == W25Q256 || flash_id == W25Q128 || flash_id == W25Q64)
    {
        // 强制不使能Flash，防止过度读写
        // flash_ready = 1; 
    }
    // flash_store_init(); // 注释掉Flash的初始化扫描

    // ??? PCF8574 ???IO ?????????DHT11 ????????
    PCF8574_ReadBit(BEEP_IO);

    // ????? AP3216C??????? LCD ???????
    while(AP3216C_Init())
    {
        LCD_ShowString(30,190,200,16,16,"AP3216C Check Failed!");
        delay_xms(500);
        LCD_ShowString(30,190,200,16,16,"Please Check!        ");
        delay_xms(500);
    }    
    
    // ?????????
    POINT_COLOR=BLUE;
    LCD_ShowString(30,150,200,16,16,"Temp:  C");    
    LCD_ShowString(30,170,200,16,16,"Humi:  %");
    
    POINT_COLOR=RED;
    LCD_ShowString(30,190,200,16,16,"AP3216C Ready!");  
    POINT_COLOR=BLUE;
    LCD_ShowString(30,220,200,16,16," IR:");     
    LCD_ShowString(30,250,200,16,16," PS:");    
    LCD_ShowString(30,280,200,16,16,"ALS:");

    printf("???????? WiFi...\r\n");

    /* ????? ATK-MW8266D WiFi ??? */
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
    
    printf("???????? AP...\r\n");
    ret  = atk_mw8266d_restore();                               /* ??????????? */
    ret += atk_mw8266d_at_test();                               /* AT ???? */
    ret += atk_mw8266d_set_mode(1);                             /* ???? Station ?? */
    ret += atk_mw8266d_sw_reset();                              /* ???????? */
    ret += atk_mw8266d_ate_config(0);                           /* ?????? */
    ret += atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD);  /* ???? WiFi */
    ret += atk_mw8266d_get_ip(ip_buf);                          /* ??? IP ??? */
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
        wd_heartbeat(WD_BIT_DHT);
			
			
        // // ??? PCF8574 ??? IO????? DHT11 ????? IO ????
        //  PCF8574_ReadBit(BEEP_IO);

        // // ??? DHT11 ?????????
        //  DHT11_Read_Data(&dht11_data.temperature,&dht11_data.humidity);
    
        // // ?????????
        //  xQueueSend(xDHT11Queue,&dht11_data,0);

        // demo_upload_data(1); // ???? net_task ?????? ATK-MW8266D UART ????

        vTaskDelay(pdMS_TO_TICKS(2000)); // ??? 2 ?????? MQTT ??????????
    }


    
}

void ap3216c_task(void *pv)
{
    AP3216C_Data_t ap3216c_data;
    char json_buf[96];

    while(1)
    {
        wd_heartbeat(WD_BIT_AP);
        // ??? AP3216C ?????????????????
        AP3216C_ReadData(&ap3216c_data.ir,&ap3216c_data.ps,&ap3216c_data.als);

        // ✅ 修复：只发送给两个队列，不在这里存 Flash
        // LCD ?????
        xQueueSend(xAP3216CQueue,&ap3216c_data,0);
        
        // MQTT ????
        BaseType_t qret = xQueueSend(xAP3216CQueueForMQTT, &ap3216c_data, 0);
        if (!g_mqtt_connected || qret != pdTRUE)
        {
            int n = snprintf(json_buf, sizeof(json_buf), "{\"als\":%u,\"ir\":%u,\"ps\":%u}",
                             ap3216c_data.als, ap3216c_data.ir, ap3216c_data.ps);
            if (n > 0 && n < (int)sizeof(json_buf))
            {
                // 注释掉该行，停止Flash写入
                // flash_store_push_locked(json_buf, (uint16_t)n);
            }
        }

        uint32_t period_ms = g_ap_sample_period_ms;
        if (period_ms < AP_SAMPLE_PERIOD_MIN_MS) period_ms = AP_SAMPLE_PERIOD_MIN_MS;
        if (period_ms > AP_SAMPLE_PERIOD_MAX_MS) period_ms = AP_SAMPLE_PERIOD_MAX_MS;
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}

void lcd_task(void *pv)
{
    DHT11_Data_t dht11_data;
    AP3216C_Data_t ap3216c_data;

    while(1)
    {
        wd_heartbeat(WD_BIT_LCD);
        // ???? DHT11 ?????????? LCD
        if(xQueueReceive(xDHT11Queue,&dht11_data,pdMS_TO_TICKS(100))==pdTRUE){
             if(xSemaphoreTake(xLCDMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                 // ??????
                 LCD_Fill(30+40, 150, 30+40+16, 150+16, WHITE);
                 LCD_ShowNum(30+40, 150, dht11_data.temperature, 2, 16);
                 
                 // ??????
                 LCD_Fill(30+40, 170, 30+40+16, 170+16, WHITE);
                 LCD_ShowNum(30+40, 170, dht11_data.humidity, 2, 16);
                 
                 xSemaphoreGive(xLCDMutex);
             }
        }

        // ???? AP3216C ?????????? LCD
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
        
        vTaskDelay(pdMS_TO_TICKS(50));  // ??? 50ms
    }
}

typedef struct
{
    AP3216C_Data_t ap_data;
    u8 key;
    char json_buf[256];
    uint16_t rx_len;
    uint8_t *uart_buf;
    uint32_t backoff_ms;
    uint32_t max_backoff_ms;
    TickType_t next_retry_tick;
    uint8_t auto_reconnect;
    TickType_t last_rx_tick;
    TickType_t last_ping_tick;
    TickType_t ping_interval_tick;
    TickType_t ping_timeout_tick;
    uint8_t waiting_pingresp;
    uint32_t cached_total;
    uint32_t sent_total;
    TickType_t last_report_tick;
    uint8_t wifi_rejoin_needed;
    TickType_t wifi_retry_tick;
    MQTTPacket_connectData connect_data;
    unsigned char sessionPresent;
    unsigned char connack_rc;
    int packet_len;
    MQTTString topic_string;
    MQTTString subscribe_topic;
    int granted_qos[1];
    int sub_count;
    unsigned short sub_packet_id;
    unsigned char dup;
    int qos;
    unsigned char retained;
    unsigned short packet_id;
    unsigned char *payload;
    int payload_len;
} NetTaskContext;

static void net_ctx_init(NetTaskContext *ctx)
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

static void net_print_stats(NetTaskContext *ctx, TickType_t now)
{
    if ((now - ctx->last_report_tick) > pdMS_TO_TICKS(5000))
    {
        printf("cache_total=%lu, sent_total=%lu, pending=%lu\r\n",
               ctx->cached_total, ctx->sent_total, (uint32_t)flash_valid_count);
        ctx->last_report_tick = now;
    }
}

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

static uint8_t net_try_connect(NetTaskContext *ctx, TickType_t now, uint8_t request_connect)
{
    if (con_status && mqtt_sock >= 0) return 1;
    g_mqtt_connected = 0;
    uint8_t can_try = request_connect;
    if (!can_try && ctx->auto_reconnect)
    {
        if (ctx->next_retry_tick == 0 || now >= ctx->next_retry_tick)
        {
            can_try = 1;
        }
    }
    if (!can_try) return 0;
    if (ctx->wifi_rejoin_needed && (ctx->wifi_retry_tick == 0 || now >= ctx->wifi_retry_tick))
    {
        uint8_t join_ret = atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD);
        if (join_ret == ATK_MW8266D_EOK)
        {
            ctx->wifi_rejoin_needed = 0;
            ctx->wifi_retry_tick = 0;
        }
        else
        {
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000);
            return 0;
        }
    }
    if (atk_mw8266d_get_ip(ip_buf) != ATK_MW8266D_EOK)
    {
        uint8_t join_ret = atk_mw8266d_join_ap(DEMO_WIFI_SSID, DEMO_WIFI_PWD);
        if (join_ret != ATK_MW8266D_EOK)
        {
            ctx->wifi_rejoin_needed = 1;
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000);
            return 0;
        }
    }
    atk_mw8266d_uart_rx_restart();
    mqtt_sock = transport_open(MQTT_BROKER_IP, atoi(MQTT_BROKER_PORT));
    if (mqtt_sock < 0)
    {
        printf("Transport Open Failed!\r\n");
        con_status = 0;
        ctx->wifi_rejoin_needed = 1;
        ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000);
    }
    else
    {
        printf("TCP Connected, Entered Transparent Mode.\r\n");
        delay_ms(500);
        net_build_connect(ctx);
        ctx->packet_len = MQTTSerialize_connect(mqtt_send_buf, sizeof(mqtt_send_buf), &ctx->connect_data);
        if (ctx->packet_len <= 0 || transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len) != ctx->packet_len)
        {
            transport_close(mqtt_sock);
            mqtt_sock = -1;
            con_status = 0;
        }
        else
        {
            ctx->packet_len = MQTTPacket_read(mqtt_recv_buf, sizeof(mqtt_recv_buf), transport_getdata);
            if (ctx->packet_len > 0 && MQTTDeserialize_connack(&ctx->sessionPresent, &ctx->connack_rc, mqtt_recv_buf, ctx->packet_len))
            {
                if(ctx->connack_rc == MQTT_CONNECTION_ACCEPTED)
                {
                    printf("MQTT Connected!\r\n");
                    con_status = 1;
                    g_mqtt_connected = 1;
                    ctx->backoff_ms = 1000;
                    ctx->next_retry_tick = 0;
                    ctx->last_rx_tick = now;
                    ctx->last_ping_tick = now;
                    ctx->waiting_pingresp = 0;
                    ctx->subscribe_topic.cstring = MQTT_TOPIC_SUB;
                    int req_qos[1] = {0};
                    ctx->packet_len = MQTTSerialize_subscribe(mqtt_send_buf, sizeof(mqtt_send_buf), 0, ctx->sub_packet_id++, 1, &ctx->subscribe_topic, req_qos);
                    if (ctx->packet_len > 0)
                    {
                        transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len);
                        printf("MQTT Subscribe Sent.\r\n");
                    }
                }
                else
                {
                    // 加上这句打印，看看到底是什么原因被踢下线！
                    printf("MQTT Connect Rejected by Server! RC=%d\r\n", ctx->connack_rc);
                    transport_close(mqtt_sock);
                    mqtt_sock = -1;
                    con_status = 0;
                    g_mqtt_connected = 0;
                }
            }
            else // 读取超时或者接收包错误
            {
                printf("MQTT Receive CONNACK Timeout or Error!\r\n");
                transport_close(mqtt_sock);
                mqtt_sock = -1;
                con_status = 0;
                g_mqtt_connected = 0;
            }
        }
    }
    if (!con_status && ctx->auto_reconnect)
    {
        if (ctx->backoff_ms < ctx->max_backoff_ms) ctx->backoff_ms <<= 1;
        if (ctx->backoff_ms > ctx->max_backoff_ms) ctx->backoff_ms = ctx->max_backoff_ms;
        ctx->next_retry_tick = now + pdMS_TO_TICKS(ctx->backoff_ms);
    }
    return (con_status && mqtt_sock >= 0) ? 1 : 0;
}

static void net_cache_offline_data(NetTaskContext *ctx)
{
    //这个是什么意思这个队列？
    while (xQueueReceive(xAP3216CQueueForMQTT, &ctx->ap_data, 0) == pdTRUE)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "als", ctx->ap_data.als);
        cJSON_AddNumberToObject(root, "ir", ctx->ap_data.ir);
        cJSON_AddNumberToObject(root, "ps", ctx->ap_data.ps);
        if (cJSON_PrintPreallocated(root, ctx->json_buf, sizeof(ctx->json_buf), 0))
        {
            // 注释掉数据入库，防止消耗Flash寿命
            // flash_store_push_locked(ctx->json_buf, (uint16_t)strlen(ctx->json_buf));
            ctx->cached_total++;
            printf("Data Ignored! (cached=%lu, pending=%lu)\r\n",
                   ctx->cached_total, flash_valid_count);
        }
        cJSON_Delete(root);
    }
}

static uint8_t net_publish_flash_backlog(NetTaskContext *ctx)
{
    // 屏蔽掉重传Flash存余数据的逻辑，直接返回1表示无滞留数据
    return 1;
    
    /*
    if (flash_valid_count == 0) return 1;
    uint16_t cached_len = 0;
    uint32_t rec_index = 0;
    if (!flash_store_peek_locked(ctx->json_buf, sizeof(ctx->json_buf), &cached_len, &rec_index)) return 1;
    printf("Read from Flash: len=%d, pending=%lu\r\n", cached_len, flash_valid_count);
    MQTTString pub_topic = MQTTString_initializer;
    pub_topic.cstring = MQTT_TOPIC_PUB;
    ctx->packet_len = MQTTSerialize_publish(mqtt_send_buf, sizeof(mqtt_send_buf), 0, 0, 0, 0,
                                            pub_topic, (unsigned char*)ctx->json_buf, cached_len);
    if (ctx->packet_len > 0)
    {
        if (transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len) == ctx->packet_len)
        {
            printf("Flash Data Sent OK (pending=%lu)\r\n", flash_valid_count);
            flash_store_mark_sent_locked(rec_index);
            ctx->sent_total++;
            return 1;
        }
        printf("Send Flash Data Failed!\r\n");
        transport_close(mqtt_sock);
        mqtt_sock = -1;
        con_status = 0;
        g_mqtt_connected = 0;
        return 0;
    }
    return 1;
    */
}

static void net_keepalive_step(NetTaskContext *ctx, TickType_t now)
{
    if (!(con_status && mqtt_sock >= 0)) return;
    if (ctx->waiting_pingresp)
    {
        if ((now - ctx->last_ping_tick) > ctx->ping_timeout_tick)
        {
            printf("MQTT Ping Timeout!\r\n");
            transport_close(mqtt_sock);
            mqtt_sock = -1;
            con_status = 0;
            ctx->waiting_pingresp = 0;
            g_mqtt_connected = 0;
            ctx->wifi_rejoin_needed = 1;
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000);
        }
    }
    else if ((now - ctx->last_rx_tick) > ctx->ping_interval_tick)
    {
        ctx->packet_len = MQTTSerialize_pingreq(mqtt_send_buf, sizeof(mqtt_send_buf));
        if (ctx->packet_len > 0 && transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ctx->packet_len) == ctx->packet_len)
        {
            ctx->waiting_pingresp = 1;
            ctx->last_ping_tick = now;
        }
        else
        {
            printf("MQTT Ping Send Failed!\r\n");
            transport_close(mqtt_sock);
            mqtt_sock = -1;
            con_status = 0;
            ctx->waiting_pingresp = 0;
            g_mqtt_connected = 0;
            ctx->wifi_rejoin_needed = 1;
            ctx->wifi_retry_tick = now + pdMS_TO_TICKS(5000);
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

static uint8_t json_to_switch_state(cJSON *item, uint8_t *state, uint8_t *toggle)
{
    *toggle = 0;
    if (item == NULL) return 0;
    if (cJSON_IsBool(item))
    {
        *state = cJSON_IsTrue(item) ? 1 : 0;
        return 1;
    }
    if (cJSON_IsNumber(item))
    {
        *state = (item->valueint != 0) ? 1 : 0;
        return 1;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        if (strcmp(item->valuestring, "on") == 0 || strcmp(item->valuestring, "ON") == 0 || strcmp(item->valuestring, "1") == 0)
        {
            *state = 1;
            return 1;
        }
        if (strcmp(item->valuestring, "off") == 0 || strcmp(item->valuestring, "OFF") == 0 || strcmp(item->valuestring, "0") == 0)
        {
            *state = 0;
            return 1;
        }
        if (strcmp(item->valuestring, "toggle") == 0 || strcmp(item->valuestring, "TOGGLE") == 0)
        {
            *toggle = 1;
            return 1;
        }
    }
    return 0;
}

static uint8_t json_to_u32_ms(cJSON *item, uint32_t *out_ms)
{
    if (item == NULL || out_ms == NULL) return 0;
    uint32_t v = 0;
    if (cJSON_IsNumber(item))
    {
        if (item->valuedouble <= 0) return 0;
        v = (uint32_t)item->valuedouble;
    }
    else if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        int n = atoi(item->valuestring);
        if (n <= 0) return 0;
        v = (uint32_t)n;
    }
    else
    {
        return 0;
    }
    if (v < AP_SAMPLE_PERIOD_MIN_MS) v = AP_SAMPLE_PERIOD_MIN_MS;
    if (v > AP_SAMPLE_PERIOD_MAX_MS) v = AP_SAMPLE_PERIOD_MAX_MS;
    *out_ms = v;
    return 1;
}

static void data_process_mqtt_msg(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0) 
    {
        printf("error: invalid buffer or length\r\n");
        return;

    }
    int start = -1;
    int end = -1;
    for (uint16_t i = 0; i < len; i++)
    {
        if (buf[i] == '{')
        {
            start = i;
            break;
        }
    }
    if (start < 0) 
    {
        printf("error: no JSON start found\r\n");
        return;
    }
    for (int i = (int)len - 1; i > start; i--)
    {
        if (buf[i] == '}')
        {
            end = i;
            break;
        }
    }
    if (end <= start) 
    {
        printf("error: no JSON end found\r\n");
        return;
    }

    int json_len = end - start + 1;
    if (json_len <= 0) 
    {
        printf("error: invalid JSON length\r\n");
        return;
    }
    if (json_len > 255) json_len = 255;
    char json_text[256];
    memcpy(json_text, buf + start, json_len);
    json_text[json_len] = '\0';

    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        printf("error: JSON parse failed\r\n");
        return;
    }

    cJSON *control = cJSON_GetObjectItemCaseSensitive(root, "control");
    cJSON *node = (control != NULL && cJSON_IsObject(control)) ? control : root;

    uint8_t state = 0;
    uint8_t toggle = 0;
    cJSON *item = NULL;

    item = cJSON_GetObjectItemCaseSensitive(node, "led0");
    if (json_to_switch_state(item, &state, &toggle))
    {
        if (toggle) LED0 = !LED0;
        else LED0 = state;
        printf("CTRL led0=%d\r\n", LED0);
    }

    item = cJSON_GetObjectItemCaseSensitive(node, "led1");
    if (json_to_switch_state(item, &state, &toggle))
    {
        if (toggle) LED1 = !LED1;
        else LED1 = state;
        printf("CTRL led1=%d\r\n", LED1);
    }

    item = cJSON_GetObjectItemCaseSensitive(node, "beep");
    if (json_to_switch_state(item, &state, &toggle))
    {
        uint8_t cur = PCF8574_ReadBit(BEEP_IO);
        if (toggle) state = cur ? 0 : 1;
        PCF8574_WriteBit(BEEP_IO, state);
        printf("CTRL beep=%d\r\n", state);
    }

    uint32_t new_period_ms = 0;
    item = cJSON_GetObjectItemCaseSensitive(node, "sample_ms");
    if (!json_to_u32_ms(item, &new_period_ms))
    {
        item = cJSON_GetObjectItemCaseSensitive(node, "sample_period_ms");
        json_to_u32_ms(item, &new_period_ms);
    }
    if (new_period_ms > 0)
    {
        g_ap_sample_period_ms = new_period_ms;
        printf("CTRL sample_ms=%lu\r\n", (unsigned long)g_ap_sample_period_ms);
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (cJSON_IsString(cmd) && cmd->valuestring != NULL && json_to_switch_state(value, &state, &toggle))
    {
        if (strcmp(cmd->valuestring, "led0") == 0)
        {
            if (toggle) LED0 = !LED0;
            else LED0 = state;
            printf("CTRL led0=%d\r\n", LED0);
        }
        else if (strcmp(cmd->valuestring, "led1") == 0)
        {
            if (toggle) LED1 = !LED1;
            else LED1 = state;
            printf("CTRL led1=%d\r\n", LED1);
        }
        else if (strcmp(cmd->valuestring, "beep") == 0)
        {
            uint8_t cur = PCF8574_ReadBit(BEEP_IO);
            if (toggle) state = cur ? 0 : 1;
            PCF8574_WriteBit(BEEP_IO, state);
            printf("CTRL beep=%d\r\n", state);
        }
        else if (strcmp(cmd->valuestring, "sample_ms") == 0 || strcmp(cmd->valuestring, "sample_period_ms") == 0)
        {
            if (json_to_u32_ms(value, &new_period_ms))
            {
                g_ap_sample_period_ms = new_period_ms;
                printf("CTRL sample_ms=%lu\r\n", (unsigned long)g_ap_sample_period_ms);
            }
        }
    }

    cJSON_Delete(root);
}
static void net_receive_mqtt_packets(NetTaskContext *ctx, TickType_t now)
{
    ctx->packet_len = transport_getdatanb(NULL, mqtt_recv_buf, sizeof(mqtt_recv_buf));
    if (ctx->packet_len <= 0) return;
    ctx->last_rx_tick = now;
    int offset = 0;
    while (offset < ctx->packet_len)
    {
        int rem_len = 0;
        int multiplier = 1;
        int i = 1;
        int packet_len = 0;
        unsigned char* curr_buf = mqtt_recv_buf + offset;
        if (ctx->packet_len - offset < 2) break;
        do {
            if (i >= (ctx->packet_len - offset)) { packet_len = 0; break; }
            unsigned char c = curr_buf[i++];
            rem_len += (c & 127) * multiplier;
            multiplier *= 128;
            if (multiplier > 2097152) { packet_len = 0; break; }
            if ((c & 128) == 0) {
                packet_len = i + rem_len;
                break;
            }
        } while (1);
        if (packet_len <= 0 || packet_len > (ctx->packet_len - offset)) break;
        if (MQTTDeserialize_publish(&ctx->dup, &ctx->qos, &ctx->retained, &ctx->packet_id, &ctx->topic_string,
                                    &ctx->payload, &ctx->payload_len, curr_buf, packet_len))
        {
            printf("MQTT Recv: Topic=%.*s, Payload=%.*s\r\n",
                   ctx->topic_string.lenstring.len, ctx->topic_string.lenstring.data,
                   ctx->payload_len, ctx->payload);
                   data_process_mqtt_msg(ctx->payload, ctx->payload_len);
            if (ctx->qos > 0)
            {
                int ack_len = MQTTSerialize_ack(mqtt_send_buf, sizeof(mqtt_send_buf), PUBACK, 0, ctx->packet_id);
                if (ack_len > 0) transport_sendPacketBuffer(mqtt_sock, mqtt_send_buf, ack_len);
            }
        }
        else if (MQTTDeserialize_suback(&ctx->packet_id, 1, &ctx->sub_count, ctx->granted_qos, curr_buf, packet_len))
        {
            printf("MQTT Subscribe ACK Received.\r\n");
        }
        else
        {
            uint8_t pkt_type = curr_buf[0] >> 4;
            if (pkt_type == 13) {
                ctx->waiting_pingresp = 0;
                ctx->last_rx_tick = now;
            } else if (pkt_type == 9) {
                printf("MQTT SUBACK Received\r\n");
            } else {
                printf("MQTT RX Ignored (Type=%d, Len=%d)\r\n", pkt_type, packet_len);
            }
        }
        offset += packet_len;
    }
}




static void net_handle_uart_rx(NetTaskContext *ctx)
{
    if (xQueueReceive(xUartRxQueue, &ctx->rx_len, 0) != pdTRUE) return;
    ctx->uart_buf = atk_mw8266d_uart_rx_get_frame();
    if (ctx->uart_buf != NULL)
    {
        printf("UART Msg: %s", ctx->uart_buf);

        // 处理接收到的 UART 数据
       // data_process_uart_msg(ctx->uart_buf, ctx->rx_len);

        atk_mw8266d_uart_rx_restart();
    }
}

void net_task(void *pv)
{
    NetTaskContext ctx;
    net_ctx_init(&ctx);

    while (1)
    {
        wd_heartbeat(WD_BIT_NET);
        ctx.key = KEY_Scan(0);
        TickType_t now = xTaskGetTickCount();
        uint8_t request_connect = 0;

        net_print_stats(&ctx, now);
        net_handle_key(&ctx, now, &request_connect);

        if (!net_try_connect(&ctx, now, request_connect))
        {
            net_cache_offline_data(&ctx);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!net_publish_flash_backlog(&ctx))
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        net_keepalive_step(&ctx, now);
        net_publish_realtime_data(&ctx);
        net_receive_mqtt_packets(&ctx, now);
        net_handle_uart_rx(&ctx);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief       FreeRTOS ????????????
 *              ??????????????????????CPU?????
 */                                                                                                   
void run_time_stats_task(void *pv)
{
    char pcWriteBuffer[512];
    
    while (1)
    {
       // wd_heartbeat(WD_BIT_STAT);
        
        vTaskGetRunTimeStats(pcWriteBuffer);

        printf("TaskName\tAbsTime\t\tTime%%\r\n%s\r\n", pcWriteBuffer);
        
        // ? 5 ???????
        vTaskDelay(pdMS_TO_TICKS(5000));

    }
}

void watchdog_task(void *pv)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    wd_set_expected(WD_BIT_NET | WD_BIT_AP | WD_BIT_LCD | WD_BIT_DHT);
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
    hiwdg.Init.Reload = 3500;//7秒超时
    HAL_IWDG_Init(&hiwdg);
    HAL_IWDG_Start(&hiwdg);
    wd_started = 1;
    int miss = 0;
    while (1)
    {
        uint32_t mask = wd_heartbeat_mask;
        if ((mask & wd_expected_mask) == wd_expected_mask)
        {
            HAL_IWDG_Refresh(&hiwdg);
            wd_heartbeat_mask = 0;
            miss = 0;
        }
        else
        {
            miss++;
            if (miss >= 8)
            {
                //遗言，查看哪个任务死了
                while (1) { 
                    printf("watchdog timeout\r\n");
                    vTaskDelay(pdMS_TO_TICKS(1000)); 
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/**
 * @brief       ??????????? (???)
 *              ??????? UART ???????????????????????
 */

void data_process_task(void *pv)
{
    uint16_t rx_len;
    uint8_t *buf;

    while (1)
    {
        // ???????????? UART ??????????????? (??????demo_upload_data?????)
       

        vTaskDelay(pdMS_TO_TICKS(2000));
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