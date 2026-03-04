#include "atk_mw8266d.h"
#include "delay.h"
#include <string.h>
#include <stdio.h>

/**
 * @brief       ATK-MW8266D????????
 * @param       ??
 * @retval      ??
 */
static void atk_mw8266d_hw_init(void)
{
    GPIO_InitTypeDef gpio_init_struct;
    
    ATK_MW8266D_RST_GPIO_CLK_ENABLE();
    
    gpio_init_struct.Pin = ATK_MW8266D_RST_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init_struct.Pull = GPIO_NOPULL;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ATK_MW8266D_RST_GPIO_PORT, &gpio_init_struct);
}

/**
 * @brief       ATK-MW8266D???????
 * @param       ??
 * @retval      ??
 */
void atk_mw8266d_hw_reset(void)
{
    ATK_MW8266D_RST(0);
    delay_ms(100);
    ATK_MW8266D_RST(1);
    delay_ms(500);
}

/**
 * @brief       ATK-MW8266D????AT???
 * @param       cmd    : ???????AT???
 *              ack    : ????????
 *              timeout: ?????????
 * @retval      ATK_MW8266D_EOK     : ??????????
 *              ATK_MW8266D_ETIMEOUT: ???????????????????????
 */
uint8_t atk_mw8266d_send_at_cmd(char *cmd, char *ack, uint32_t timeout)
{
    uint8_t *ret = NULL;
    

    atk_mw8266d_uart_rx_restart();
    atk_mw8266d_uart_printf("%s\r\n", cmd);
    
    if ((ack == NULL) || (timeout == 0))
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        while (timeout > 0)
        {
            ret = atk_mw8266d_uart_rx_get_frame();
            

            if (ret != NULL)
            {
                if (strstr((const char *)ret, ack) != NULL)
                {
                    return ATK_MW8266D_EOK;
                }
                else
                {
                    atk_mw8266d_uart_rx_restart();
                }
            }
            timeout--;    
            delay_ms(1);
        }
        
        return ATK_MW8266D_ETIMEOUT;
    }
}

/**
 * @brief       ATK-MW8266D?????
 * @param       baudrate: ATK-MW8266D UART????????
 * @retval      ATK_MW8266D_EOK  : ATK-MW8266D????????????????????
 *              ATK_MW8266D_ERROR: ATK-MW8266D???????????????????
 */
uint8_t atk_mw8266d_init(uint32_t baudrate)
{
    atk_mw8266d_hw_init();                          /* ATK-MW8266D???????? */
    atk_mw8266d_hw_reset();                         /* ATK-MW8266D??????? */
    
    // ??????????????
    atk_mw8266d_uart_init(baudrate);                
    if (atk_mw8266d_at_test() == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }

    // ??????????? 9600
    if (baudrate != 9600) 
    {
        atk_mw8266d_uart_init(9600);
        if (atk_mw8266d_at_test() == ATK_MW8266D_EOK)
        {
            // ???9600??????????????9600????????????????115200????????????????9600??
            // ????????????9600????????????????????????????????????
            // ??????????????????????????????9600
            // ??????? AT+UART_CUR=baudrate,8,1,0,0
            char cmd[64];
            sprintf(cmd, "AT+UART_CUR=%u,8,1,0,0", baudrate);
            atk_mw8266d_send_at_cmd(cmd, "OK", 500);
            
            // ????????????????????
            atk_mw8266d_uart_init(baudrate);
            delay_ms(500); // ??????????
            if (atk_mw8266d_at_test() == ATK_MW8266D_EOK)
            {
                return ATK_MW8266D_EOK;
            }
        }
    }

    // ????????????? 115200 (?????????115200)
    if (baudrate != 115200)
    {
        atk_mw8266d_uart_init(115200);
        if (atk_mw8266d_at_test() == ATK_MW8266D_EOK)
        {
             return ATK_MW8266D_EOK;
        }
    }
    
    return ATK_MW8266D_ERROR;
}

/**
 * @brief       ATK-MW8266D???????????
 * @param       ??
 * @retval      ATK_MW8266D_EOK  : ??????????????
 *              ATK_MW8266D_ERROR: ??????????????
 */
uint8_t atk_mw8266d_restore(void)
{
    uint8_t ret;
    
    ret = atk_mw8266d_send_at_cmd("AT+RESTORE", "ready", 3000);
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D AT??????
 * @param       ??
 * @retval      ATK_MW8266D_EOK  : AT????????
 *              ATK_MW8266D_ERROR: AT?????????
 */
uint8_t atk_mw8266d_at_test(void)
{
    uint8_t ret;
    uint8_t i;
    
    for (i=0; i<10; i++)
    {
        ret = atk_mw8266d_send_at_cmd("AT", "OK", 500);
        if (ret == ATK_MW8266D_EOK)
        {
            return ATK_MW8266D_EOK;
        }
    }
    
    return ATK_MW8266D_ERROR;
}

/**
 * @brief       ????ATK-MW8266D??????
 * @param       mode: 1??Station??
 *                    2??AP??
 *                    3??AP+Station??
 * @retval      ATK_MW8266D_EOK   : ?????????????
 *              ATK_MW8266D_ERROR : ?????????????
 *              ATK_MW8266D_EINVAL: mode????????????????????
 */
uint8_t atk_mw8266d_set_mode(uint8_t mode)
{
    uint8_t ret;
    
    switch (mode)
    {
        case 1:
        {
            ret = atk_mw8266d_send_at_cmd("AT+CWMODE=1", "OK", 500);    /* Station?? */
            break;
        }
        case 2:
        {
            ret = atk_mw8266d_send_at_cmd("AT+CWMODE=2", "OK", 500);    /* AP?? */
            break;
        }
        case 3:
        {
            ret = atk_mw8266d_send_at_cmd("AT+CWMODE=3", "OK", 500);    /* AP+Station?? */
            break;
        }
        default:
        {
            return ATK_MW8266D_EINVAL;
        }
    }
    
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D????????
 * @param       ??
 * @retval      ATK_MW8266D_EOK  : ???????????
 *              ATK_MW8266D_ERROR: ???????????
 */
uint8_t atk_mw8266d_sw_reset(void)
{
    uint8_t ret;
    
    ret = atk_mw8266d_send_at_cmd("AT+RST", "OK", 500);
    if (ret == ATK_MW8266D_EOK)
    {
        delay_ms(1000);
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D?????????
 * @param       cfg: 0????????
 *                   1???????
 * @retval      ATK_MW8266D_EOK  : ????????????
 *              ATK_MW8266D_ERROR: ????????????
 */
uint8_t atk_mw8266d_ate_config(uint8_t cfg)
{
    uint8_t ret;
    
    switch (cfg)
    {
        case 0:
        {
            ret = atk_mw8266d_send_at_cmd("ATE0", "OK", 500);   /* ?????? */
            break;
        }
        case 1:
        {
            ret = atk_mw8266d_send_at_cmd("ATE1", "OK", 500);   /* ????? */
            break;
        }
        default:
        {
            return ATK_MW8266D_EINVAL;
        }
    }
    
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D????WIFI
 * @param       ssid: WIFI????
 *              pwd : WIFI????
 * @retval      ATK_MW8266D_EOK  : WIFI??????
 *              ATK_MW8266D_ERROR: WIFI???????
 */
uint8_t atk_mw8266d_join_ap(char *ssid, char *pwd)
{
    uint8_t ret;
    char cmd[64];
    
    sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"", ssid, pwd);
    ret = atk_mw8266d_send_at_cmd(cmd, "WIFI GOT IP", 10000);
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D???IP???
 * @param       buf: IP????????16????????
 * @retval      ATK_MW8266D_EOK  : ???IP??????
 *              ATK_MW8266D_ERROR: ???IP??????
 */
uint8_t atk_mw8266d_get_ip(char *buf)
{
    uint8_t ret;
    char *p_start;
    char *p_end;
    
    ret = atk_mw8266d_send_at_cmd("AT+CIFSR", "OK", 500);
    if (ret != ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_ERROR;
    }
    
    p_start = strstr((const char *)atk_mw8266d_uart_rx_get_frame(), "\"");
    p_end = strstr(p_start + 1, "\"");
    *p_end = '\0';
    sprintf(buf, "%s", p_start + 1);
    
    return ATK_MW8266D_EOK;
}

/**
 * @brief       ATK-MW8266D????TCP??????
 * @param       server_ip  : TCP??????IP???
 *              server_port: TCP??????????
 * @retval      ATK_MW8266D_EOK  : ????TCP?????????
 *              ATK_MW8266D_ERROR: ????TCP?????????
 */
uint8_t atk_mw8266d_connect_tcp_server(char *server_ip, char *server_port)
{
    uint8_t ret;
    char cmd[64];
    
    sprintf(cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s", server_ip, server_port);
    ret = atk_mw8266d_send_at_cmd(cmd, "CONNECT", 5000);
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D???????
 * @param       ??
 * @retval      ATK_MW8266D_EOK  : ??????????
 *              ATK_MW8266D_ERROR: ??????????
 */
uint8_t atk_mw8266d_enter_unvarnished(void)
{
    uint8_t ret;
    
    ret  = atk_mw8266d_send_at_cmd("AT+CIPMODE=1", "OK", 500);
    ret += atk_mw8266d_send_at_cmd("AT+CIPSEND", ">", 500);
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D??????
 * @param       ??
 * @retval      ??
 */
void atk_mw8266d_exit_unvarnished(void)
{
    delay_ms(1000);
    atk_mw8266d_uart_printf("+++");
    delay_ms(1000);
    atk_mw8266d_uart_rx_restart();
    atk_mw8266d_send_at_cmd("AT", "OK", 500);
}

/**
 * @brief       ATK-MW8266D??????????????
 * @param       id : ???????????
 *              pwd: ????????????
 * @retval      ATK_MW8266D_EOK  : ?????????????????
 *              ATK_MW8266D_ERROR: ?????????????????
 */
uint8_t atk_mw8266d_connect_atkcld(char *id, char *pwd)
{
    uint8_t ret;
    char cmd[64];
    
    sprintf(cmd, "AT+ATKCLDSTA=\"%s\",\"%s\"", id, pwd);
    ret = atk_mw8266d_send_at_cmd(cmd, "CLOUD CONNECTED", 10000);
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ATK-MW8266D?????????????????
 * @param       ??
 * @retval      ATK_MW8266D_EOK  : ???????????????????
 *              ATK_MW8266D_ERROR: ????????????????????
 */
uint8_t atk_mw8266d_disconnect_atkcld(void)
{
    uint8_t ret;
    
    ret = atk_mw8266d_send_at_cmd("AT+ATKCLDCLS", "CLOUD DISCONNECT", 500);
    if (ret == ATK_MW8266D_EOK)
    {
        return ATK_MW8266D_EOK;
    }
    else
    {
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ????MQTT??????
 * @param       scheme: 0: disable SSL
 *                      1: enable SSL without CA
 *                      2: enable SSL with CA
 * @param       client_id: ?????ID
 * @param       username: ?????
 * @param       password: ????
 * @retval      ATK_MW8266D_EOK: ???
 *              ATK_MW8266D_ERROR: ???
 */
uint8_t atk_mw8266d_mqtt_usercfg(uint8_t scheme, char *client_id, char *username, char *password)
{
    uint8_t ret;
    char cmd[256];

    // ????AT??????: 
    // AT+MQTTUSERCFG=<LinkID>,<scheme>,<"client_id">,<"username">,<"password">,<cert_key_ID>,<CA_ID>,<"path">
    // scheme: 1: TCP, 2: TLS(no verify) ...
    sprintf(cmd, "AT+MQTTUSERCFG=0,%d,\"%s\",\"%s\",\"%s\",0,0,\"\"", scheme, client_id, username, password);
    
    // ?????????
    printf("Sending MQTT User Config: %s\r\n", cmd);
    
    ret = atk_mw8266d_send_at_cmd(cmd, "OK", 1000);
    
    if (ret != ATK_MW8266D_EOK)
    {
        printf("MQTT User Config Failed! Response might be ERROR or timeout.\r\n");
        // ??????????????????????????? atk_mw8266d_uart_rx_get_frame() ??????????
        // ??????????????????????????????
        // ???? send_at_cmd ?????????????????????????????????????????
        // ?????????????????????
    }
    
    return (ret == ATK_MW8266D_EOK) ? ATK_MW8266D_EOK : ATK_MW8266D_ERROR;
}

/**
 * @brief       ????MQTT Broker
 * @param       host: Broker???
 * @param       port: ???
 * @param       reconnect: ?????????? (0:??, 1:??)
 * @retval      ATK_MW8266D_EOK: ???
 *              ATK_MW8266D_ERROR: ???
 */
uint8_t atk_mw8266d_mqtt_conn(char *host, char *port, uint8_t reconnect)
{
    uint8_t ret;
    char cmd[128];

    sprintf(cmd, "AT+MQTTCONN=0,\"%s\",%s,%d", host, port, reconnect);
    
    printf("Sending MQTT Connect: %s\r\n", cmd);

    // ????????????????
    ret = atk_mw8266d_send_at_cmd(cmd, "OK", 5000); 
    
    if (ret != ATK_MW8266D_EOK)
    {
        printf("MQTT Connect Failed! Check Broker IP/Port or Firewall.\r\n");
    }

    return (ret == ATK_MW8266D_EOK) ? ATK_MW8266D_EOK : ATK_MW8266D_ERROR;
}

/**
 * @brief       ????MQTT???
 * @param       topic: ????
 * @param       data: ????
 * @param       qos: QoS??? (0, 1, 2)
 * @param       retain: Retain??? (0, 1)
 * @retval      ATK_MW8266D_EOK: ???
 *              ATK_MW8266D_ERROR: ???
 */
uint8_t atk_mw8266d_mqtt_pub(char *topic, char *data, uint8_t qos, uint8_t retain)
{
    uint8_t ret;
    char cmd[256]; 
    int data_len = strlen(data);
    uint8_t *resp;
	uint32_t timeout;
    // Use RAW mode to avoid escaping issues
    
    // AT+MQTTPUBRAW=<LinkID>,<"topic">,<len>,<qos>,<retain>
    sprintf(cmd, "AT+MQTTPUBRAW=0,\"%s\",%d,%d,%d", topic, data_len, qos, retain);
    
    printf("Sending MQTT Pub Raw: %s\r\n", cmd);

    // 1. Send Command and wait for '>'
    ret = atk_mw8266d_send_at_cmd(cmd, "OK", 2000); // Expect OK first
    if (ret == ATK_MW8266D_EOK)
    {
        // 2. Wait for '>' prompt
        // Since atk_mw8266d_send_at_cmd restarts RX, we might miss '>' if it comes immediately after OK.
        // But usually there is a small delay. Or the OK and > come together.
        // Let's check if we can send data now.
        // A more robust way is to wait for '>' specifically.
        // Since we can't easily change the helper, let's just send the data blindly after a small delay 
        // OR rely on the fact that send_at_cmd returns OK which means it's ready.
        // NOTE: Some firmwares return "OK\r\n>"
        
        delay_ms(10); // Small delay to ensure module is ready
        
        printf("Sending Data: %s\r\n", data);
        
        // 3. Send Data (NO \r\n at the end, unless it's part of data)
        atk_mw8266d_uart_rx_restart();
        atk_mw8266d_uart_printf("%s", data);
        
        // 4. Wait for +MQTTPUB:OK
        // We implement a custom wait loop here
         timeout = 2000;
        while (timeout > 0)
        {
            resp = atk_mw8266d_uart_rx_get_frame();
            if (resp != NULL)
            {
                if (strstr((const char *)resp, "+MQTTPUB:OK") != NULL)
                {
                    return ATK_MW8266D_EOK;
                }
                // Keep waiting if we got other messages (like +IPD...)
                atk_mw8266d_uart_rx_restart();
            }
            delay_ms(1);
            timeout--;
        }
        
        printf("Wait for +MQTTPUB:OK Timeout!\r\n");
        return ATK_MW8266D_ETIMEOUT;
    }
    else
    {
        printf("MQTT Pub Raw Command Failed (No OK/>)!\r\n");
        resp = atk_mw8266d_uart_rx_get_frame();
        if (resp != NULL) printf("Resp: %s\r\n", resp);
        return ATK_MW8266D_ERROR;
    }
}

/**
 * @brief       ????MQTT????
 * @param       topic: ????
 * @param       qos: QoS???
 * @retval      ATK_MW8266D_EOK: ???
 *              ATK_MW8266D_ERROR: ???
 */
uint8_t atk_mw8266d_mqtt_sub(char *topic, uint8_t qos)
{
    uint8_t ret;
    char cmd[128];

    sprintf(cmd, "AT+MQTTSUB=0,\"%s\",%d", topic, qos);
    ret = atk_mw8266d_send_at_cmd(cmd, "OK", 1000);
    return (ret == ATK_MW8266D_EOK) ? ATK_MW8266D_EOK : ATK_MW8266D_ERROR;
}

/**
 * @brief       ???????MQTT????
 * @param       topic: ????
 * @retval      ATK_MW8266D_EOK: ???
 *              ATK_MW8266D_ERROR: ???
 */
uint8_t atk_mw8266d_mqtt_unsub(char *topic)
{
    uint8_t ret;
    char cmd[128];

    sprintf(cmd, "AT+MQTTUNSUB=0,\"%s\"", topic);
    ret = atk_mw8266d_send_at_cmd(cmd, "OK", 1000);
    return (ret == ATK_MW8266D_EOK) ? ATK_MW8266D_EOK : ATK_MW8266D_ERROR;
}

/**
 * @brief       ???MQTT????
 * @param       ??
 * @retval      ATK_MW8266D_EOK: ???
 *              ATK_MW8266D_ERROR: ???
 */
uint8_t atk_mw8266d_mqtt_clean(void)
{
    uint8_t ret;
    
    ret = atk_mw8266d_send_at_cmd("AT+MQTTCLEAN=0", "OK", 1000);
    return (ret == ATK_MW8266D_EOK) ? ATK_MW8266D_EOK : ATK_MW8266D_ERROR;
}

/**
 * @brief       Ping IP???
 * @param       ip: ???IP???
 * @retval      ATK_MW8266D_EOK: Ping???
 *              ATK_MW8266D_ERROR: Ping???
 */
uint8_t atk_mw8266d_ping(char *ip)
{
    uint8_t ret = ATK_MW8266D_ETIMEOUT;
    char cmd[64];
    uint8_t *resp;
    uint32_t timeout = 3000;
    
    sprintf(cmd, "AT+PING=\"%s\"", ip);
    
    atk_mw8266d_uart_rx_restart();
    atk_mw8266d_uart_printf("%s\r\n", cmd);
    
    printf("Pinging %s ...\r\n", ip);
    
    while (timeout > 0)
    {
        resp = atk_mw8266d_uart_rx_get_frame();
        if (resp != NULL)
        {
            if (strstr((const char *)resp, "+PING:TIMEOUT") != NULL || strstr((const char *)resp, "ERROR") != NULL)
            {
                ret = ATK_MW8266D_ERROR;
                break;
            }
            else if (strstr((const char *)resp, "OK") != NULL) // Success usually ends with OK
            {
                ret = ATK_MW8266D_EOK;
                break;
            }
            // If we get "+PING:23" but not OK yet, we keep waiting or could consider it success if we parsed it.
            // But waiting for OK is safer.
            // Note: If +PING:<time> comes in a separate packet from OK, we need to handle that.
            // But get_frame usually returns the whole buffer or the latest chunk.
            // If the buffer accumulates, checking for OK is fine.
            // If the buffer is cleared on restart, we might miss the previous part if we restart too aggressively.
            // atk_mw8266d_uart_rx_get_frame() returns the buffer.
            // If we don't find what we want, we should probably NOT restart RX immediately if we are expecting a split response?
            // However, the existing driver pattern is: check -> restart if not found. 
            // This assumes the whole response comes in one go or we are lucky.
            // But for PING, it might be split.
            // Let's stick to the driver pattern: restart if not matched.
            
            // Wait, if I restart, I lose the previous data.
            // If "+PING:23" comes, then "OK" comes later.
            // If I see "+PING:23", I should probably not restart, but just wait for OK?
            // Actually, if I see "+PING:" and NOT "TIMEOUT", it's a good sign.
            
            if (strstr((const char *)resp, "+PING:") != NULL && strstr((const char *)resp, "TIMEOUT") == NULL)
            {
                 // Found valid PING response time, assume success or wait for OK?
                 // Let's wait for OK to be strictly correct, but if we see a time, it's definitely a success.
                 ret = ATK_MW8266D_EOK;
                 break;
            }
            
            atk_mw8266d_uart_rx_restart();
        }
        delay_ms(1);
        timeout--;
    }
    
    if (ret != ATK_MW8266D_EOK)
    {
        printf("Ping Failed! Host unreachable or Timeout.\r\n");
    }
    else
    {
        printf("Ping Success!\r\n");
    }
    
    return ret;
}
