#ifndef __WDOG_H
#define __WDOG_H

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stm32f4xx_hal.h"


#define WD_BIT_NET   (1U << 0)
#define WD_BIT_AP    (1U << 1)
#define WD_BIT_LCD   (1U << 2)
#define WD_BIT_STAT  (1U << 3)
#define WD_BIT_DHT   (1U << 4)

extern IWDG_HandleTypeDef hiwdg;
extern volatile uint32_t wd_expected_mask;
extern volatile uint32_t wd_heartbeat_mask;
extern volatile uint8_t wd_started;


void watchdog_task(void *pv);





 inline void wd_set_expected(uint32_t mask);

  void wd_heartbeat(uint32_t mask);


 
#endif
