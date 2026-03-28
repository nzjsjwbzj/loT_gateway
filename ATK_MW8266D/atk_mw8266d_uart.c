/**
 ****************************************************************************************************
 * @file        atk_mw8266d_uart.c
 * @author      ALIENTEK
 * @version     V1.0
 * @date        2022-06-21
 * @brief       ATK-MW8266D ??????????UART ??????????????
 * @license     Copyright (c) 2020-2032, ALIENTEK
 ****************************************************************************************************
 * @attention
 *
 * ???: ????????? STM32 HAL ????? ATK-MW8266D ???? UART ???????????????????
 *
 ****************************************************************************************************
 */

#include "atk_mw8266d_uart.h"
#include "usart.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"

extern QueueHandle_t xUartRxQueue; // ??????????

UART_HandleTypeDef g_uart_handle;                    /* ATK-MW8266D UART ??? */
static struct
{
    uint8_t buf[ATK_MW8266D_UART_RX_BUF_SIZE];              /* ????????? */
    struct
    {
        uint16_t len    : 15;                               /* ???????????15 ???? */
        uint16_t finsh  : 1;                                /* ???????????1 ???? */
    } sta;                                                  /* ?????? */
} g_uart_rx_frame = {0};                                    /* ????????????? */



static uint8_t g_uart_tx_buf[ATK_MW8266D_UART_TX_BUF_SIZE]; /* ????????? */

/**
 * @brief       ATK-MW8266D UART printf????????????
 * @param       fmt: ????????
 * @retval      ??
 */
void atk_mw8266d_uart_printf(char *fmt, ...)
{
    va_list ap;
    uint16_t len;
    
    va_start(ap, fmt);
    vsprintf((char *)g_uart_tx_buf, fmt, ap);
    va_end(ap);
    
    len = strlen((const char *)g_uart_tx_buf);
    HAL_UART_Transmit(&g_uart_handle, g_uart_tx_buf, len, HAL_MAX_DELAY);
}

/**
 * @brief       ????????????????????????
 * @param       ??
 * @retval      ??
 */
void atk_mw8266d_uart_rx_restart(void)
{
    g_uart_rx_frame.sta.len     = 0;
    g_uart_rx_frame.sta.finsh   = 0;
}

/**
 * @brief       ???????????????
 * @param       ??
 * @retval      NULL: ??????????
 *              ?? NULL: ????? '\0' ?????????????
 */
uint8_t *atk_mw8266d_uart_rx_get_frame(void)
{
    if (g_uart_rx_frame.sta.finsh == 1)
    {
        g_uart_rx_frame.buf[g_uart_rx_frame.sta.len] = '\0';
        return g_uart_rx_frame.buf;
    }
    else
    {
        return NULL;
    }
}

/**
 * @brief       ?????????????????????????? '\0'??
 * @param       ??
 * @retval      0   : ???????
 *              ?? 0: ?????????
 */
uint16_t atk_mw8266d_uart_rx_get_frame_len(void)
{
    if (g_uart_rx_frame.sta.finsh == 1)
    {
        return g_uart_rx_frame.sta.len;
    }
    
    else
    {
        return 0;
    }
}

/**
 * @brief       ????? ATK-MW8266D UART ???
 * @param       baudrate: ??????
 * @retval      ??
 */
void atk_mw8266d_uart_init(uint32_t baudrate)
{
    g_uart_handle.Instance          = ATK_MW8266D_UART_INTERFACE;   /* ??? UART ???? */
    g_uart_handle.Init.BaudRate     = baudrate;                     /* ?????? */
    g_uart_handle.Init.WordLength   = UART_WORDLENGTH_8B;           /* 8 ???????? */
    g_uart_handle.Init.StopBits     = UART_STOPBITS_1;              /* 1 ?????? */
    g_uart_handle.Init.Parity       = UART_PARITY_NONE;             /* ???????? */
    g_uart_handle.Init.Mode         = UART_MODE_TX_RX;              /* ????? */
    g_uart_handle.Init.HwFlowCtl    = UART_HWCONTROL_NONE;          /* ????????? */
    g_uart_handle.Init.OverSampling = UART_OVERSAMPLING_16;         /* ?????? 16 */
    HAL_UART_Init(&g_uart_handle);                                  /* ????? UART
                                                                     * HAL_UART_Init() ????? HAL_UART_MspInit()
                                                                     * ??????????????????? usart.c ?????
                                                                     */

    HAL_NVIC_SetPriority(ATK_MW8266D_UART_IRQn, 6, 0);              /* ?????????????? 6
                                                                     * ???
                                                                     * FreeRTOS ?????? API ??????????????????(???????)
                                                                     * configMAX_SYSCALL_INTERRUPT_PRIORITY (???? 5)
                                                                     * ????????? 6 ?????? (6 > 5)
                                                                     */
    HAL_NVIC_EnableIRQ(ATK_MW8266D_UART_IRQn);                      /* ??? UART ???? */
}

/**
 * @brief       ATK-MW8266D UART ???????????
 * @param       ??
 * @retval      ??
 */

void ATK_MW8266D_UART_IRQHandler(void)
{
    uint8_t tmp;

    /* 1. ???? ORE ??????? */
    if (__HAL_UART_GET_FLAG(&g_uart_handle, UART_FLAG_ORE) != RESET)
    {
        __HAL_UART_CLEAR_OREFLAG(&g_uart_handle);
        (void)g_uart_handle.Instance->SR;
        (void)g_uart_handle.Instance->DR;
    }

    /* 2. ??????????????? HAL_UART_Receive */
    if (__HAL_UART_GET_FLAG(&g_uart_handle, UART_FLAG_RXNE) != RESET)
    {
        /* ? ???????????? */
        tmp = (uint8_t)(g_uart_handle.Instance->DR & 0xFF);

        if (g_uart_rx_frame.sta.len < (ATK_MW8266D_UART_RX_BUF_SIZE - 1))
        {
            g_uart_rx_frame.buf[g_uart_rx_frame.sta.len++] = tmp;
        }
        else
        {
            g_uart_rx_frame.sta.len = ATK_MW8266D_UART_RX_BUF_SIZE - 1;
        }
    }

    /* 3. IDLE ????????????????? */
    if (__HAL_UART_GET_FLAG(&g_uart_handle, UART_FLAG_IDLE) != RESET)
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        __HAL_UART_CLEAR_IDLEFLAG(&g_uart_handle);
        g_uart_rx_frame.sta.finsh = 1;

        /* 4. ????????????????????????? */
        if (xUartRxQueue != NULL)//
        {
            uint16_t len = g_uart_rx_frame.sta.len;
            xQueueSendFromISR(xUartRxQueue, &len, &xHigherPriorityTaskWoken);
        }

        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

