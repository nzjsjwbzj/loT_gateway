#include "sensor_app.h"
#include "wdog.h"

// 队列及信号量句柄
QueueHandle_t xDHT11Queue;
QueueHandle_t xAP3216CQueue;
QueueHandle_t xAP3216CQueueForMQTT; // MQTT专用发送队列

volatile uint32_t g_ap_sample_period_ms = 5000;

void dht11_task(void *pv)
{
    DHT11_Data_t dht11_data; 
    
    while (1)
    {
        wd_heartbeat(WD_BIT_DHT);
			
			
        // // 获取 PCF8574 扩展 IO状态和 DHT11 专属 IO 电平
        //  PCF8574_ReadBit(BEEP_IO);

        // // 读取 DHT11 温湿度数值
        //  DHT11_Read_Data(&dht11_data.temperature,&dht11_data.humidity);
    
        // // 将读到的温湿度入队列
        //  xQueueSend(xDHT11Queue,&dht11_data,0);

        // demo_upload_data(1); // 检查 net_task 内部对于 ATK-MW8266D UART 数据的解析

        vTaskDelay(pdMS_TO_TICKS(2000)); // 睡眠 2 秒钟等待下一次 MQTT 采集或温湿度转换
    }


    
}

void ap3216c_task(void *pv)
{
    AP3216C_Data_t ap3216c_data;
    char json_buf[96];

    while(1)
    {
        wd_heartbeat(WD_BIT_AP);
        // 读取 AP3216C 环境光、红外和接近距离数据
        AP3216C_ReadData(&ap3216c_data.ir,&ap3216c_data.ps,&ap3216c_data.als);

        // ✅ 修复：只发送给两个队列，不在这里存 Flash
        // 传递给 LCD 界面刷新任务
        xQueueSend(xAP3216CQueue,&ap3216c_data,0);
        
        // 传递给 MQTT 网络上报任务
        BaseType_t qret = xQueueSend(xAP3216CQueueForMQTT, &ap3216c_data, 0);
        if (!g_mqtt_connected || qret != pdTRUE)
        {
            int n = snprintf(json_buf, sizeof(json_buf), "{\"als\":%u,\"ir\":%u,\"ps\":%u}",
                             ap3216c_data.als, ap3216c_data.ir, ap3216c_data.ps);
            if (n > 0 && n < (int)sizeof(json_buf))
            {
                // 注释掉该行，停止Flash写入
                 flash_store_push_locked(json_buf, (uint16_t)n);
            }
        }

        uint32_t period_ms = g_ap_sample_period_ms;
        if (period_ms < AP_SAMPLE_PERIOD_MIN_MS) period_ms = AP_SAMPLE_PERIOD_MIN_MS;
        if (period_ms > AP_SAMPLE_PERIOD_MAX_MS) period_ms = AP_SAMPLE_PERIOD_MAX_MS;
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }
}