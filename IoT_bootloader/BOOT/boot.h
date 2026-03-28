#ifndef __BOOT_H
#define __BOOT_H

#include "usart.h"
#include "delay.h"
#include <stm32f4xx_hal_flash.h>

#include "flash.h"

#define BootLoader_addr      0x08000000U
#define BootLoader_Size      0x00010000U
#define META_Flag_Addr       0x08100000U
#define Application_1_Addr   0x08020000U
#define Application_2_Addr   0x08120000U 
#define Application_Size     0x00080000U

//extern OtaBootMeta meta;

void Load_App(unsigned int App_Addr);

uint8_t check_flash_flag(uint32_t app_addr);



#endif
