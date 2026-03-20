# SPI Flash 上电流程梳理计划

## Summary
- 目标：梳理“上电后 SPI Flash 相关流程”的真实调用链，明确每个函数做了什么，并给出复杂度的客观评价。
- 输出：一份可直接用于学习/面试复述的分层说明（启动链路、离线缓存链路、补发链路、复杂度评估）。

## Current State Analysis
- 已确认 SPI Flash 驱动入口为 `w25qxx`：
  - `W25QXX_Init` / `W25QXX_ReadID` / `W25QXX_Read` / `W25QXX_Write` / `W25QXX_Erase_Sector`
  - 位置：`HARDWARE/W25QXX/w25qxx.c`
- 已确认业务层上电入口与缓存模块：
  - `start_task` 创建任务
  - `init_task` 中调用 `W25QXX_Init`、`W25QXX_ReadID`、`flash_store_init`
  - 缓存管理函数：`flash_store_rescan/init/push/peek/mark_sent`
  - 位置：`USER/main.c`
- 已确认运行时断网缓存与重连补发逻辑入口在 `net_task` 及其拆分函数中。

## Proposed Changes
- 本次不改动业务代码，仅交付“流程梳理说明”：
  1. 梳理上电初始化阶段的调用顺序（从 `main` 到 `init_task` 到 `flash_store_init`）。
  2. 梳理离线写入阶段的函数链（采集任务/网络任务触发 -> `flash_store_push_locked` -> `flash_store_push` -> `W25QXX_Write`）。
  3. 梳理重连补发阶段的函数链（`flash_store_peek_locked` -> 发布 -> `flash_store_mark_sent_locked`）。
  4. 解释关键状态位语义（`FF/A5/00`）与索引语义（`read/write/valid_count`）。
  5. 进行复杂度评估（代码复杂度、状态复杂度、并发复杂度、故障复杂度）并给出客观分级。

## Assumptions & Decisions
- 假设你当前关注“理解与复述能力”，不是本轮继续改代码。
- 复杂度评价口径采用“嵌入式项目维护难度”而非算法大 O。
- 评估结论会区分：
  - 学习门槛（新人上手）
  - 面试可讲深度
  - 线上维护风险点

## Verification Steps
1. 对照 `USER/main.c` 的函数顺序，确认每一段流程都有对应函数和触发条件。
2. 对照 `HARDWARE/W25QXX/w25qxx.c`，确认底层读写擦接口与业务层调用一致。
3. 输出最终说明时附上关键代码引用（文件+行段），确保结论可追溯。
