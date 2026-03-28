#ifndef PROTOCOL_ONENET_H
#define PROTOCOL_ONENET_H

#include <stdint.h>
#include "cJSON.h"

// 解析下发 MQTT 数据，并执行控制硬件等协议指令
void data_process_mqtt_msg(uint8_t *buf, uint16_t len);

#endif // PROTOCOL_ONENET_H