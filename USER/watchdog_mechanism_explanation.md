# 看门狗（Watchdog）机制详解

## 🐕 什么是看门狗？

**看门狗是一个硬件定时器，用来检测系统是否卡死或死循环。**

### 工作原理

```
正常情况：
┌──────────────┐
│ 任务定期"喂狗" │ (每 500ms 喂一次)
└──────────────┘
        ↓
    ┌────────┐
    │看门狗  │ (每 500ms 重置一次计数器)
    │计数器  │
    │= 0    │
    └────────┘

系统卡死（没人喂狗）：
┌─────────────────────┐
│ 任务卡在某个地方     │ (无法执行喂狗代码)
└─────────────────────┘
        ↓
    ┌────────────┐
    │看门狗计数器 │ (继续计数)
    │0 → 1 → 2.. │
    │→ 计数满 2000 │
    └────────────┘
        ↓
    🔴 自动重启 MCU
```

---

## 📋 你项目中的看门狗构成

### 1. **硬件部分（IWDG - 独立看门狗）**

```c
static IWDG_HandleTypeDef hiwdg;

hiwdg.Init.Prescaler = IWDG_PRESCALER_64;  // 分频 64
hiwdg.Init.Reload = 2000;                 // 重装值 2000

// 时间计算：
// LSI 时钟 = 32kHz（STM32 内部低速时钟）
// 预分频：32000 / 64 = 500Hz（每 2ms 计数一次）
// 计数满：2000 × 2ms = 4000ms = 4 秒
// → 意思是：如果 4 秒内没有喂狗，MCU 自动重启
```

### 2. **软件部分（看门狗任务）**

```c
void watchdog_task(void *pv)
{
    // 延迟 5 秒启动（给其他任务初始化时间）
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // 设置期望的任务位图（这些任务必须定期喂狗）
    wd_set_expected(WD_BIT_NET | WD_BIT_AP | WD_BIT_LCD | WD_BIT_STAT | WD_BIT_DHT);
    
    // 初始化硬件看门狗
    HAL_IWDG_Init(&hiwdg);
    HAL_IWDG_Start(&hiwdg);
    wd_started = 1;
    
    // 监控循环
    while (1)
    {
        // 检查是否所有任务都喂过狗
        if ((mask & wd_expected_mask) == wd_expected_mask)  // ← 所有位都被设置
        {
            HAL_IWDG_Refresh(&hiwdg);  // 重置硬件看门狗
            wd_heartbeat_mask = 0;      // 清空喂狗标记
            miss = 0;
        }
        else  // ← 有任务没有喂狗
        {
            miss++;
            if (miss >= 6)  // 连续 6 次 (3 秒) 检测失败
            {
                while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }  // 死循环，让硬件看门狗自动重启
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));  // 每 500ms 检查一次
    }
}
```

---

## 🎯 核心概念：位图 + 喂狗

### 定义的位

```c
#define WD_BIT_NET   (1U << 0)    // 第 0 位：网络任务
#define WD_BIT_AP    (1U << 1)    // 第 1 位：WiFi 任务
#define WD_BIT_LCD   (1U << 2)    // 第 2 位：LCD 任务
#define WD_BIT_STAT  (1U << 3)    // 第 3 位：统计任务
#define WD_BIT_DHT   (1U << 4)    // 第 4 位：传感器任务

// 期望位图：0001 1111 = 0x1F (5 个任务都要喂狗)
wd_expected_mask = WD_BIT_NET | WD_BIT_AP | WD_BIT_LCD | WD_BIT_STAT | WD_BIT_DHT;
```

### 喂狗函数

```c
static inline void wd_heartbeat(uint32_t mask)
{
    taskENTER_CRITICAL();      // 进入临界区（关中断）
    wd_heartbeat_mask |= mask; // 设置对应的位
    taskEXIT_CRITICAL();       // 退出临界区（开中断）
}

// 使用方式（在各个任务中）：
// wd_heartbeat(WD_BIT_NET);   // net_task 喂狗
// wd_heartbeat(WD_BIT_AP);    // ap3216c_task 喂狗
```

---

## 🔄 完整工作流程

```
时刻 0ms：
┌──────────────────────────────┐
│ watchdog_task 启动            │
│ 设置 wd_expected_mask = 0x1F  │
│ 启动硬件看门狗                 │
└──────────────────────────────┘

时刻 500ms：
┌──────────────────────────────┐
│ 各任务执行并喂狗               │
├──────────────────────────────┤
│ net_task:   wd_heartbeat(0x01)→ wd_heartbeat_mask |= 0x01│
│ ap3216c:    wd_heartbeat(0x02)→ wd_heartbeat_mask |= 0x02│
│ lcd_task:   wd_heartbeat(0x04)→ wd_heartbeat_mask |= 0x04│
│ run_stats:  wd_heartbeat(0x08)→ wd_heartbeat_mask |= 0x08│
│ dht11_task: wd_heartbeat(0x10)→ wd_heartbeat_mask |= 0x10│
└──────────────────────────────┘
         ↓
┌──────────────────────────────┐
│ watchdog_task 检查            │
├──────────────────────────────┤
│ 期望：0x1F (0001 1111)        │
│ 实际：0x1F (0001 1111)        │
│ 相等 ✓                        │
│ → HAL_IWDG_Refresh()          │
│ → wd_heartbeat_mask = 0       │
│ → miss = 0                    │
└──────────────────────────────┘

时刻 1000ms：
(重复上面的流程)

如果某个任务卡死（比如 net_task）：
时刻 500ms：
┌──────────────────────────────┐
│ net_task: 卡在某个地方        │ ❌ 无法执行 wd_heartbeat
│ ap_task:  wd_heartbeat(0x02)  │ ✓
│ lcd_task: wd_heartbeat(0x04)  │ ✓
│ run_stats:wd_heartbeat(0x08)  │ ✓
│ dht11:    wd_heartbeat(0x10)  │ ✓
└──────────────────────────────┘
         ↓
┌──────────────────────────────┐
│ watchdog_task 检查            │
├──────────────────────────────┤
│ 期望：0x1F (0001 1111)        │
│ 实际：0x1E (0001 1110)        │❌ 缺少 0x01
│ 不相等！miss++                │
│ miss = 1                      │
└──────────────────────────────┘

(继续 5 个 500ms 周期)
miss = 2, 3, 4, 5, 6...

当 miss >= 6（即 3 秒后）：
┌──────────────────────────────┐
│ while(1) { delay(1000); }     │
│ 进入死循环，让硬件看门狗继续计时│
│ 硬件看门狗在 4 秒时触发       │
│ → MCU 自动重启！              │
└──────────────────────────────┘
```

---

## 📊 时间轴图

```
启动                 正常运行              检测到故障     重启
  ↓                    ↓                     ↓           ↓
[0s] init_5s  [5s] 喂狗循环  [8s] miss++   [8s+3s=11s] [11s+1s=12s]
              │               │               │
         每 500ms          连续 miss ≥ 6    硬件重启
         检查一次          (3 秒超时)
```

---

## 🛠️ 为什么你注释掉了？

你注释掉了 `watchdog_task` 因为：

1. **开发调试时** - 看门狗会在程序卡住时自动重启，不利于调试
2. **没有实现所有任务的喂狗** - 只有 5 个任务定义了位，但你可能加入新任务
3. **不是必需功能** - 可靠性要求不高的项目可以不用

---

## ✅ 如果要启用看门狗

需要在**每个任务**中添加喂狗代码：

```c
void dht11_task(void *pv)
{
    while (1)
    {
        wd_heartbeat(WD_BIT_DHT);  // ← 加这行
        
        // 任务逻辑...
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void ap3216c_task(void *pv)
{
    while (1)
    {
        wd_heartbeat(WD_BIT_AP);   // ← 加这行
        
        // 任务逻辑...
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void lcd_task(void *pv)
{
    while (1)
    {
        wd_heartbeat(WD_BIT_LCD);  // ← 加这行
        
        // 任务逻辑...
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void net_task(void *pv)
{
    while (1)
    {
        wd_heartbeat(WD_BIT_NET);  // ← 加这行
        
        // 任务逻辑...
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void run_time_stats_task(void *pv)
{
    while (1)
    {
        wd_heartbeat(WD_BIT_STAT); // ← 加这行
        
        // 任务逻辑...
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

然后在 `start_task` 中启用看门狗任务：

```c
xTaskCreate(watchdog_task, "watchdog", WATCHDOG_STK_SIZE, NULL, WATCHDOG_TASK_PRIO, &WatchdogTask_Handler);
```

---

## 🎯 简单总结

| 概念 | 含义 |
|------|------|
| **硬件看门狗** | 4 秒内没有喂狗就自动重启 MCU |
| **软件看门狗** | 检查所有任务是否都定期运行 |
| **喂狗** | 任务定期调用 `wd_heartbeat()` 表示"我还活着" |
| **位图** | 用 5 个 bit 分别表示 5 个任务的状态 |
| **检查** | 看门狗任务每 500ms 检查一次，如果有任务连续 3 秒没喂狗，就让 MCU 重启 |

---

## 💡 面试怎么说

> **"我们的项目有一个看门狗机制来监控系统健康状态。
>
> 工作原理是：
> 1. 硬件看门狗设置 4 秒超时
> 2. 5 个关键任务每执行一遍都会调用 `wd_heartbeat()` 来"喂狗"
> 3. 看门狗任务每 500ms 检查一次，确保所有任务都喂过狗
> 4. 如果某个任务卡住了，无法喂狗，连续 3 秒检测失败后，硬件看门狗会自动重启 MCU
>
> 这样可以防止系统无限卡死，提高可靠性。"**

