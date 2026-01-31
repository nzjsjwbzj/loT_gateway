/**
 ****************************************************************************************************
 * @file        atk_mw8266d_uart.h
 * @author      ??????????(ALIENTEK)
 * @version     V1.0
 * @date        2022-06-21
 * @brief       ATK-MW8266D???UART???????????
 * @license     Copyright (c) 2020-2032, ??????????????????????
 ****************************************************************************************************
 * @attention
 *
 * ?????:??????? ?????? F429??????
 * ???????:www.yuanzige.com
 * ???????:www.openedv.com
 * ??????:www.alientek.com
 * ??????:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#ifndef __ATK_MW8266D_UART_H
#define __ATK_MW8266D_UART_H

#include "sys.h"

/* ??????? */
#define ATK_MW8266D_UART_TX_GPIO_PORT           GPIOA
#define ATK_MW8266D_UART_TX_GPIO_PIN            GPIO_PIN_2
#define ATK_MW8266D_UART_TX_GPIO_AF             GPIO_AF7_USART2
#define ATK_MW8266D_UART_TX_GPIO_CLK_ENABLE()   do{ __HAL_RCC_GPIOB_CLK_ENABLE(); }while(0)     /* PB???????? */

#define ATK_MW8266D_UART_RX_GPIO_PORT           GPIOA
#define ATK_MW8266D_UART_RX_GPIO_PIN            GPIO_PIN_3
#define ATK_MW8266D_UART_RX_GPIO_AF             GPIO_AF7_USART2
#define ATK_MW8266D_UART_RX_GPIO_CLK_ENABLE()   do{ __HAL_RCC_GPIOB_CLK_ENABLE(); }while(0)     /* PB???????? */

#define ATK_MW8266D_UART_INTERFACE              USART2
#define ATK_MW8266D_UART_IRQn                   USART2_IRQn
#define ATK_MW8266D_UART_IRQHandler             USART2_IRQHandler
#define ATK_MW8266D_UART_CLK_ENABLE()           do{ __HAL_RCC_USART2_CLK_ENABLE(); }while(0)    /* USART3 ?????? */

/* UART????????§³ */
#define ATK_MW8266D_UART_RX_BUF_SIZE            512
#define ATK_MW8266D_UART_TX_BUF_SIZE            512

/* ???????? */
void atk_mw8266d_uart_printf(char *fmt, ...);       /* ATK-MW8266D UART printf */
void atk_mw8266d_uart_rx_restart(void);             /* ATK-MW8266D UART?????????????? */
uint8_t *atk_mw8266d_uart_rx_get_frame(void);       /* ???ATK-MW8266D UART????????????? */
uint16_t atk_mw8266d_uart_rx_get_frame_len(void);   /* ???ATK-MW8266D UART????????????????? */
void atk_mw8266d_uart_init(uint32_t baudrate);      /* ATK-MW8266D UART????? */

#endif
