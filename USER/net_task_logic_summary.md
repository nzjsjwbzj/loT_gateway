# net_task() 核心逻辑总结

## 📋 整体框架（4 个主要部分）

```
┌─────────────────────────────────────────────────────┐
│                   net_task() 主循环                 │
├─────────────────────────────────────────────────────┤
│ 1️⃣ 按键处理                                          │
│    ├─ KEY0：连接 MQTT Broker                        │
│    └─ KEY1：断开 MQTT 连接                          │
│                                                     │
│ 2️⃣ 断线重连（指数退避算法）                         │
│    ├─ 未连接且自动重连开启                          │
│    ├─ 等待延迟时间到期                              │
│    ├─ 连接失败 → 延迟翻倍，继续重试                 │
│    └─ 连接成功 → 重置延迟，开始通信                 │
│                                                     │
│ 3️⃣ 保活心跳（Keep Alive）                           │
│    ├─ 每 20 秒发送 PING 请求                        │
│    ├─ 等待 PINGRESP 响应（超时 5 秒）               │
│    └─ 超时 → 连接断开，触发重连                     │
│                                                     │
│ 4️⃣ 数据收发                                         │
│    ├─ 发送：传感器数据 → JSON → PUBLISH 包          │
│    └─ 接收：MQTT 包 → 解析 → 处理（PUBLISH/SUBACK）│
└─────────────────────────────────────────────────────┘
```

---

## 🔑 关键变量含义

| 变量 | 类型 | 含义 |
|------|------|------|
| `con_status` | bool | 连接状态（1=已连接，0=未连接） |
| `mqtt_sock` | int | Socket ID（-1=未连接，≥0=已连接） |
| `auto_reconnect` | uint8 | 自动重连开关 |
| `backoff_ms` | uint32 | 当前重连延迟（毫秒） |
| `next_retry_tick` | TickType | 下次重连时刻 |
| `last_rx_tick` | TickType | 最后一次收数据的时刻 |
| `waiting_pingresp` | uint8 | 正在等 PING 响应标志 |

---

## 📍 执行流程（按顺序）

### **步骤 1：按键处理**

```c
KEY0：连接请求
  ├─ auto_reconnect = 1（启用自动重连）
  └─ request_connect = 1（立即尝试连接）

KEY1：断开请求
  ├─ 发送 DISCONNECT 包
  ├─ 关闭 socket
  ├─ con_status = 0
  └─ auto_reconnect = 1（用户可重新连接）
```

### **步骤 2：连接判断**

```c
if (!con_status || mqtt_sock < 0)  // 未连接
{
    // 检查是否可以尝试连接
    can_try = request_connect  // 用户主动请求
          || (auto_reconnect && now >= next_retry_tick)  // 自动重连且延迟到期
    
    if (can_try)
    {
        // 尝试连接...
    }
}
```

### **步骤 3：连接过程**

```
3.1 检查 WiFi 连接
    └─ 如果掉线，重新连接 WiFi

3.2 打开 TCP 连接
    └─ mqtt_sock = transport_open(IP, PORT)

3.3 发送 MQTT CONNECT 包
    └─ MQTTSerialize_connect() 打包
    └─ transport_sendPacketBuffer() 发送

3.4 等待 CONNACK 响应
    ├─ MQTTPacket_read() 接收
    ├─ MQTTDeserialize_connack() 解析
    └─ 检查返回码是否为 MQTT_CONNECTION_ACCEPTED

3.5 连接成功 ✓
    ├─ con_status = 1
    ├─ 重置重连延迟：backoff_ms = 1000
    ├─ 初始化心跳参数：last_rx_tick = now
    └─ 发送订阅请求

3.6 连接失败 ✗
    ├─ con_status = 0
    ├─ 翻倍重连延迟：backoff_ms <<= 1
    └─ 计算下次重连时刻：next_retry_tick = now + backoff_ms
```

### **步骤 4：心跳保活**

```c
if (con_status && mqtt_sock >= 0)
{
    if (waiting_pingresp)  // 正在等 PING 响应
    {
        if ((now - last_ping_tick) > 5秒)  // 超时
        {
            // 断开连接，触发重连
            con_status = 0;
            waiting_pingresp = 0;
        }
    }
    else if ((now - last_rx_tick) > 20秒)  // 超过 20 秒没收数据
    {
        // 发送 PING 请求
        MQTTSerialize_pingreq() → transport_sendPacketBuffer()
        waiting_pingresp = 1;
    }
}
```

### **步骤 5：发送传感器数据**

```c
if (xQueueReceive(xAP3216CQueueForMQTT, ...))  // 有数据
{
    // 1. 用 cJSON 把数据转换为 JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "als", ap3216c_data.als);
    cJSON_PrintPreallocated(root, json_buf, ...);

    // 2. 把 JSON 序列化为 MQTT PUBLISH 包
    MQTTSerialize_publish(mqtt_send_buf, ...);

    // 3. 通过 socket 发送
    transport_sendPacketBuffer(mqtt_sock, ...);
}
```

### **步骤 6：接收和处理 MQTT 数据**

```c
len = transport_getdatanb(NULL, mqtt_recv_buf, ...);  // 接收数据
if (len > 0)
{
    // 6.1 解析可变长度编码，分离多个报文
    while (offset < len)
    {
        // 计算每个报文的长度和边界
        // ...
        
        // 6.2 根据报文类型处理
        if (PUBLISH 报文)
        {
            // 打印主题和载荷
            // 如果 QoS>0，发送 PUBACK 确认
        }
        else if (SUBACK 报文)
        {
            // 打印"订阅确认"
        }
        else if (PINGRESP 报文)
        {
            // 清除等待标志，更新接收时刻
            waiting_pingresp = 0;
            last_rx_tick = now;
        }
        
        offset += packet_len;  // 处理下一个报文
    }
}
```

---

## 🎯 关键概念速记

### **1. 指数退避重连**
```
失败 1 次 → 等待 1s    → 失败 2 次 → 等待 2s
失败 3 次 → 等待 4s    → 失败 4 次 → 等待 8s
...                    → 最多等待 32s

成功 → 重置为 1s
```

### **2. 心跳保活**
```
正常收数据 → last_rx_tick 更新
20 秒后没收数据 → 发 PING 请求
PING 请求发出后等 5 秒
5 秒内收到 PINGRESP → 继续工作
5 秒内没收到 → 断开重连
```

### **3. 数据流向**
```
传感器数据 → xAP3216CQueueForMQTT 队列
     ↓
本任务取出
     ↓
cJSON 转 JSON 字符串
     ↓
MQTTSerialize_publish 打包
     ↓
transport_sendPacketBuffer 发送
```

---

## 💡 面试简洁说法

> **"net_task 负责 MQTT 通信和数据上传。
>
> 核心功能：
> 1. **连接管理** - 自动重连，使用指数退避避免频繁重试
> 2. **心跳保活** - 每 20 秒发送 PING，检测连接是否掉线
> 3. **数据上传** - 从队列取传感器数据，转 JSON，发 MQTT PUBLISH
> 4. **数据接收** - 处理 Broker 推送的消息，分离多报文，根据类型响应
>
> 整个设计是完全异步的，不会阻塞其他任务。"**

---

## 📊 状态转移图

```
                    ┌─────────────────┐
                    │   未连接状态     │
                    │ con_status = 0   │
                    │ mqtt_sock = -1   │
                    └────────┬─────────┘
                             │
                    按 KEY0 或自动重连
                             │
                    连接成功  │  连接失败
                   ┌─────────▼──────────┐
                   │   已连接状态        │
                   │ con_status = 1      │
                   │ mqtt_sock ≥ 0       │◄──────────┐
                   │ 可收发数据          │           │
                   └────────┬────────────┘           │
                            │                        │
                 ┌──────────┴──────────────┐          │
                 │                        │          │
            定期发 PING         收发数据  │          │
            (20 秒一次)        发传感器   │          │
                 │              数据      │          │
                 │                        │          │
        20 秒内收 PINGRESP      ┌─────────▼──────┐   │
                 │              │               │   │
              成功  │失败        │ 心跳超时      │   │
                 │              │ (5 秒无响应)  │   │
                 │              │               │   │
                 └──────────────┼───────────────┘   │
                                │                   │
                         断开连接重连◄───────────────┘
```

---

## ✅ 快速检查清单

- [ ] 理解按键触发连接/断开
- [ ] 理解指数退避重连机制（1秒→2秒→4秒...)
- [ ] 理解心跳机制（20秒发PING，5秒等响应）
- [ ] 理解数据流：传感器→队列→JSON→PUBLISH→发送
- [ ] 理解多报文分离（offset逐个处理）
- [ ] 理解报文处理（PUBLISH/SUBACK/PINGRESP）

