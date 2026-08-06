#ifndef NETWORK_APP_H
#define NETWORK_APP_H

#include "MQTTPacket.h"
#include "MQTTConnect.h"
#include "MQTTPublish.h"
#include "MQTTSubscribe.h"
#include "transport.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "sensor_app.h"
#include "store_flash.h"
#include "wdog.h"


#define DEMO_WIFI_SSID          "ari"
#define DEMO_WIFI_PWD           "zhanglanxiong"
#define DEMO_ATKCLD_DEV_ID      "63152966615081879519"
#define DEMO_ATKCLD_DEV_PWD     "12345678"
 
//可以更改IP接入其他MQTT Broker
#define MQTT_BROKER_IP          "studio-mqtt.heclouds.com"
#define MQTT_BROKER_PORT        "1883"
#define MQTT_CLIENT_ID          "d0"//设备名称
#define MQTT_USER_NAME          "XHm5ltr9qA"//产品ID
#define MQTT_PASSWORD           "version=2018-10-31&res=products%2FXHm5ltr9qA%2Fdevices%2Fd0&et=2089352870&method=md5&sign=ZHWb%2F2TNh0C2aS8nBxenrQ%3D%3D"
#define MQTT_TOPIC_PUB          "$sys/XHm5ltr9qA/d0/thing/property/post"
#define MQTT_TOPIC_SUB          "$sys/XHm5ltr9qA/d0/thing/property/set"


// 需要用的跨文件队列（定义在 main.c，由本文件使用）
extern QueueHandle_t xAP3216CQueueForMQTT;
extern QueueHandle_t xUartRxQueue;

/**
 * @brief  查询当前 MQTT 是否已连接（对外只暴露这一条状态接口）
 * @retval 1=已连接  0=未连接
 */
uint8_t net_is_connected(void);



typedef struct
{
    // --- 业务数据与缓存 ---
    AP3216C_Data_t ap_data;             // 从队列取出的环境光传感器数据
    u8 key;                             // 按键状态（用于触发手动重连/断开网络）
    char json_buf[256];                 // 拼装 JSON 上报数据和读取 Flash 数据的通用字符串缓存区
    
    // --- 底层接收相关 ---
    uint16_t rx_len;                    // 接收到的串口透传数据包长度
    uint8_t *uart_buf;                  // 指向底层串口透传接收缓冲区的指针
    
    // --- MQTT 重连退避与状态机机制 ---
    uint32_t backoff_ms;                // 当前重连退避延时，失败会成倍增加（指数级退避算法）
    uint32_t max_backoff_ms;            // 最大重连退避延时上限（防止间隔变得无限大）
    TickType_t next_retry_tick;         // 下一次允许发起 MQTT 重连尝试的系统滴答时钟时刻
    uint8_t auto_reconnect;             // 标志：是否开启自动重连机制
    
    // --- MQTT Keep-Alive (心跳/Ping) 控制 ---
    TickType_t last_rx_tick;            // 最后一次接收到网络有效数据的时刻（以此判定是否需要发心跳保活）
    TickType_t last_ping_tick;          // 最后一次发出 PINGREQ(心跳请求包) 的时刻
    TickType_t ping_interval_tick;      // 发送心跳包的周期间隔（必须小于等于服务端的 KeepAlive）
    TickType_t ping_timeout_tick;       // 等待心跳响应 PINGRESP 的超时时限
    uint8_t waiting_pingresp;           // 标志：是否正在等待 Broker 的心跳响应
    
    // --- 离线存储(Flash)与统计处理 ---
    uint32_t cached_total;              // 统计：本次开机以来总计存入 Flash 的离线滞留数据条数
    uint32_t sent_total;                // 统计：本次开机以来从 Flash 成功补传到云端的离线数据条数
    TickType_t last_report_tick;        // 辅助记录上一次在串口打印统计信息的时刻
    
    // --- WiFi 底层链路恢复 ---
    uint8_t wifi_rejoin_needed;         // 标志：如果 ESP8266 掉线，标记为 1 需要重新通过 AT 指令连 WiFi
    TickType_t wifi_retry_tick;         // 下一次允许发起重连 WiFi AP 的系统滴答时刻
    
    // --- MQTT 协议栈相关参数 (Paho MQTT 原生状态) ---
    MQTTPacket_connectData connect_data;// MQTT 建立连接时的参数配置 (Client ID, Username, Password 等)
    unsigned char sessionPresent;       // 握手响应：Broker 是否保存了一份属于该客户端的历史会话
    unsigned char connack_rc;           // 握手响应：Broker 返回的连接许可码 (0 表示连接成功接受，其他为拒绝原因)
    int packet_len;                     // 当前组装生成的或正向解析收到的整个 MQTT 报文总长度
    
    // --- MQTT 订阅配置参数 ---
    MQTTString topic_string;            // Paho：收到下发报文时，截取保存出所对应的 Topic
    MQTTString subscribe_topic;         // Paho：本地准备发往服务器请求订阅的 Topic 字符串
    int granted_qos[1];                 // Broker 返回确认授权的该 Topic 订阅质量 (QoS)
    int sub_count;                      // 准备请求订阅的 Topic 个数 (代码里目前用来请求 1 个)
    unsigned short sub_packet_id;       // 本地分发给 Subscribe(订阅申请包) 的报文标识符 ID
    
    // --- 收到的有效下发 MQTT 报文属性细节 ---
    unsigned char dup;                  // 标志：收到的该报文是否是服务端由于没收到应答而重复投递的
    int qos;                            // 标志：收到的该报文所使用的 QoS 级别 (0, 1, 或 2)
    unsigned char retained;             // 标志：收到的该报文是否是之前保留(Retain)在 Broker 上的遗老消息
    unsigned short packet_id;           // 解析出的通用数据包 ID 数值
    unsigned char *payload;             // 解析出的核心负载数据(即下发的控制 JSON)所在的内存指针起始位
    int payload_len;                    // 核心负载数据的字节长度
} NetTaskContext;

/* ====================================================================
 * 网络与 MQTT 应用层任务对外接口函数声明
 * ==================================================================== */

// 网络与 MQTT 主任务函数 (创建为 FreeRTOS 任务)
void net_task(void *pv);


#endif // NETWORK_APP_H

