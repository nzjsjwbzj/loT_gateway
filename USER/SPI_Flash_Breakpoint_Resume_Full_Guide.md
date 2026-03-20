# STM32F429 SPI Flash 断点续传完整指南

## 目录

1. [整体架构](#整体架构)
2. [数据结构设计](#数据结构设计)
3. [核心概念](#核心概念)
4. [工作流程图](#工作流程图)
5. [关键函数详解](#关键函数详解)
6. [实际应用场景](#实际应用场景)
7. [常见问题与 Bug](#常见问题与-bug)

---

## 整体架构

### 系统框图

```
┌─────────────────────────────────────────────────────────────────┐
│                      STM32F429 MCU                              │
│                   (FreeRTOS + MQTT)                             │
└──────────────────────────┬──────────────────────────────────────┘
         │
         ├─── AP3216C 传感器 ─────────────┐
         │                                │
         ├─── DHT11 温湿度传感器 ─────────┤
         │                                │ xAP3216CQueueForMQTT
         ├─── WiFi 模块 (ATK-MW8266D) ──┐ │
         │       ↑                       │ │
         │       │ MQTT 通信             │ │
         │       │ (正常连接)            ▼ ▼
         │       │        ┌─────────────────────┐
         │       │        │  net_task           │
         │       │        │  (网络任务)         │
         │       │        │                     │
         │       │        │ - 维护 MQTT 连接   │
         │       │        │ - 发送实时数据     │
         │       │        │ - 读取并上传 Flash │
         │       │        └──────────┬──────────┘
         │       │                   │
         │       └─── (Connected) ───┘
         │
         └──── (Disconnected) ──────┐
                                     │
                                     ▼
                        ┌─────────────────────────────┐
                        │   SPI Flash 存储层          │
                        │   (W25Q256 / W25Q128)       │
                        │                             │
                        │ 环形缓冲 (1MB)              │
                        │ - 4096 条记录               │
                        │ - 每条 256 字节             │
                        │                             │
                        │ 记录格式:                   │
                        │ ┌─────────────────────────┐ │
                        │ │ Flag (1B) │ Len (2B)    │ │
                        │ │ JSON Data (253B)        │ │
                        │ └─────────────────────────┘ │
                        │                             │
                        │ 状态标志:                   │
                        │ - 0xFF = 空                 │
                        │ - 0xA5 = 有效(未发送)      │
                        │ - 0x00 = 已发送            │
                        └─────────────────────────────┘
```

### 设计思路

**为什么用 SPI Flash 存储？**


| 方案              | 存储容量 | 掉电保存 | 写入频率   | 管理复杂度     | 项目适用度          |
| ----------------- | -------- | -------- | ---------- | -------------- | ------------------- |
| SRAM 内存缓冲     | ≤64KB   | ❌       | 不限       | 低             | ❌ 掉电丢失         |
| STM32 内部 Flash  | ≤1MB    | ✅       | 受限(寿命) | 高(擦除粒度大) | ⚠️ 频繁写消耗寿命 |
| **SPI NOR Flash** | 1-256MB  | ✅       | 高         | 中             | ✅**最适合**        |
| SD 卡 + FatFs     | ≥1GB    | ✅       | 高         | 高(文件系统)   | ⚠️ 过度设计       |

**本项目选择 SPI NOR Flash 的原因：**

- ✅ 掉电保存数据（不丢失离线时的采集数据）
- ✅ 容量足（1MB > 几天的数据）
- ✅ 无文件系统开销（FatFs 占用内存和 CPU）
- ✅ 固定记录大小便于循环管理
- ✅ 中断期间不中断 MQTT 心跳线程

---

## 数据结构设计

### 1. Flash 内存布局

```
Flash 起始地址: 0x00100000
Flash 大小:     1MB (1024KB)
扇区大小:       4KB (SPI Flash 最小擦除单位)
记录大小:       256 字节 (2^8，便于索引计算)

总记录数 = 1MB / 256B = 4096 条
单扇区内记录数 = 4KB / 256B = 16 条

地址划分:
┌──────────────────────────────┐ 0x00100000
│ Sector 0 (记录 0-15)         │
├──────────────────────────────┤ 0x00101000 (+ 4KB)
│ Sector 1 (记录 16-31)        │
├──────────────────────────────┤ 0x00102000
│ ...                          │
├──────────────────────────────┤ 0x00200000 (+ 1MB)
│ Flash 结束                   │
└──────────────────────────────┘
```

### 2. 单条记录结构

```c
typedef struct 
{
    uint8_t flag;           // [0]     标志位
    uint16_t len;           // [1-2]   JSON 数据长度
    char data[253];         // [3-255] JSON 数据 + 填充
} FlashRecord;              // 总计 256 字节
```

**记录内容示例：**

```
位置        内容              说明
────────────────────────────────────────────
0x00100000  0xA5              Flag = VALID (未发送)
0x00100001  0x2C              Len = 44 (低字节)
0x00100002  0x00              Len = 44 (高字节)
0x00100003  {"als":1234,...   JSON 数据开始 (44 字节)
0x00100035  0xFF 0xFF 0xFF... 剩余填充为 0xFF

0x00100100  0x00              Flag = SENT (已发送，标记为垃圾)
0x00100101  ...               不再需要

0x00100200  0xFF              Flag = EMPTY (空槽)
0x00100201  ...               未使用
```

### 3. 全局状态变量

```c
// Flash 指针（循环缓冲的核心）
static uint32_t flash_read_index = 0;      // 指向第一条待发送的记录
static uint32_t flash_write_index = 0;     // 指向下一条空闲槽位
static uint32_t flash_valid_count = 0;     // 当前未发送的记录数
static uint8_t flash_ready = 0;            // Flash 是否初始化成功

// 示意：
//
// Flash: [0xA5] [0xA5] [0xA5] [0x00] [0x00] [0xFF] [0xFF] ...
//         ▲                          ▲            ▲
//         │                          │            │
//    read_index                      │        write_index
//    (待发送)                    (已发送)      (空槽)
//    valid_count = 3
```

---

## 核心概念

### 1. 标志位（Flag）的三种状态


| 标志值 | 符号               | 含义     | 用途                   |
| ------ | ------------------ | -------- | ---------------------- |
| `0xFF` | `FLASH_FLAG_EMPTY` | 空槽     | 可以写入新数据         |
| `0xA5` | `FLASH_FLAG_VALID` | 有效数据 | 网络恢复后优先读取     |
| `0x00` | `FLASH_FLAG_SENT`  | 已发送   | 标记垃圾数据，等待覆盖 |

**状态转移：**

```
┌──────────────────────────────────────────┐
│                                          │
│  EMPTY (0xFF)                            │
│         │                                │
│         │ (写入新数据)                    │
│         ▼                                │
│  VALID (0xA5) ─── (发送成功) ──► SENT (0x00)
│                                  (继续覆盖)
│         ▲                         │
│         └─────────────────────────┘
│              (覆盖垃圾数据)
│
└──────────────────────────────────────────┘
```

### 2. 循环缓冲与 Wrap-Around

```c
// 当 write_index 到达末尾时自动回绕
if (flash_write_index >= FLASH_RECORD_COUNT)
{
    flash_write_index = 0;  // 回到起点，覆盖最老的已发送数据
}

// 模运算实现：
new_index = (old_index + 1) % FLASH_RECORD_COUNT;
```

**举例：**

```
FLASH_RECORD_COUNT = 4096

写第 4095 条: write_index = 4095
写第 4096 条: write_index = (4095 + 1) % 4096 = 0 (回绕到起点)
写第 4097 条: write_index = (0 + 1) % 4096 = 1

这样形成了一个环形缓冲，最老的已发送数据被覆盖。
```

### 3. 扇区擦除与批量删除

SPI Flash 的最小可擦除单位是 **4KB 扇区**（16 条记录）。

```c
#define FLASH_RECORDS_PER_SECTOR (FLASH_SECTOR_SIZE / FLASH_RECORD_SIZE)
                                  = 4096 / 256 = 16

// 某条记录所在的扇区
uint32_t sector_start = (index / 16) * 16;
uint32_t sector_end = sector_start + 16;

// 示例：
// 记录 0-15 在 Sector 0
// 记录 16-31 在 Sector 1
// 记录 32-47 在 Sector 2
// ...
```

**为什么不能单独擦除一条记录？**

- SPI Flash 硬件特性：一次至少擦除 4KB
- 擦除的同时会把整个扇区变为 0xFF（全空）
- 必须一次性写入 16 条记录，或接受"垃圾数据"占用空间

**解决方案：标记已发送，而不是立刻删除**

```
已发送的记录不删除，只把 flag 改为 0x00
当整个扇区的 16 条都发送完后，才擦除整个扇区
新数据写入时覆盖垃圾数据（flag = 0x00）
```

---

## 工作流程图

### 完整的断点续传流程

```
设备启动
  │
  ▼
┌──────────────────────────────────┐
│ 1. Flash 初始化 (flash_store_init)│
│ - 扫描所有 4096 条记录           │
│ - 统计有效数据个数               │
│ - 设置 read_index/write_index    │
└──────────────────┬───────────────┘
                   │
        ┌──────────┴──────────┐
        │                     │
        ▼                     ▼
   MQTT 连接成功         MQTT 连接失败
        │                     │
        │                     ▼
        │              ┌─────────────────┐
        │              │ 后台采集数据     │
        │              │ (AP3216C, DHT11)│
        │              │ 存入队列         │
        │              └────────┬────────┘
        │                       │
        │              ┌────────▼────────┐
        │              │ Flash 满了？     │
        │              └─┬────────────┬──┘
        │                │            │
        │                │ 是          │ 否
        │                │            │
        │        ┌───────▼────────┐  │
        │        │ 擦除最老扇区   │  │
        │        │ (包含16条垃圾) │  │
        │        └────────────────┘  │
        │                             │
        │        ┌────────────────────┘
        │        │
        │        ▼
        │  ┌──────────────────────┐
        │  │ Flash Push 新数据    │
        │  │ flag=0xA5, len, json │
        │  │ write_index++        │
        │  │ valid_count++        │
        │  └──────────────────────┘
        │
        ▼
  ┌──────────────────────────────┐
  │ 2. 网络恢复 (MQTT 重新连接)  │
  │    读取 valid_count > 0      │
  └──────────┬───────────────────┘
             │
             ▼
  ┌──────────────────────────────┐
  │ 3. Flash Peek (读取待发数据) │
  │ - 从 read_index 开始扫描     │
  │ - 找第一条 flag=0xA5         │
  │ - 读取 len 和 JSON data      │
  │ - 返回给 net_task            │
  └──────────┬───────────────────┘
             │
             ▼
  ┌──────────────────────────────┐
  │ 4. MQTT Publish (发送数据)   │
  │ - 通过 MQTT 上传 JSON        │
  │ - 等待确认                   │
  └──────────┬───────────────────┘
             │
             ▼
  ┌──────────────────────────────┐
  │ 5. Mark Sent (标记已发送)    │
  │ - flag[index] = 0x00         │
  │ - valid_count--              │
  │ - read_index 推进到下一条0xA5│
  └──────────┬───────────────────┘
             │
             ▼
  ┌──────────────────────────────┐
  │ 判断是否还有待发数据         │
  │ (valid_count > 0?)           │
  └──┬──────────────────────────┬┘
     │ 有                        │ 没有
     │                           │
     │                    ┌──────▼──────┐
     │                    │ 实时发送模式│
     │                    │ (不读 Flash) │
     └────────┬───────────┴─────────────┘
              │
              ▼
    (循环回到 Flash Peek 或实时发送)
```

---

## 关键函数详解

### 1. `flash_store_init()` - 初始化 Flash

**何时调用：** 系统启动时（在 `init_task`）

**功能：** 扫描 Flash，恢复断点续传的状态

```c
static void flash_store_init(void)
{
    // 步骤 1: 检查 Flash 是否就绪
    if (!flash_ready) return;
  
    // 步骤 2: 读取第一个字节，检查是否有垃圾数据
    uint8_t first_flag = 0xFF;
    W25QXX_Read(&first_flag, FLASH_DATA_START, 1);
  
    // 步骤 3: 如果第一个字节不是有效的状态码，格式化整个 Flash
    // (防止 Flash 内有随机垃圾导致扫描崩溃)
    if (first_flag != 0xFF && first_flag != 0xA5 && first_flag != 0x00)
    {
        printf("Flash corrupted, formatting...\r\n");
        flash_store_format();
        return;
    }
  
    // 步骤 4: 调用 rescan 恢复状态
    flash_store_rescan();
  
    // 步骤 5: 打印初始化结果（用于调试）
    printf("Flash Init: read=%lu, write=%lu, valid=%lu\r\n",
           flash_read_index, flash_write_index, flash_valid_count);
}
```

**典型输出：**

```
Flash Init: read=42, write=120, valid=78
   ↑        ↑           ↑            ↑
   │        │           │            └─ 有 78 条未发送的数据
   │        │           └─ 下一个空槽在 120
   │        └─ 第一条待发送的在位置 42
   └─ 说明 Flash 中有数据要恢复
```

---

### 2. `flash_store_rescan()` - 扫描并恢复状态

**何时调用：**

- `flash_store_init()` 初始化时
- `flash_store_push()` 擦除扇区时
- `flash_store_peek()` 找不到数据时（自救）

**功能：** 遍历整个 Flash，统计有效数据，找出指针位置

```c
static void flash_store_rescan(void)
{
    uint8_t flag = 0;
    uint32_t first_empty = 0xFFFFFFFF;    // 第一个空槽位置
    uint32_t first_valid = 0xFFFFFFFF;    // 第一个有效数据位置
    uint32_t valid_count = 0;              // 临时计数
  
    // 步骤 1: 遍历所有 4096 条记录
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        W25QXX_Read(&flag, flash_record_addr(i), 1);
      
        // 统计有效数据
        if (flag == FLASH_FLAG_VALID)
        {
            valid_count++;
            if (first_valid == 0xFFFFFFFF)
                first_valid = i;
        }
      
        // 找第一个空槽
        if ((flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT) 
            && first_empty == 0xFFFFFFFF)
        {
            first_empty = i;
        }
    }
  
    // 步骤 2: 根据扫描结果设置指针
    flash_valid_count = valid_count;
  
    if (first_valid == 0xFFFFFFFF)
    {
        // Flash 是空的或全是垃圾
        flash_read_index = (first_empty == 0xFFFFFFFF) ? 0 : first_empty;
        flash_write_index = flash_read_index;
        printf("Flash empty\r\n");
    }
    else
    {
        // Flash 有有效数据
        flash_read_index = first_valid;    // 指向第一条待发送
        flash_write_index = (first_empty == 0xFFFFFFFF) 
                          ? first_valid 
                          : first_empty;   // 指向第一个空槽或回绕
        printf("Rescan: valid=%lu, read=%lu, write=%lu\r\n",
               valid_count, first_valid, flash_write_index);
    }
}
```

**时间复杂度：** O(n)，n = 4096 条记录，读取约 4KB 数据，耗时 ~10-50ms

**为什么需要 rescan？**

```
情景：某个扇区要被擦除（覆盖）

擦除前：
  Flash: [0xA5] [0xA5] [0x00] [0x00] | [0xA5] [0xA5] ...
         0-3   4-7   8-11 12-15    16-19 20-23
         └─────── read 指向这里       └─ 下一个有效

擦除第一个扇区后：
  Flash: [0xFF] [0xFF] ... [0xFF] | [0xA5] [0xA5] ...
         0-15 (全空，16条)          16-19 20-23
       
问题：read_index 原本指向 0，现在该指向 16！
解决：rescan 找到 first_valid = 16，更新 read_index = 16
```

---

### 3. `flash_store_push()` - 写入新数据

**何时调用：**

- `ap3216c_task()` 采集数据时（若 MQTT 未连接）
- `net_task()` 离线时收到队列数据时

**功能：** 将 JSON 数据写入 Flash，维护循环缓冲

```c
static void flash_store_push(const char *data, uint16_t len)
{
    if (!flash_ready) return;
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 1: 查找空槽 (从 write_index 开始)
    // ═══════════════════════════════════════════════════════════
    uint8_t flag = 0;
    uint32_t empty_idx = 0xFFFFFFFF;
  
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t idx = (flash_write_index + i) % FLASH_RECORD_COUNT;
        W25QXX_Read(&flag, flash_record_addr(idx), 1);
      
        if (flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT)
        {
            // 找到可用槽位
            empty_idx = idx;
            break;
        }
    }
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 2: 处理两种情况
    // ═══════════════════════════════════════════════════════════
    if (empty_idx != 0xFFFFFFFF)
    {
        // 情况 A: 找到空槽，可以直接写
        flash_write_index = empty_idx;
    }
    else
    {
        // 情况 B: Flash 满了，需要擦除一个扇区
        // 这是最复杂的部分！
      
        uint32_t sector_start = (flash_write_index / FLASH_RECORDS_PER_SECTOR) 
                              * FLASH_RECORDS_PER_SECTOR;
        uint32_t sector_end = sector_start + FLASH_RECORDS_PER_SECTOR;
      
        printf("Flash full, erasing sector [%lu-%lu]\r\n", sector_start, sector_end);
      
        // 擦除扇区
        flash_store_erase_sector(flash_write_index);
      
        // 如果 read_index 在被擦除的扇区内，需要 rescan 重新定位
        if (flash_read_index >= sector_start && flash_read_index < sector_end)
        {
            printf("read_index in erased sector, rescanning...\r\n");
            flash_store_rescan();
        }
      
        // 从扇区起点开始写
        flash_write_index = sector_start;
    }
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 3: 构造 256 字节的记录
    // ═══════════════════════════════════════════════════════════
    uint32_t addr = flash_record_addr(flash_write_index);
  
    if (len > 253) len = 253;  // 最多 253 字节数据
  
    uint8_t buf[256];
    memset(buf, 0xFF, sizeof(buf));           // 全部填充 0xFF
    buf[0] = FLASH_FLAG_VALID;                // flag = 0xA5
    buf[1] = (uint8_t)(len & 0xFF);           // 长度低字节
    buf[2] = (uint8_t)((len >> 8) & 0xFF);    // 长度高字节
    memcpy(&buf[3], data, len);               // 拷贝 JSON 数据
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 4: 写入 Flash
    // ═══════════════════════════════════════════════════════════
    W25QXX_Write(buf, addr, FLASH_RECORD_SIZE);
  
    printf("Pushed to Flash[%lu]: len=%u\r\n", flash_write_index, len);
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 5: 更新计数器
    // ═══════════════════════════════════════════════════════════
    flash_write_index = (flash_write_index + 1) % FLASH_RECORD_COUNT;
  
    if (flash_valid_count < FLASH_RECORD_COUNT)
        flash_valid_count++;
}
```

**时间线示例：**

```
初始状态：
  read_idx=0, write_idx=100, valid_count=50
  Flash[0-49] = 0xA5 (有效)
  Flash[50-99] = 0x00 (已发送/垃圾)
  Flash[100-3999] = 0xFF (空)
  Flash[4000-4095] = 0xA5 (有效)

执行 flash_store_push(json1, len1):
  1. 查找空槽：从 write_idx=100 开始，找到 empty_idx=100（0xFF）
  2. 可以写：write_idx = 100
  3. 构造记录：buf[0]=0xA5, buf[1-2]=len1, buf[3...]=json1
  4. 写入 Flash[100]
  5. write_idx = 101, valid_count = 51

后续继续 push，当 write_idx 到达 4000 时：
  Flash[4000-4095] 都是 0xA5（有效数据）
  下一个 push 找不到空槽，触发擦除

执行 flash_store_push(json_n):
  1. 查找空槽：从 write_idx=4000 开始扫，无空槽
  2. 需要擦除：sector_start = 4000, sector_end = 4015
  3. 擦除这 16 条记录
  4. read_idx=0 不在 [4000-4015]，不需要 rescan
  5. write_idx = 4000 (重新从扇区起点写)
```

---

### 4. `flash_store_peek()` - 读取待发数据

**何时调用：** `net_task()` 网络恢复后，若 `valid_count > 0`

**功能：** 取出第一条未发送的数据，供 MQTT 发送

```c
static int flash_store_peek(char *out, uint16_t max_len, 
                            uint16_t *out_len, uint32_t *out_index)
{
    if (!flash_ready) return 0;
    if (flash_valid_count == 0) return 0;
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 1: 从 read_index 开始扫描，找第一条 0xA5
    // ═══════════════════════════════════════════════════════════
    uint8_t flag = 0;
    uint32_t start = flash_read_index;
    uint32_t idx = flash_read_index;
  
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t addr = flash_record_addr(idx);
        W25QXX_Read(&flag, addr, 1);
  
        if (flag == FLASH_FLAG_VALID)
        {
            // ═══════════════════════════════════════════════════
            // 步骤 2: 读取长度和数据
            // ═══════════════════════════════════════════════════
            uint8_t len_bytes[2];
            W25QXX_Read(len_bytes, addr + 1, 2);
            uint16_t len = (uint16_t)(len_bytes[0] | (len_bytes[1] << 8));
      
            if (len >= max_len) len = max_len - 1;
      
            W25QXX_Read((uint8_t *)out, addr + 3, len);
            out[len] = '\0';
      
            // ═══════════════════════════════════════════════════
            // 步骤 3: 返回数据和记录索引
            // ═══════════════════════════════════════════════════
            *out_len = len;
            *out_index = idx;
      
            printf("Peeked Flash[%lu]: len=%u\r\n", idx, len);
      
            return 1;  // 成功
        }
  
        idx = (idx + 1) % FLASH_RECORD_COUNT;
        if (idx == start) break;  // 扫描了一整圈，没找到
    }
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 4: 找不到有效数据，自救
    // ═══════════════════════════════════════════════════════════
    printf("WARNING: No valid Flash data found, rescanning...\r\n");
    flash_store_rescan();
  
    return 0;  // 失败
}
```

**数据流：**

```
Flash 内容：
  [0]   = 0xA5, len=44,  data="{"als":1200,...}"
  [1]   = 0x00 (已发送)
  [2]   = 0xA5, len=45,  data="{"als":1201,...}"
  [3]   = 0xFF (空)
  ...

调用 flash_store_peek():
  1. read_idx = 0
  2. idx = 0, 读 flag = 0xA5 ✓ 找到
  3. 读 len = 44
  4. 读 data (44字节)
  5. 返回 out_index = 0, out_len = 44

网络任务发送给 MQTT:
  "{"als":1200,...}"  (44 字节)

发送成功后调用 flash_store_mark_sent(0):
  (见下文)
```

---

### 5. `flash_store_mark_sent()` - 标记已发送

**何时调用：** MQTT 发送成功后（在 `net_task`）

**功能：** 标记一条记录为已发送，让 `read_index` 推进

```c
static void flash_store_mark_sent(uint32_t index)
{
    if (!flash_ready) return;
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 1: 将 flag 改为 0x00 (已发送)
    // ═══════════════════════════════════════════════════════════
    uint8_t sent_flag = FLASH_FLAG_SENT;
    W25QXX_Write(&sent_flag, flash_record_addr(index), 1);
  
    printf("Marked sent: Flash[%lu]\r\n", index);
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 2: 减少计数
    // ═══════════════════════════════════════════════════════════
    if (flash_valid_count > 0)
        flash_valid_count--;
  
    // ═══════════════════════════════════════════════════════════
    // 步骤 3: 推进 read_index 到下一条有效数据
    // ═══════════════════════════════════════════════════════════
    flash_read_index = (index + 1) % FLASH_RECORD_COUNT;
}
```

**状态转换示例：**

```
before:
  Flash[0] = 0xA5 (有效)
  Flash[1] = 0xA5 (有效)
  read_index = 0, valid_count = 2

调用 flash_store_mark_sent(0):
  1. Flash[0] flag 改为 0x00
  2. valid_count = 1
  3. read_index = 1

after:
  Flash[0] = 0x00 (已发送，垃圾)
  Flash[1] = 0xA5 (有效)
  read_index = 1, valid_count = 1

下次 peek() 会跳过 [0]，直接读 [1]
```

**问题与改进：**
⚠️ 当前实现的问题：

```
如果 index = 0，执行：
  read_index = (0 + 1) % 4096 = 1

但如果位置 1 也是 0x00（垃圾），下次 peek() 开始从 1 扫描：
  Flash[1] = 0x00 ✗
  Flash[2] = 0xA5 ✓
  读取 Flash[2]

这是可以接受的，但不够"高效"。

更好的做法：mark_sent 后扫描找下一个 0xA5，直接跳过垃圾：
  flash_read_index = 找第一个 0xA5(从 index+1 开始)
```

---

## 实际应用场景

### 场景 1: 正常工作（网络良好）

```
时间线：
──────────────────────────────────────────────────────────

T0: 系统启动
   flash_store_init() → Flash 是空的
   read_idx=0, write_idx=0, valid_count=0

T1: AP3216C 传感器采样
   JSON = {"als":1200,"ir":100,"ps":50}
   发送给 net_task 队列 (同时，MQTT 已连接)
   net_task 立即发送给 MQTT (不存 Flash)

T2: MQTT 发送成功
   继续等待下一条数据

T3: 数据源源不断
   全部实时上传，Flash 保持空状态

结果：
   cached_total = 0    (没有存过 Flash)
   sent_total = 多      (全部实时发送)
   pending = 0         (Flash 空)
```

### 场景 2: 网络中断与恢复

```
时间线：
──────────────────────────────────────────────────────────

T0: MQTT 连接良好
   read_idx=0, write_idx=0, valid_count=0, cached_total=0, sent_total=0

T1: 网络中断（WiFi 断开）
   con_status = 0, g_mqtt_connected = 0
   net_task 开始进入"存储模式"

T2-T10: 数据继续产生（离线 10 秒）
   ap3216c_task 采样 → JSON
   
   数据流向：
   ├─ xAP3216CQueueForMQTT ← ap3216c 采样数据
   ├─ net_task 检测 con_status=0
   ├─ net_task 循环读队列，写 Flash
   │   flash_store_push(json, len)
   │   cached_total++
   │   pending++
   └─ 重复 9 次

   结果：
   Flash: [0xA5] [0xA5] [0xA5] ... [0xA5] [0xFF] [0xFF] ...
          0-8 (9条有效)                9
   read_idx=0, write_idx=9, valid_count=9, cached_total=9, pending=9

T11: WiFi 重新连接
   atk_mw8266d_join_ap() 成功
   获取到 IP 地址
   link_status = 1

T12: 尝试连接 MQTT
   transport_open() → 建立 TCP 连接
   MQTTSerialize_connect() → 发送 CONNECT
   收到 CONNACK ✓
   con_status = 1, g_mqtt_connected = 1
   subscribe 到 topic

T13-T20: 优先发送 Flash 数据
   while (flash_valid_count > 0)
   {
       // 读取 Flash 数据
       flash_store_peek_locked(out, ..., out_idx)
       → 读 Flash[0], len=44, data="..."

       // 通过 MQTT 发送
       MQTTSerialize_publish()
       transport_sendPacketBuffer()
       → 发送成功 ✓

       // 标记已发送
       flash_store_mark_sent(0)
       → Flash[0] flag 改为 0x00
       → pending--
       → sent_total++
       → read_idx = 1

       vTaskDelay(10ms)  // 不要太快，防止 MQTT 队列溢出
   }

   反复循环，Flash[0-8] 逐条发送
   当 pending = 0 时，切换到实时模式

T21+: 网络稳定，实时数据发送
   ap3216c 数据实时上传，Flash 空闲
```

**期间的打印日志：**

```
[T1-T10 离线期间]
Data Stored to Flash! (cached=1, pending=1)
Data Stored to Flash! (cached=2, pending=2)
...
Data Stored to Flash! (cached=9, pending=9)
cache_total=9, sent_total=0, pending=9

[T12-T20 恢复期间]
MQTT Connected!
Read from Flash: len=44, pending=9
Flash Data Sent OK (pending=8)
cache_total=9, sent_total=1, pending=8
Read from Flash: len=45, pending=8
Flash Data Sent OK (pending=7)
cache_total=9, sent_total=2, pending=7
...
[继续 7 次类似的输出]
...
cache_total=9, sent_total=9, pending=0

[T21+ 实时模式]
MQTT Pub AP3216C OK
MQTT Pub AP3216C OK
...
```

---

### 场景 3: Flash 满了（需要擦除）

```
条件：
  Flash 有 4090 条有效数据
  写入位置 write_idx = 4090
  下一个空槽还需要继续找

执行 flash_store_push(new_json):
  1. 查找空槽，从 4090 开始扫
  2. 4090: 0xA5 ✗
  3. 4091: 0xA5 ✗
  4. ...
  5. 4095: 0xA5 ✗
  6. 0: 0xA5 ✗
  7. ...（扫一圈都是 0xA5）
  8. 无空槽！需要擦除

  sector_start = (4090 / 16) * 16 = 4080
  sector_end = 4080 + 16 = 4096

  擦除 [4080-4095] 的 16 条记录
  ⚠️ 注意：这些可能是最新的有效数据！

  if (read_idx 在 [4080-4095])
      → 调用 flash_store_rescan()
      → 从头查找下一个 0xA5

  write_idx = 4080  // 从扇区起点重新开始

  写入新数据
  write_idx = 4081

结果：原来的 4090 条变成了 ~4074 条（擦除了 16 条）
```

**这里的风险：**

```
如果 read_idx=4085 (在被擦除区间内)
   1. 擦除后，[4085] 变成 0xFF
   2. read_idx = 4085 还是指向 0xFF（空！）
   3. 下次 peek() 会跳过 4085，找下一个 0xA5

   如果从 4086 到 4100 都是 0xFF（空）
   那 peek() 扫一圈找不到，会调用 rescan()

   rescan() 找下一个 0xA5，假设是位置 500
   更新 read_idx = 500

问题：
   原本还有 [4090, 4091, ...] 的有效数据
   被当作"新的 0xA5"处理了，可能会重复读
```

**解决方案：** 改进 `flash_store_push()` 的擦除逻辑

```c
// 不要擦除当前 write_idx 所在的扇区
// 而要擦除最老的、已发送完的扇区
uint32_t oldest_sector = (flash_read_index / FLASH_RECORDS_PER_SECTOR) 
                       * FLASH_RECORDS_PER_SECTOR;
if (oldest_sector != current_sector)
    flash_store_erase_sector(oldest_sector);
```

---

## 常见问题与 Bug

### Q1: 为什么 pending 和 cached-sent 不相等？

**原因及诊断：**


| 原因            | 日志表现                                 | 修复                                             |
| --------------- | ---------------------------------------- | ------------------------------------------------ |
| 多处写入 (双写) | cached 增长慢，pending 增长快            | 确认只有`net_task` 写 Flash                      |
| peek 找不到数据 | valid_count > 0 但 "no valid found"      | 检查 read_index 是否指向垃圾                     |
| 指针跳跃        | pending 忽大忽小                         | 检查 mark_sent 后的 read_index 推进逻辑          |
| 扇区擦除 bug    | pending 突然减少很多                     | 检查是否不小心擦了 read_index 区间               |
| 启动计数差异    | 重启后 cached/sent 归零但 pending 还有值 | 正常现象（pending 来自 Flash，本地计数重启归零） |

**诊断命令（加入日志）：**

```c
// 在 net_task 的 report 中加入
printf("cache_total=%lu, sent_total=%lu, pending=%lu, read_idx=%lu, write_idx=%lu\r\n",
       cached_total, sent_total, flash_valid_count, flash_read_index, flash_write_index);

// 查看指针关系
// 若 read_idx > write_idx，说明已经回绕
// 若 pending != (write_idx - read_idx) % COUNT，说明有垃圾或计数错误
```

---

### Q2: peek() 找不到数据，但 valid_count > 0

**症状：**

```
cache_total=5, sent_total=0, pending=5
WARNING: Flash data not found! valid_count=5, read_idx=100, write_idx=50
```

**常见原因：**

1. **read_index 指向垃圾**

   ```
   Flash[100] = 0x00 (已发送)
   Flash[101] = 0x00 (已发送)
   ...
   Flash[120] = 0xA5 (有效，但 peek 从 100 开始扫，绕了一圈没找到)

   root cause: mark_sent 没有推进 read_index 到下一个 0xA5
   ```
2. **扇区擦除导致 read_index 失效**

   ```
   擦除 [100-115]
   read_index = 100
   Flash[100] 现在是 0xFF (被擦除)

   peek 扫一圈找不到 0xA5
   ```
3. **计数错误导致 valid_count 虚高**

   ```
   本应 mark_sent，但没有执行
   或执行 push 时 valid_count++ 了多次
   ```

**修复步骤：**

```c
// 方案 A: 在 mark_sent 中加入"找下一个 0xA5"逻辑
static void flash_store_mark_sent(uint32_t index)
{
    if (!flash_ready) return;
    uint8_t sent_flag = FLASH_FLAG_SENT;
    W25QXX_Write(&sent_flag, flash_record_addr(index), 1);
    if (flash_valid_count > 0) flash_valid_count--;
  
    // 改进：扫描从 index+1 开始的下一个 0xA5
    for (uint32_t i = 1; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t next_idx = (index + i) % FLASH_RECORD_COUNT;
        uint8_t flag;
        W25QXX_Read(&flag, flash_record_addr(next_idx), 1);
        if (flag == FLASH_FLAG_VALID)
        {
            flash_read_index = next_idx;
            return;
        }
    }
    // 没找到有效数据，说明全部发送完了
    flash_read_index = (index + 1) % FLASH_RECORD_COUNT;
}

// 方案 B: 在 peek 中加强防护
static int flash_store_peek(...)
{
    // ... 扫描逻辑 ...
  
    if (no valid found)
    {
        printf("WARN: valid_count=%lu but no 0xA5 found\r\n", flash_valid_count);
        printf("  read_idx=%lu, write_idx=%lu\r\n", flash_read_index, flash_write_index);
      
        // 打印周围的记录标志
        for (uint32_t i = 0; i < 10; i++)
        {
            uint8_t f;
            W25QXX_Read(&f, flash_record_addr((flash_read_index + i) % FLASH_RECORD_COUNT), 1);
            printf("  [%lu]=0x%02X\r\n", (flash_read_index + i) % FLASH_RECORD_COUNT, f);
        }
      
        flash_store_rescan();  // 自救
        return 0;
    }
}
```

---

### Q3: 为什么某些数据被重复发送？

**原因：**

1. **mark_sent 没有正确调用**

   ```
   push → peek → publish ✓
   mark_sent ✗ (漏掉了)

   下次 peek 还会读到同一条
   ```
2. **并发竞态**

   ```
   线程 A: peek(idx=10) → 读取 Flash[10]
   线程 B: mark_sent(10) → 改 Flash[10] 为 0x00
   线程 A: publish 失败，但没有 mark_sent

   下次 peek 还是读 idx=10
   ```
3. **发送失败没有重试**

   ```
   read → publish fail → 没有 mark_sent
   下次 peek 重复读

   这可能是"特性"而非 bug（失败的不标记）
   ```

**检查点：**

```c
// 在 net_task 的发送逻辑中
if (flash_store_peek_locked(...))
{
    printf("Read from Flash: len=%d, pending=%lu\r\n", cached_len, flash_valid_count);
  
    // 发送
    len = MQTTSerialize_publish(...);
    if (transport_sendPacketBuffer(...) == len)
    {
        printf("Flash Data Sent OK\r\n");
        // ✅ 务必调用 mark_sent
        flash_store_mark_sent_locked(rec_index);
        sent_total++;
    }
    else
    {
        printf("Send Flash Data Failed!\r\n");
        // ❌ 发送失败，不标记，下次重试
    }
}
```

---

### Q4: 启动时为什么有大量数据要发送？

**原因：** Flash 中仍有上次未发送完的数据

**处理方式：**

```c
// 在 flash_store_init() 之后立即检查
void init_task(void *pv)
{
    // ...
    flash_store_init();
  
    if (flash_valid_count > 0)
    {
        printf("Device restarted with %lu pending records in Flash\r\n", 
               flash_valid_count);
        printf("These will be uploaded when MQTT connects.\r\n");
    }
  
    // ...
}
```

**日志输出：**

```
Device restarted with 42 pending records in Flash
These will be uploaded when MQTT connects.

[等待网络恢复...]

MQTT Connected!
Read from Flash: len=44, pending=42
Flash Data Sent OK (pending=41)
... (重复 41 次)
```

---

## 总结与建议

### 关键指标与健康检查

```c
// 定期打印这些指标
printf("═══════════════════════════════════════════\r\n");
printf("Flash Status:\r\n");
printf("  cached_total   = %lu  (本次运行写入 Flash 的条数)\r\n", cached_total);
printf("  sent_total     = %lu  (本次运行发送并标记的条数)\r\n", sent_total);
printf("  pending        = %lu  (当前 Flash 中未发送的条数)\r\n", flash_valid_count);
printf("  read_index     = %lu  (下一条待读位置)\r\n", flash_read_index);
printf("  write_index    = %lu  (下一条待写位置)\r\n", flash_write_index);
printf("  expect: pending ≈ cached_total - sent_total\r\n");
printf("═══════════════════════════════════════════\r\n");
```

**健康状态判断：**


| 情况                                    | 表现 | 结论                       |
| --------------------------------------- | ---- | -------------------------- |
| cached=10, sent=10, pending=0           | ✅   | 所有数据已发送             |
| cached=15, sent=10, pending=5           | ✅   | 5 条待发送，符合预期       |
| cached=10, sent=0, pending=10, 网络未连 | ✅   | 正常存储，等待恢复         |
| cached=10, sent=0, pending=20           | ⚠️ | 有重复写或重复计数         |
| cached=10, sent=5, pending=10           | ❌   | pending 与计数不符，有 bug |
| pending=0 但周期性下降到负              | ❌   | 指针管理有问题             |

---

### 代码改进建议

**优先级 1：必须做**

- [ ]  确保只有 `net_task` 可以调用 `flash_store_push()`（删除 `ap3216c_task` 中的直接写）
- [ ]  为 `mark_sent()` 加入"扫描下一个 0xA5"逻辑，避免跳过记录
- [ ]  在 `peek()` 找不到数据时打印周围记录的 flag，便于诊断

**优先级 2：应该做**

- [ ]  增强 Flash 初始化检查（已有，但可以更严格）
- [ ]  在 `push()` 的擦除逻辑中加上防护（不擦除 read_index 区间）
- [ ]  定期打印 Flash 健康指标（每 5 秒）

**优先级 3：锦上添花**

- [ ]  实现"智能擦除"（优先擦除最老的已发送扇区）
- [ ]  添加 Flash 自检命令（通过串口或按键触发 rescan）
- [ ]  记录 Flash 统计数据（总擦除次数、总写入条数等）

---

**这就是 SPI Flash 断点续传的完整逻辑！**

理解了这些，你可以：

1. ✅ 在简历上自信地写"独立设计并实现 SPI Flash 循环缓冲机制"
2. ✅ 面试时详细解释"为什么这样设计"和"遇到过什么 bug"
3. ✅ 继续改进代码、优化逻辑

加油！
