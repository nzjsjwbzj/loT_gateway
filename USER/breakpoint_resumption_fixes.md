# 断点续传逻辑修复总结

## 📝 问题分析与解决

### 🔴 问题 1️⃣：flash_valid_count 永远为 0
**原因：** 
- `init_task()` 中局部变量 `con_status` 遮蔽了全局变量
- 导致网络判断混乱

**修复方案：**
```c
// ❌ 原来（错误）
void init_task(void *pv)
{
    u8 ret;
    char ip_buf[16];
    bool  link_status;
    bool  con_status;  // ← 局部变量，遮蔽全局的！
    // ...
}

// ✅ 修复后
void init_task(void *pv)
{
    u8 ret;
    char ip_buf[16];
    bool  link_status;
    // ✅ 删除这行，使用全局的 con_status
    // ...
}
```

**修复位置：** `main.c` 第 435-439 行

---

### 🔴 问题 2️⃣：flash_store_peek() 频繁调用 rescan 破坏计数
**原因：**
- 没找到数据时自动调用 `flash_store_rescan()`
- 导致 `flash_valid_count` 被重新计算并可能清零

**修复方案：**
```c
// ❌ 原来（错误）
static int flash_store_peek(...)
{
    // ... 扫描逻辑 ...
    if (!found)
    {
        flash_store_rescan();  // ← 破坏计数！
        return 0;
    }
}

// ✅ 修复后
static int flash_store_peek(...)
{
    // ... 扫描逻辑 ...
    if (!found)
    {
        printf("WARNING: Flash data not found! valid_count=%lu\r\n", flash_valid_count);
        // ✅ 只记录日志，不重新扫描
        return 0;
    }
}
```

**修复位置：** `main.c` 第 273-310 行

---

### 🔴 问题 3️⃣：ap3216c_task 与 net_task 同时存储数据到 Flash
**原因：**
- `ap3216c_task()` 中队列满时或网络未连接时都会存储到 Flash
- `net_task()` 中网络未连接时也会存储到 Flash
- 导致同一条数据被存两次，`cached_total` 与实际存储不匹配

**修复方案：**
```c
// ❌ 原来（错误）
void ap3216c_task(void *pv)
{
    // ...
    if (con_status && xAP3216CQueueForMQTT != NULL)
    {
        if (xQueueSend(xAP3216CQueueForMQTT, &ap3216c_data, 0) != pdTRUE)
        {
            flash_store_push(...);  // ← 队列满时存储
        }
    }
    else
    {
        flash_store_push(...);      // ← 网络断开时存储
    }
}

// ✅ 修复后
void ap3216c_task(void *pv)
{
    // ...
    // ✅ 只发送给两个队列，不存储 Flash
    xQueueSend(xAP3216CQueue, &ap3216c_data, 0);           // LCD 显示
    xQueueSend(xAP3216CQueueForMQTT, &ap3216c_data, 0);    // MQTT 上报
    // Flash 存储由 net_task 统一处理！
}
```

**修复位置：** `main.c` 第 552-585 行

---

## ✅ 改进的工作流程

```
┌─────────────────────────────────────────────┐
│ 正常状态（网络连接）                         │
├─────────────────────────────────────────────┤
│ ap3216c_task                                 │
│  ├─ 采集传感器数据                           │
│  ├─ 发送到 xAP3216CQueue（LCD 显示）        │
│  └─ 发送到 xAP3216CQueueForMQTT（MQTT 上报）│
│         ↓                                    │
│ net_task                                     │
│  ├─ 检查 con_status = 1（网络连接）         │
│  ├─ 接收队列数据                            │
│  ├─ 直接发送 MQTT                           │
│  └─ printf("New Data Sent OK")              │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│ 网络断开                                     │
├─────────────────────────────────────────────┤
│ ap3216c_task                                 │
│  ├─ 采集传感器数据                           │
│  └─ 发送到队列（无网络也要发）               │
│         ↓                                    │
│ net_task                                     │
│  ├─ 检查 con_status = 0（网络未连接）       │
│  ├─ 接收队列数据，转 JSON                    │
│  ├─ flash_store_push() 存到 Flash           │
│  ├─ printf("Data Stored to Flash...")       │
│  ├─ flash_valid_count++                     │
│  └─ 继续重连...                              │
└─────────────────────────────────────────────┘

┌─────────────────────────────────────────────┐
│ 网络恢复                                     │
├─────────────────────────────────────────────┤
│ net_task                                     │
│  ├─ 检查 flash_valid_count > 0 ✅           │
│  ├─ flash_store_peek() 读一条               │
│  ├─ printf("Read from Flash: len=X...")     │
│  ├─ 发送 MQTT                               │
│  ├─ printf("Flash Data Sent OK...")         │
│  ├─ flash_store_mark_sent() 标记删除        │
│  ├─ flash_valid_count--                     │
│  └─ 重复直到 pending=0                      │
└─────────────────────────────────────────────┘
```

---

## 🔍 修复前后对比

| 现象 | 修复前 | 修复后 |
|------|--------|--------|
| **pending 值** | 永远 0 | 正确显示缓存数据数量 |
| **断网时日志** | 无日志，看不到是否存储 | `Data Stored to Flash! pending=X` |
| **恢复时日志** | 无日志，不知道是否发送 | `Flash Data Sent OK (pending=X)` |
| **数据重复** | 可能存两次 | 只在 net_task 存一次 |
| **con_status** | 判断混乱 | 清晰准确 |

---

## 📊 修复后的串口打印预期

```
初始化：
Flash Init: read_idx=0, write_idx=0, valid_count=0
IP: 192.168.87.100

网络连接：
Connecting to MQTT Broker...
TCP Connected, Entered Transparent Mode.
MQTT Connected!
MQTT Subscribe Sent.

正常工作 5 秒打印：
cache_total=0, sent_total=0, pending=0

[拔网线，模拟网络断开]
Transport Open Failed!

网络断开时收集数据：
Data Stored to Flash! (cached=1, pending=1)
Data Stored to Flash! (cached=2, pending=2)
Data Stored to Flash! (cached=3, pending=3)

5 秒统计打印（现在能看到缓存！）：
cache_total=3, sent_total=0, pending=3  ← ✅ pending 不是 0 了！

[插上网线，网络恢复]
Connecting to MQTT Broker...
TCP Connected, Entered Transparent Mode.
MQTT Connected!
MQTT Subscribe Sent.

网络恢复，自动上传缓存数据：
Read from Flash: len=31, pending=3
Flash Data Sent OK (pending=3)
Read from Flash: len=31, pending=2
Flash Data Sent OK (pending=2)
Read from Flash: len=31, pending=1
Flash Data Sent OK (pending=1)

缓存数据全部发送完毕：
cache_total=3, sent_total=3, pending=0  ← ✅ pending 变 0，任务完成！

恢复正常发送：
Read from Flash: len=31, pending=0
New Data Sent OK
```

---

## 🎯 验证清单

修复后，你应该看到：

- [ ] `pending` 值在网络断开时增加
- [ ] `pending` 值在网络恢复后逐条递减
- [ ] 最终 `pending=0` 表示所有缓存数据都发送完毕
- [ ] `cached_total` 和 `sent_total` 数字匹配
- [ ] 没有 `pending > 0` 但长时间无日志的情况
- [ ] Flash 初始化成功时看到 `Flash Init: ...` 的日志

---

## 📝 修改文件列表

| 文件 | 修改位置 | 修改内容 |
|------|---------|--------|
| `main.c` | 第 435-439 行 | 删除 init_task 中的局部 con_status 声明 |
| `main.c` | 第 273-310 行 | 移除 flash_store_peek 中的 rescan 调用 |
| `main.c` | 第 552-585 行 | 清理 ap3216c_task，只发队列不存 Flash |
| `main.c` | 第 842-860 行 | 添加调试打印到网络断开存储部分 |
| `main.c` | 第 861-896 行 | 改进 Flash 数据发送逻辑 |

---

## 💡 总结

这三个修复解决了断点续传的核心问题：
1. **全局变量混乱** → 使用正确的 con_status
2. **计数被破坏** → 移除不必要的 rescan
3. **重复存储** → 单一责任原则，net_task 统一处理 Flash

修复后，你的断点续传逻辑会完全工作，能清晰看到缓存、发送、恢复的全过程！ ✅
