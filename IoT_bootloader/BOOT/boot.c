#include "boot.h"
#include <string.h>
#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "stm32f429xx.h"
#include "core_cm4.h"

// 为了兼容正点原子或者老工程里的 u32, u8 定义
#include "sys.h" 

#include "flash.h"
#include "delay.h"

typedef void (*Jump_Fun)(void);

void Load_App(u32 App_Addr)
{
    Jump_Fun JumpToApp;

    // Check if the stack pointer is in the valid range
    if ( ( ( * ( (__IO u32 *) App_Addr ) ) & 0x2FFE0000 ) == 0x20000000 )
    {
        // // ========== 新增：跳转前的底层清场工作 ==========
        // __disable_irq();           // 1. 禁用所有外部中断
        
        // SysTick->CTRL = 0;         // 2. 取消 SysTick 定时器
        // SysTick->LOAD = 0;
        // SysTick->VAL = 0;
        
        // HAL_DeInit();              // 3. 复位 HAL 库底层硬件状态
        // // ===============================================

        JumpToApp = (Jump_Fun)*(__IO u32 *)(App_Addr + 4);
        
        MSR_MSP( * ( __IO unsigned int * ) App_Addr );								//?????APP??????(?????????????????????????????)
		//SCB->VTOR = App_Addr; 没用，跳转后会复位，要在APP里面执行这个
        JumpToApp();
    }
}



u8 check_flash_flag(u32 app_addr)
{
    OtaBootMeta meta = {0};

    if (OTA_ReadBootMeta(&meta, sizeof(meta)))
    {
        
            if (meta.state == OTA_META_STATE_PENDING)
            {
                // 有待处理的镜像
                return 1;
            }
        
    }
    return 0;
}