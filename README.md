# STM32 FreeRTOS 智能物联网网关 (STM32-IoT-Gateway)

![RTOS](file:///E:/Tdoc/%E5%B5%8C%E8%BD%AFSTM32%E8%A3%B8%E6%9C%BA.assets/RTOS-FreeRTOS_V9.0.0-blue.svg?lastModify=1774671062) ![MCU](file:///E:/Tdoc/%E5%B5%8C%E8%BD%AFSTM32%E8%A3%B8%E6%9C%BA.assets/MCU-STM32F429IGT6-red.svg?lastModify=1774671062) ![Protocol](file:///E:/Tdoc/%E5%B5%8C%E8%BD%AFSTM32%E8%A3%B8%E6%9C%BA.assets/Protocol-MQTT_V3.1.1-green.svg?lastModify=1774671062) ![License](file:///E:/Tdoc/%E5%B5%8C%E8%BD%AFSTM32%E8%A3%B8%E6%9C%BA.assets/License-MIT-brightgreen.svg?lastModify=1774671062)

本项目是一个基于 STM32F429的物联网网关。系统采用 **FreeRTOS** 实时操作系统统筹调度，通过 ESP8266 (ATK-MW8266D) 透传模式接入 OneNET 平台，实现传感器数据的云端上报与云端指令的设备控制。

项目解决了网络环境下的断网数据丢失、**死机无响应**、**远程固件更新难**等问题。

## 核心技术

### 1. SPI Flash 的可靠“断点续传”机制

**针对弱网环境导致的数据丢失问题，设计并实现了基于 SPI Flash (W25QXX) 的离线数据环形缓冲区（Ring Buffer）：**

* **无损覆盖与磨损均衡**：自定义扇区状态机 (`0xFF` 空闲 -> `0xA5` 待发 -> `0x00` 作废)。发送成功后无需立刻擦除整个扇区，只需将标志位标为 `0x00`，极大延长了 Flash 寿命。
* **线程安全机制**：封装了并发安全的 `push_locked` 和 `peek_locked` 接口，底层使用 FreeRTOS `Mutex` 互斥锁，确保『采集任务』和『网络任务』同时操作 Flash 时的数据一致性。
* **开机自检与游标恢复**：网关重启或断电复位后，初始化程序会全盘扫面识别 `0xA5` 标记，自动恢复读写游标，确保断电不丢历史数据。

### 2. 软硬结合“位图看门狗”

**设计了高强度的多任务监控看门狗：**

* **任务打卡机制**：采用全局并发安全的 32 位 Bitmap (`wd_expected_mask` / `wd_heartbeat_mask`)。
* **全生命周期监控**：网络任务 (`NET`)、传感器任务 (`AP`)、显示任务 (`LCD`) 等必须在规定时间内各自调用 `wd_heartbeat()` 独立打卡。
* **精准死机定位**：看门狗任务定时校验，如有任何一个核心任务发生死锁（如网络层 API 阻塞不释放），看门狗将停止触发硬件 IWDG 重启系统，并在临终前通过串口打印“遗言”指出具体死锁的任务（`xxx task dead`）。

### 3. MQTT 网络状态机与通信优化

* **指数退避重连算法**：网络断开时，重连等待时间设定为 1s -> 2s -> 4s -> ... -> 32s，防止网络突发瘫痪时设备高频重连引发“雪崩效应”并降低功耗。
* **动态心跳保活 (Dynamic Keep-Alive)**：重写了 MQTT 定时机制，只有在链路空闲超出阈值时才发送 `PINGREQ`，如有业务数据收发则自动顺延，节省平台流量。
* **双缓冲与非阻塞解析**：采用 AT 指令透传模式，底层通过 `xUartRxQueue` 异步通知网络任务，彻底解决 TCP 粘包问题与解析阻塞问题。

### 4. 双分区 OTA 远程平滑升级

**实现了完整可靠的云端固件升级体系：**

* **结构设计**：划分 Bootloader、Meta\_Flag (状态元数据区)、App1 (主运行区)、App2 (OTA 下载缓存区)。
* **HMAC-SHA1 加密与安全校验机制**：支持云端下发任务 ID 与 MD5。过 HTTP Ranged Requests (分块下载)，在后台静默下载固件切片后，累加计算 MD5。只有下载完整且 MD5 效验通过后，才修改 Meta 标志位并重启交由 Bootloader 搬运，杜绝升级变砖风险。
* **JSON 动态解析**：结合 `cJSON` 库，处理来自平台的物模型指令，支持下发周期修改、下发控制 LED/蜂鸣器以及触发 OTA 升级等操作。

---

## 目录结构 (Directory Structure)

├── APP/
│   ├── NET_APP/        # 网络核心功能：重连机制、数据组包、MQTT 协议收发
│   ├── Sensor_APP/     # 传感器应用：多传感器采样轮询任务
│   ├── SPI_FLASH_APP/  # 存储应用：Flash 上电恢复、断点续传队列逻辑
│   └── WDOG_APP/       # 看门狗应用：基于位图的多任务健康监控机制
├── ATK_MW8266D/        # 网卡/Wi-Fi模块底层 AT 指令驱动与透传管理
├── OTA/                # OTA 下载流程控制与校验模块 (MD5, Meta配置)
├── PROTOCOL/           # 与云平台对接的物模型解析层 (cJSON 解析)
├── HARDWARE/           # 底层外设驱动 (AP3216C, W25QXX, LCD, SDRAM, PCF8574等)
├── FreeRTOS/           # FreeRTOS V9.0 实时系统源码及配置
├── SYSTEM/             # STM32 时钟、延时、串口打印等系统底层环境
└── USER/               # Main 函数、中断向量配置及工程入口

## 硬件与环境依赖 (Hardware & Software)

* **主控芯片**: 正点原子STM32F429IGT6 (180MHz, 1MB Flash, 256KB RAM)
* **外部存储**: W25Q256 (32MB SPI Flash), 外部 SDRAM
* **网络模块**: ATK-MW8266D (基于 ESP8266，串口通信)
* **核心外设**: AP3216C (环境光/接近/红外), DHT11(未使用), 4.3寸 TFT LCD
* **开发环境**: Keil MDK v5.x / ARM Compiler 5
* **第三方组件**: MQTT Packet 库, cJSON

## 快速开始 (Getting Started)

**1.环境配置: 修改 network\_app.h 中的 Wi-Fi 账号和 MQTT 云端配置：**

```
 #define DEMO_WIFI_SSID          "Your_SSID"
 #define DEMO_WIFI_PWD           "Your_Password"
 #define MQTT_BROKER_IP          "studio-mqtt.heclouds.com"
 #define MQTT_CLIENT_ID          "Your_Device_Name"
 #define MQTT_USER_NAME          "Your_Product_ID"
 #define MQTT_PASSWORD           "Your_Token_Sign"
```

**连接其他MQTT服务器也可以**

**2.下载与烧录**

**需要注意配合 Bootloader 使用。如果是单工程测试调试，请修改 main.c 中中断向量表偏移 SCB->VTOR 到 0x08000000 并更改 Keil Target 的 IROM1 起始地址。如果是配合 OTA 测试，默认 App 偏移量为 Application\_1\_Addr (0x08020000)。**

**OneNet平台给出的OTA升级流程以鉴权密钥以及注意事项见升级流程.md**
