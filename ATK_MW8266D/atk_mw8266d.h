/**
 ****************************************************************************************************
 * @file        atk_mw8266d.h
 * @author      ??????????(ALIENTEK)
 * @version     V1.0
 * @date        2022-06-21
 * @brief       ATK-MW8266D???????????
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

#ifndef __ATK_MW8266D_H
#define __ATK_MW8266D_H

#include "sys.h"
#include "atk_mw8266d_uart.h"

/* ??????? */
#define ATK_MW8266D_RST_GPIO_PORT           GPIOI
#define ATK_MW8266D_RST_GPIO_PIN            GPIO_PIN_11
#define ATK_MW8266D_RST_GPIO_CLK_ENABLE()   do{ __HAL_RCC_GPIOI_CLK_ENABLE(); }while(0) /* PA???????? */

/* IO???? */
#define ATK_MW8266D_RST(x)                  do{ x ?                                                                                     \
                                                HAL_GPIO_WritePin(ATK_MW8266D_RST_GPIO_PORT, ATK_MW8266D_RST_GPIO_PIN, GPIO_PIN_SET) :  \
                                                HAL_GPIO_WritePin(ATK_MW8266D_RST_GPIO_PORT, ATK_MW8266D_RST_GPIO_PIN, GPIO_PIN_RESET); \
                                            }while(0)

/* ??????? */
#define ATK_MW8266D_EOK         0   /* ??????? */
#define ATK_MW8266D_ERROR       1   /* ?????? */
#define ATK_MW8266D_ETIMEOUT    2   /* ??????? */
#define ATK_MW8266D_EINVAL      3   /* ???????? */

/* ???????? */
void atk_mw8266d_hw_reset(void);                                            /* ATK-MW8266D??????? */
uint8_t atk_mw8266d_send_at_cmd(char *cmd, char *ack, uint32_t timeout);    /* ATK-MW8266D????AT??? */
uint8_t atk_mw8266d_init(uint32_t baudrate);                                /* ATK-MW8266D????? */
uint8_t atk_mw8266d_restore(void);                                          /* ATK-MW8266D??????????? */
uint8_t atk_mw8266d_at_test(void);                                          /* ATK-MW8266D AT?????? */
uint8_t atk_mw8266d_set_mode(uint8_t mode);                                 /* ????ATK-MW8266D?????? */
uint8_t atk_mw8266d_sw_reset(void);                                         /* ATK-MW8266D???????? */
uint8_t atk_mw8266d_ate_config(uint8_t cfg);                                /* ATK-MW8266D????????? */
uint8_t atk_mw8266d_join_ap(char *ssid, char *pwd);                         /* ATK-MW8266D????WIFI */
uint8_t atk_mw8266d_get_ip(char *buf);                                      /* ATK-MW8266D???IP??? */
uint8_t atk_mw8266d_connect_tcp_server(char *server_ip, char *server_port); /* ATK-MW8266D????TCP?????? */
uint8_t atk_mw8266d_enter_unvarnished(void);                                /* ATK-MW8266D??????? */
void atk_mw8266d_exit_unvarnished(void);                                    /* ATK-MW8266D?????? */
uint8_t atk_mw8266d_connect_atkcld(char *id, char *pwd);                    /* ATK-MW8266D?????????????? */
uint8_t atk_mw8266d_disconnect_atkcld(void);                                /* ATK-MW8266D????????????????? */
uint8_t atk_mw8266d_mqtt_usercfg(uint8_t scheme, char *client_id, char *username, char *password); /* ????MQTT?????? */
uint8_t atk_mw8266d_mqtt_conn(char *host, char *port, uint8_t reconnect);     /* ????MQTT Broker */
uint8_t atk_mw8266d_mqtt_pub(char *topic, char *data, uint8_t qos, uint8_t retain); /* ????MQTT??? */
uint8_t atk_mw8266d_mqtt_sub(char *topic, uint8_t qos);                       /* ????MQTT???? */
uint8_t atk_mw8266d_mqtt_unsub(char *topic);                                  /* ???????MQTT???? */
uint8_t atk_mw8266d_mqtt_clean(void);                                         /* ???MQTT???? */
uint8_t atk_mw8266d_ping(char *ip);   

//自定义
void wifi_net_init(char *ssid, char *pwd,char *ip_buf);

/* Ping IP??? */

#endif
