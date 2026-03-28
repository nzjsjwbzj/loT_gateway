#include "protocol_onenet.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 包含所需的硬件控制头文件以及全局变量控制头文件
#include "led.h"
#include "pcf8574.h"
#include "../OTA/ota.h"

// 从外部引入在其他模块定义的全局配置和互操作变量
extern volatile uint32_t g_ap_sample_period_ms;
extern volatile uint8_t g_ota_request;
#define AP_SAMPLE_PERIOD_MIN_MS 200
#define AP_SAMPLE_PERIOD_MAX_MS 60000

static uint8_t json_to_switch_state(cJSON *item, uint8_t *state, uint8_t *toggle)
{
    *toggle = 0;
    if (item == NULL) return 0;
    if (cJSON_IsBool(item))
    {
        *state = cJSON_IsTrue(item) ? 1 : 0;
        return 1;
    }
    if (cJSON_IsNumber(item))
    {
        *state = (item->valueint != 0) ? 1 : 0;
        return 1;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        if (strcmp(item->valuestring, "on") == 0 || strcmp(item->valuestring, "ON") == 0 || strcmp(item->valuestring, "1") == 0)
        {
            *state = 1;
            return 1;
        }
        if (strcmp(item->valuestring, "off") == 0 || strcmp(item->valuestring, "OFF") == 0 || strcmp(item->valuestring, "0") == 0)
        {
            *state = 0;
            return 1;
        }
        if (strcmp(item->valuestring, "toggle") == 0 || strcmp(item->valuestring, "TOGGLE") == 0)
        {
            *toggle = 1;
            return 1;
        }
    }
    return 0;
}

static uint8_t json_to_u32_ms(cJSON *item, uint32_t *out_ms)
{
    if (item == NULL || out_ms == NULL) return 0;
    uint32_t v = 0;
    if (cJSON_IsNumber(item))
    {
        if (item->valuedouble <= 0) return 0;
        v = (uint32_t)item->valuedouble;
    }
    else if (cJSON_IsString(item) && item->valuestring != NULL)
    {
        int n = atoi(item->valuestring);
        if (n <= 0) return 0;
        v = (uint32_t)n;
    }
    else
    {
        return 0;
    }
    if (v < AP_SAMPLE_PERIOD_MIN_MS) v = AP_SAMPLE_PERIOD_MIN_MS;
    if (v > AP_SAMPLE_PERIOD_MAX_MS) v = AP_SAMPLE_PERIOD_MAX_MS;
    *out_ms = v;
    return 1;
}

void data_process_mqtt_msg(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0) 
    {
        printf("error: invalid buffer or length\r\n");
        return;

    }
    int start = -1;
    int end = -1;
    for (uint16_t i = 0; i < len; i++)
    {
        if (buf[i] == '{')
        {
            start = i;
            break;
        }
    }
    if (start < 0) 
    {
        printf("error: no JSON start found\r\n");
        return;
    }
    for (int i = (int)len - 1; i > start; i--)
    {
        if (buf[i] == '}')
        {
            end = i;
            break;
        }
    }
    if (end <= start) 
    {
        printf("error: no JSON end found\r\n");
        return;
    }

    int json_len = end - start + 1;
    if (json_len <= 0) 
    {
        printf("error: invalid JSON length\r\n");
        return;
    }
    if (json_len > 255) json_len = 255;
    char json_text[256];
    memcpy(json_text, buf + start, json_len);
    json_text[json_len] = '\0';

    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        printf("error: JSON parse failed\r\n");
        return;
    }

    cJSON *control = cJSON_GetObjectItemCaseSensitive(root, "control");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *node = root;
    if (control != NULL && cJSON_IsObject(control))
    {
        node = control;
    }
    else if (params != NULL && cJSON_IsObject(params))
    {
        node = params;
    }

    uint8_t state = 0;
    uint8_t toggle = 0;
    cJSON *item = NULL;

    item = cJSON_GetObjectItemCaseSensitive(node, "led0");
    if (json_to_switch_state(item, &state, &toggle))
    {
        if (toggle) LED0 = !LED0;
        else LED0 = state;
        printf("CTRL led0=%d\r\n", LED0);
    }

    item = cJSON_GetObjectItemCaseSensitive(node, "led1");
    if (json_to_switch_state(item, &state, &toggle))
    {
        if (toggle) LED1 = !LED1;
        else LED1 = state;
        printf("CTRL led1=%d\r\n", LED1);
    }

    item = cJSON_GetObjectItemCaseSensitive(node, "beep");
    if (json_to_switch_state(item, &state, &toggle))
    {
        uint8_t cur = PCF8574_ReadBit(BEEP_IO);
        if (toggle) state = cur ? 0 : 1;
        PCF8574_WriteBit(BEEP_IO, state);
        printf("CTRL beep=%d\r\n", state);
    }

    uint32_t new_period_ms = 0;
    item = cJSON_GetObjectItemCaseSensitive(node, "sample_ms");
    if (!json_to_u32_ms(item, &new_period_ms))
    {
        item = cJSON_GetObjectItemCaseSensitive(node, "sample_period_ms");
        json_to_u32_ms(item, &new_period_ms);
    }
    if (new_period_ms > 0)
    {
        g_ap_sample_period_ms = new_period_ms;
        printf("CTRL sample_ms=%lu\r\n", (unsigned long)g_ap_sample_period_ms);
    }

    cJSON *ota_tid = cJSON_GetObjectItemCaseSensitive(node, "tid");
    if (ota_tid == NULL) ota_tid = cJSON_GetObjectItemCaseSensitive(root, "tid");
    if (cJSON_IsString(ota_tid) && ota_tid->valuestring != NULL)
    {
        OTA_SetTaskId(ota_tid->valuestring);
    }
    else if (cJSON_IsNumber(ota_tid))
    {
        char tid_buf[24];
        snprintf(tid_buf, sizeof(tid_buf), "%d", ota_tid->valueint);
        OTA_SetTaskId(tid_buf);
    }

    cJSON *ota_upgrade = cJSON_GetObjectItemCaseSensitive(node, "ota_upgrade");
    if (ota_upgrade != NULL && cJSON_IsObject(ota_upgrade))
    {
        cJSON *ota_value = cJSON_GetObjectItemCaseSensitive(ota_upgrade, "value");
        if (ota_value != NULL) ota_upgrade = ota_value;
    }
    if (cJSON_IsBool(ota_upgrade))
    {
        if (cJSON_IsTrue(ota_upgrade))
        {
            g_ota_request = 1;
            printf("CTRL ota_upgrade=1\r\n");
        }
    }
    else if (cJSON_IsNumber(ota_upgrade))
    {
        if (ota_upgrade->valueint != 0)
        {
            g_ota_request = 1;
            printf("CTRL ota_upgrade=1\r\n");
        }
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (cJSON_IsString(cmd) && cmd->valuestring != NULL && json_to_switch_state(value, &state, &toggle))
    {
        if (strcmp(cmd->valuestring, "led0") == 0)
        {
            if (toggle) LED0 = !LED0;
            else LED0 = state;
            printf("CTRL led0=%d\r\n", LED0);
        }
        else if (strcmp(cmd->valuestring, "led1") == 0)
        {
            if (toggle) LED1 = !LED1;
            else LED1 = state;
            printf("CTRL led1=%d\r\n", LED1);
        }
        else if (strcmp(cmd->valuestring, "beep") == 0)
        {
            uint8_t cur = PCF8574_ReadBit(BEEP_IO);
            if (toggle) state = cur ? 0 : 1;
            PCF8574_WriteBit(BEEP_IO, state);
            printf("CTRL beep=%d\r\n", state);
        }
        else if (strcmp(cmd->valuestring, "sample_ms") == 0 || strcmp(cmd->valuestring, "sample_period_ms") == 0)
        {
            if (json_to_u32_ms(value, &new_period_ms))
            {
                g_ap_sample_period_ms = new_period_ms;
                printf("CTRL sample_ms=%lu\r\n", (unsigned long)g_ap_sample_period_ms);
            }
        }
        else if (strcmp(cmd->valuestring, "ota_upgrade") == 0)
        {
            g_ota_request = 1;
            printf("CTRL ota_upgrade=1\r\n");
        }
    }

    cJSON_Delete(root);
}
