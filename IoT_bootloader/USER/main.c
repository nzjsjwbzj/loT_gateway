#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "boot.h"
#include "flash.h"
/************************************************
 ALIENTEK 阿波罗STM32F429开发板实验0-1
 Template工程模板-新建工程章节使用-HAL库版本
 技术支持：www.openedv.com
 淘宝店铺： http://eboard.taobao.com 
 关注微信公众平台微信号："正点原子"，免费获取STM32资料。
 广州市星翼电子科技有限公司  
 作者：正点原子 @ALIENTEK
************************************************/


/***注意：本工程和教程中的新建工程3.3小节对应***/




int main(void)
{

	
    HAL_Init();                     //初始化HAL库    
    Stm32_Clock_Init(360,25,2,8);   //设置时钟,180Mhz
		delay_init(360);
		uart_init(115200);
		

	printf("Bootloader Start\r\n");
    // 加入延时，等待所有外设稳定
    delay_ms(500);

	if(check_flash_flag(META_Flag_Addr))
	{// 单片机重启后，最先运行的是 
// Bootloader（在 0x08000000）。Bootloader 开机
// 第一件事就是去检查 0x08100000。 它一旦发现那里
// 写着 0x54414F31 且状态是 PENDING，就会启动你的 
// MoveCode 函数，把 0x08120000 里的新代码，原封不
// 动地搬运覆盖到当前工作区 0x08020000 (Applicati
//     on 1)。搬完后擦除 META 标志，启动新 App，整
//     个 OTA 彻底完成。
		// 有待处理的镜像，执行 OTA 更新
		printf("Pending image found, starting OTA update...\r\n");
		MoveCode(Application_2_Addr, Application_1_Addr, Application_Size);
		OTA_ClearPendingImage();
	}

	else
	{
		printf("no no no\r\n");

	}

	// 启动应用程序
	printf("start now\r\n");
	Load_App(Application_1_Addr);

}
