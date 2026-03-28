#include "wdog.h"



IWDG_HandleTypeDef hiwdg;
volatile uint32_t wd_expected_mask;
volatile uint32_t wd_heartbeat_mask;
volatile uint8_t wd_started;


 inline void wd_set_expected(uint32_t mask)
{
    taskENTER_CRITICAL();
    wd_expected_mask |= mask;
    taskEXIT_CRITICAL();
}

 inline void wd_heartbeat(uint32_t mask)
{
    taskENTER_CRITICAL();
    wd_heartbeat_mask |= mask;
    taskEXIT_CRITICAL();
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
            if (miss >= 8)//8s没得就死了
            {
                //遗言，查看哪个任务死了
                while (1) { 
                    // 取反或者判断等于0，说明这个位没被打卡
                    if (!(mask & WD_BIT_NET)) {
                        printf("net task dead\r\n");
                    }
                    if (!(mask & WD_BIT_AP)) {
                        printf("ap task dead\r\n");
                    }
                    if (!(mask & WD_BIT_LCD)) {
                        printf("lcd task dead\r\n");
                    }
                    if (!(mask & WD_BIT_DHT)) {
                        printf("dht task dead\r\n");
                    }
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}






