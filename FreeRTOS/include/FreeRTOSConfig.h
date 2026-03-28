/*
    FreeRTOS V9.0.0 - Copyright (C) 2016 Real Time Engineers Ltd.
    All rights reserved

    VISIT http://www.FreeRTOS.org TO ENSURE YOU ARE USING THE LATEST VERSION.

    This file is part of the FreeRTOS distribution.

    FreeRTOS is free software; you can redistribute it and/or modify it under
    the terms of the GNU General Public License (version 2) as published by the
    Free Software Foundation >>>> AND MODIFIED BY <<<< the FreeRTOS exception.

    ***************************************************************************
    >>!   NOTE: The modification to the GPL is included to allow you to     !<<
    >>!   distribute a combined work that includes FreeRTOS without being   !<<
    >>!   obliged to provide the source code for proprietary components     !<<
    >>!   outside of the FreeRTOS kernel.                                   !<<
    ***************************************************************************

    FreeRTOS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE.  Full license text is available on the following
    link: http://www.freertos.org/a00114.html

    ***************************************************************************
     *                                                                       *
     *    FreeRTOS provides completely free yet professionally developed,    *
     *    robust, strictly quality controlled, supported, and cross          *
     *    platform software that is more than just the market leader, it     *
     *    is the industry's de facto standard.                               *
     *                                                                       *
     *    Help yourself get started quickly while simultaneously helping     *
     *    to support the FreeRTOS project by purchasing a FreeRTOS           *
     *    tutorial book, reference manual, or both:                          *
     *    http://www.FreeRTOS.org/Documentation                              *
     *                                                                       *
    ***************************************************************************

    http://www.FreeRTOS.org/FAQHelp.html - Having a problem?  Start by reading
    the FAQ page "My application does not run, what could be wrong?".  Have you
    defined configASSERT()?

    http://www.FreeRTOS.org/support - In return for receiving this top quality
    embedded software for free we request you assist our global community by
    participating in the support forum.

    http://www.FreeRTOS.org/training - Investing in training allows your team to
    be as productive as possible as early as possible.  Now you can receive
    FreeRTOS training directly from Richard Barry, CEO of Real Time Engineers
    Ltd, and the world's leading authority on the world's leading RTOS.

    http://www.FreeRTOS.org/plus - A selection of FreeRTOS ecosystem products,
    including FreeRTOS+Trace - an indispensable productivity tool, a DOS
    compatible FAT file system, and our tiny thread aware UDP/IP stack.

    http://www.FreeRTOS.org/labs - Where new FreeRTOS products go to incubate.
    Come and try FreeRTOS+TCP, our new open source TCP/IP stack for FreeRTOS.

    http://www.OpenRTOS.com - Real Time Engineers ltd. license FreeRTOS to High
    Integrity Systems ltd. to sell under the OpenRTOS brand.  Low cost OpenRTOS
    licenses offer ticketed support, indemnification and commercial middleware.

    http://www.SafeRTOS.com - High Integrity Systems also provide a safety
    engineered and independently SIL3 certified version for use in safety and
    mission critical applications that require provable dependability.

    1 tab == 4 spaces!
*/

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "sys.h"

// 针对不同的编译器调用不同的 stdint.h 文件
#if defined(__ICCARM__) || defined(__CC_ARM) || defined(__GNUC__)
    #include <stdint.h>
    extern uint32_t SystemCoreClock;
#endif

// 断言：当表达式为 0 时调用，打印出错误发生的文件名和行号
#define vAssertCalled(pcFile, ulLine) printf("Error:%s,%ld\r\n", pcFile, ulLine)
#define configASSERT(x) if((x)==0) vAssertCalled(__FILE__,__LINE__)

/***************************************************************************************************************/
/*                                        FreeRTOS 基础配置宏                                                  */
/***************************************************************************************************************/
#define configUSE_PREEMPTION					1                       // 1: 使用抢占式调度器; 0: 使用协程式调度器
#define configUSE_TIME_SLICING					1						// 1: 开启时间片轮转（同优先级任务切片调度）
#define configUSE_PORT_OPTIMISED_TASK_SELECTION	1                       // 1: 使用硬件计算前导零指令优化任务查找
                                                                        // 特别适合 STM32 等自带 CLZ 指令的 MCU
                                                                        
#define configUSE_TICKLESS_IDLE					1                       // 1: 开启 Tickless 低功耗模式，空闲时停掉滴答定时器
#define configUSE_QUEUE_SETS					1                       // 1: 开启队列集功能
#define configCPU_CLOCK_HZ						(SystemCoreClock)       // CPU 核心时钟频率
#define configTICK_RATE_HZ						(1000)                  // 操作系统心跳频率 1000Hz (即 1ms 一个 Tick)
#define configMAX_PRIORITIES					(32)                    // 操作系统支持的最大优先级数量 0 ~ 31
#define configMINIMAL_STACK_SIZE				((unsigned short)130)   // 空闲任务分配的最小堆栈大小 (字，非字节)
#define configMAX_TASK_NAME_LEN					(16)                    // 任务名称字符串最大长度

#define configUSE_16_BIT_TICKS					0                       // 0: 使用 32 位的 Tick 计数器，防止系统运行 49 天后溢出
#define configIDLE_SHOULD_YIELD					1                       // 1: 空闲任务遇到同优先级任务时主动让出 CPU
#define configUSE_TASK_NOTIFICATIONS            1                       // 1: 开启直接任务通知功能 (轻量级信号量)
#define configUSE_MUTEXES						1                       // 1: 开启互斥信号量功能
#define configQUEUE_REGISTRY_SIZE				8                       // 可注册的带名称的队列总数 (调试用)
#define configCHECK_FOR_STACK_OVERFLOW			0                       // 0: 关闭堆栈溢出检测 (生产环境可关提高性能，调试建议开 1 或 2)
#define configUSE_RECURSIVE_MUTEXES				1                       // 1: 开启递归互斥量
#define configUSE_MALLOC_FAILED_HOOK			0                       // 1: 开启内存分配失败钩子函数
#define configUSE_APPLICATION_TASK_TAG			0                       // 0: 不使用任务标签
#define configUSE_COUNTING_SEMAPHORES			1                       // 1: 开启计数信号量

/***************************************************************************************************************/
/*                                FreeRTOS 内存管理配置                                                        */
/***************************************************************************************************************/
#define configSUPPORT_DYNAMIC_ALLOCATION        1                       // 1: 支持动态内存分配 (即用 FreeRTOS 的 malloc)
#define configTOTAL_HEAP_SIZE					((size_t)(46*1024))     // FreeRTOS 系统能管理的总堆内存大小 (46KB)

/***************************************************************************************************************/
/*                                FreeRTOS 钩子函数配置                                                        */
/***************************************************************************************************************/
#define configUSE_IDLE_HOOK						0                       // 0: 不使用空闲任务钩子函数
#define configUSE_TICK_HOOK						0                       // 0: 不使用时间片滴答钩子函数

/***************************************************************************************************************/
/*                                FreeRTOS 运行时间和任务状态统计宏                                            */
/***************************************************************************************************************/
#define configGENERATE_RUN_TIME_STATS	        1                       // 1: 开启运行时 CPU 占用率统计分析
#define configUSE_TRACE_FACILITY				1                       // 1: 开启可视化跟踪调试
#define configUSE_STATS_FORMATTING_FUNCTIONS	1                       // 1: 编译 vTaskList 和 vTaskGetRunTimeStats 字符串格式化函数

/* 滴答定时器外设的底层宏映射挂载点 */
extern void ConfigureTimeForRunTimeStats(void);
extern uint32_t GetTimerCounterValue(void);
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() ConfigureTimeForRunTimeStats() // 映射为初始化高精度定时器 TIM2
#define portGET_RUN_TIME_COUNTER_VALUE()         GetTimerCounterValue()         // 映射为读取 TIM2 计数值
                                                                        
/***************************************************************************************************************/
/*                                协程配置 (适用于内存极小的 8/16 位单片机，32位通常不用)                      */
/***************************************************************************************************************/
#define configUSE_CO_ROUTINES 			        0                       // 0: 关闭协程
#define configMAX_CO_ROUTINE_PRIORITIES         ( 2 )                   // 协程的最大优先级

/***************************************************************************************************************/
/*                                软件定时器配置                                                               */
/***************************************************************************************************************/
#define configUSE_TIMERS				        1                               // 1: 开启软件定时器
#define configTIMER_TASK_PRIORITY		        (configMAX_PRIORITIES-1)        // 软件定时器守护任务的优先级设为最高 (31)
#define configTIMER_QUEUE_LENGTH		        5                               // 定时器命令队列的长度
#define configTIMER_TASK_STACK_DEPTH	        (configMINIMAL_STACK_SIZE*2)    // 定时器任务的堆栈深度

/***************************************************************************************************************/
/*                                可选函数 API 裁剪配置 (1: 包含纳入编译，0: 剔除省体积)                       */
/***************************************************************************************************************/
#define INCLUDE_xTaskGetSchedulerState          1                       
#define INCLUDE_vTaskPrioritySet		        1
#define INCLUDE_uxTaskPriorityGet		        1
#define INCLUDE_vTaskDelete				        1
#define INCLUDE_vTaskCleanUpResources	        1
#define INCLUDE_vTaskSuspend			        1
#define INCLUDE_vTaskDelayUntil			        1
#define INCLUDE_vTaskDelay				        1
#define INCLUDE_eTaskGetState			        1
#define INCLUDE_xTimerPendFunctionCall	        1

/***************************************************************************************************************/
/*                                中断优先级系统配置                                                           */
/***************************************************************************************************************/
#ifdef __NVIC_PRIO_BITS
	#define configPRIO_BITS       		__NVIC_PRIO_BITS
#else
	#define configPRIO_BITS       		4                               // STM32F4 使用 4 位表示中断抢占优先级
#endif

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY			15              // 库能够设定的最低中断优先级 (对于 4 位优先级就是 15)
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY	5               // FreeRTOS 能管理/屏蔽的安全最高优先级阈值 (5)
                                                                        // 意味着 0~4 优先级的中断不受 FreeRTOS 影响，也不会被屏蔽
#define configKERNEL_INTERRUPT_PRIORITY 		( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) ) // 底层内核调度的基础优先级组合
#define configMAX_SYSCALL_INTERRUPT_PRIORITY 	( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS) ) // 组装受系统保护的最大位移宏

/***************************************************************************************************************/
/*                                内核底层异常处理程序重定向映射                                               */
/***************************************************************************************************************/
#define xPortPendSVHandler 	PendSV_Handler
#define vPortSVCHandler 	SVC_Handler

extern void PreSleepProcessing(uint32_t *ulExpectedIdleTime);
extern void PostSleepProcessing(uint32_t *ulExpectedIdleTime);

#define configPRE_SLEEP_PROCESSING(xModifiableTime) PreSleepProcessing(&xModifiableTime)

#define configPOST_SLEEP_PROCESSING(xExpectedIdleTime) PostSleepProcessing(&xExpectedIdleTime)
#endif /* FREERTOS_CONFIG_H */


