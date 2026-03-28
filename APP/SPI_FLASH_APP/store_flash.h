#ifndef STORE_FLASH_H
#define STORE_FLASH_H
#include <stdint.h>
#include "FreeRTOS.h"
#include "semphr.h"

/**
 * @brief Flash 中单条断网存储记录的数据结构
 *        总大小严格限制为 256 字节，方便 Flash 的页对齐写入（Page Program）
 */
typedef struct
{
    uint8_t flag;      /**< 记录标志位：0xFF为空，0xA5为有效数据，0x00为已发送作废数据 */
    uint16_t len;      /**< 实际有效负载的数据长度 */
    char data[253];    /**< 具体的负载数据内容，总结构体大小 1+2+253 = 256 字节 */
} FlashRecord;

// ===== 存储空间分配与结构宏定义 =====
#define FLASH_DATA_START      0x00100000   // 离线数据存储池在 Flash 中的物理首地址 (1MB 定位)
#define FLASH_DATA_SIZE       (1024 * 1024)// 分配给离线存储池的总容量 (此处为 1MB 空间)
#define FLASH_SECTOR_SIZE     4096         // W25Q 系列 Flash 的最小擦除单元：扇区大小 (4KB)
#define FLASH_RECORD_SIZE     256          // 一条记录占用的字节数：256B (即 1 页/Page 的大小)
#define FLASH_RECORD_COUNT    (FLASH_DATA_SIZE / FLASH_RECORD_SIZE) // 存储池总共能容纳的记录总条数 (4096 条)
#define FLASH_RECORDS_PER_SECTOR (FLASH_SECTOR_SIZE / FLASH_RECORD_SIZE) // 每个 4KB 扇区能存多少条记录 (16 条)

// ===== 数据头部状态标志位定义 =====
#define FLASH_FLAG_EMPTY      0xFF         // 擦除后默认全 1 (1111 1111) => 表示可用空位
#define FLASH_FLAG_VALID      0xA5         // 我们自定义的一个魔数 (1010 0101) => 表示有未上传的有效数据
#define FLASH_FLAG_SENT       0x00         // 数据上传成功后，把有效标志修改为全 0 (无需擦除，只能将 1 写为 0)

// ===== 全局变量及句柄 =====
extern SemaphoreHandle_t xFlashMutex; // 并发写保护：多任务操作 Flash 时的互斥锁

// 环形队列游标指针：为了方便查找，抽象出 0 到 FLASH_RECORD_COUNT-1 的逻辑索引
extern uint32_t flash_read_index;     // 读游标：指向下一个待读取并发送的有效数据的逻辑索引
extern uint32_t flash_write_index;    // 写游标：指向下一个空的位置，用于存入新断网数据的逻辑索引
extern uint32_t flash_valid_count;    // 计数器：当前环形队列里还有多少条"有效且未发送"的数据
extern uint8_t flash_ready;           // 标志位：SPI Flash 芯片是否存在并正常初始化 (1=正常可用)

// ===== 提供给外部上层模块调用的 API 接口 =====

/**
 * @brief  初始化离线数据存储管理机制，通常需要扫描一遍 Flash 恢复读写游标和统计当前有效报文量
 */
void flash_store_init(void);

/**
 * @brief  向 Flash 中压入一条新的离线数据 (需要获取锁后调用)
 * @param  data: 待存数据的指针 (通常是 cJSON 生成的字符串格式)
 * @param  len: 待存数据的字节长度 (不能超过上面定义的 253）
 */
void flash_store_push_locked(const char *data, uint16_t len);

/**
 * @brief  从 Flash 中取出一条符合条件的有效数据查看其内容 (带锁环境调用)
 * @param  out: 数据拷贝输出的目标缓存区
 * @param  max_len: 目标缓存区的容量大小，防止溢出
 * @param  out_len: 返回读出的实际有效载荷长度
 * @param  out_index: 告诉你这条数据在什么位置，方便后续使用位置坐标删数据
 * @return 0 成功，-1 没搜到或者失败
 */
int flash_store_peek_locked(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index);

/**
 * @brief  把指定逻辑索引位的那条数据标记作废（设为 SENT=0x00）(带锁环境调用)
 * @param  index: 需要清理作废的记录的逻辑下标
 */
void flash_store_mark_sent_locked(uint32_t index);

#endif

