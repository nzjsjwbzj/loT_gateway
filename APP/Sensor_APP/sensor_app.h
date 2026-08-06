#ifndef __SENSOR_APP_H
#define __SENSOR_APP_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "ap3216c.h"
#include "dht11.h"

#define AP_SAMPLE_PERIOD_MIN_MS 200
#define AP_SAMPLE_PERIOD_MAX_MS 60000

typedef struct 
{
    u8 temperature;
    u8 humidity;
} DHT11_Data_t;

//自定义
typedef struct
{
    u16 ir;
    u16 ps;
    u16 als;
} AP3216C_Data_t;


void dht11_task(void *pv);
void ap3216c_task(void *pv);

extern QueueHandle_t xDHT11Queue;
extern QueueHandle_t xAP3216CQueue;
extern QueueHandle_t xAP3216CQueueForMQTT; // MQTT专用发送队列

extern volatile uint32_t g_ap_sample_period_ms;

#endif
