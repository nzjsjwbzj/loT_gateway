#ifndef STORE_FLASH_H
#define STORE_FLASH_H
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

// ARMCC 默认不支持匿名 union（C99 模式），需要此 pragma 才能编译 FlashRequest
#pragma anon_unions

/**
 * @brief Flash 中单条断网存储记录的数据结构
 *        总大小严格限制为 256 字节，方便 Flash 的页对齐写入（Page Program）
 */
typedef struct
{
    uint8_t  flag;      /**< 记录标志位：0xFF为空，0xA5为有效数据，0x00为已发送作废数据 */
    uint32_t seq;       /**< 全局递增序列号，用于断电恢复时确定数据写入的先后顺序 */
    uint16_t len;       /**< 实际有效负载的数据长度 */
    char     data[249]; /**< 具体的负载数据内容，总结构体大小 1+4+2+249 = 256 字节 */
} FlashRecord;

// ===== 存储空间分配与结构宏定义 =====
#define FLASH_DATA_START      0x00200000   // 离线数据存储池在 Flash 中的物理首地址 (1MB 定位)
#define FLASH_DATA_SIZE       (1024 * 1024)// 分配给离线存储池的总容量 (此处为 1MB 空间)
#define FLASH_SECTOR_SIZE     4096         // W25Q 系列 Flash 的最小擦除单元：扇区大小 (4KB)
#define FLASH_RECORD_SIZE     256          // 一条记录占用的字节数：256B (即 1 页/Page 的大小)
#define FLASH_RECORD_COUNT    (FLASH_DATA_SIZE / FLASH_RECORD_SIZE) // 存储池总共能容纳的记录总条数 (4096 条)
#define FLASH_RECORDS_PER_SECTOR (FLASH_SECTOR_SIZE / FLASH_RECORD_SIZE) // 每个 4KB 扇区能存多少条记录 (16 条)

// ===== 数据头部状态标志位定义 =====
#define FLASH_FLAG_EMPTY      0xFF         // 擦除后默认全 1 (1111 1111) => 表示可用空位
#define FLASH_FLAG_VALID      0xA6        // 自定义魔数（不要A5，和RTOS的重叠了，后面不好查BUG） => 表示有未上传的有效数据
#define FLASH_FLAG_SENT       0x00         // 数据上传成功后，把有效标志修改为全 0 (无需擦除，只能将 1 写为 0)

// ===== 消息队列：Flash 写任务的消息类型 =====
#define FLASH_REQ_DATA_MAX    249          // push 消息中数据字段的最大长度
#define FLASH_REQ_QUEUE_LEN   10           // 请求队列深度

typedef enum {
    FLASH_REQ_PUSH = 0,      // 存一条离线数据
    FLASH_REQ_PEEK,           // 读取最旧的一条有效数据（需同步应答）
    FLASH_REQ_MARK_SENT,      // 将指定索引的数据标记为已发送
    FLASH_REQ_FORMAT,         // 擦除整个离线数据区（格式化）
} FlashReqType;

typedef struct {
    FlashReqType type;
    union {
        struct {
            char     data[FLASH_REQ_DATA_MAX];
            uint16_t len;
        } push;
        struct {
            char     *out;        // 调用者提供的读缓冲区
            uint16_t  max_len;
            uint16_t *out_len;    // 输出实际数据长度
            uint32_t *out_index;  // 输出数据所在索引
            int       *ret;       // 输出返回值 (1=成功 0=失败)
        } peek;
        struct {
            uint32_t index;       // 要标记为 SENT 的逻辑索引
        } mark_sent;
    };
} FlashRequest;

// ===== 全局变量及句柄 =====
extern QueueHandle_t xFlashReqQueue;   // Flash 写任务的请求队列

// 环形队列游标指针（只读，供外部模块参考统计信息）
extern uint32_t flash_read_index;      // 读游标
extern uint32_t flash_write_index;     // 写游标
extern uint32_t flash_valid_count;     // 有效未发送数据计数
extern uint32_t g_flash_seq;           // 全局递增序列号
extern uint8_t  flash_ready;           // SPI Flash 芯片是否可用 (1=正常)

// ===== 对外 API：异步接口（通过队列委托给 flash_writer_task） =====

/**
 * @brief  异步存入一条离线数据，立即返回
 * @param  data: 数据指针
 * @param  len:  数据长度（≤249）
 * @retval 1=已入队  0=队列满，建议调用者稍后重试
 */
int flash_store_push_async(const char *data, uint16_t len);

/**
 * @brief  读取最旧的一条有效数据（同步，阻塞等待 flash_writer_task 应答）
 * @param  out:      输出数据缓冲区
 * @param  max_len:  缓冲区容量
 * @param  out_len:  输出实际数据长度
 * @param  out_index:输出数据索引（供 mark_sent 用）
 * @retval 1=成功  0=失败（无数据或错误）
 */
int flash_store_peek_async(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index);

/**
 * @brief  异步标记一条数据为已发送，立即返回
 * @param  index: 要标记的数据逻辑索引
 */
void flash_store_mark_sent_async(uint32_t index);

/**
 * @brief  擦除整个离线数据区（格式化），读/写指针与计数全部清零
 * @note   会阻塞较长时间（擦除 256 个扇区），调用方不要在高优先级/中断里用
 */
void flash_store_format_all(void);

/**
 * @brief  异步请求擦除整个离线数据区（委托给 flash_writer_task 串行执行）
 * @note   通过队列排队，不会与正在进行的写入冲突；立即返回
 */
void flash_store_format_async(void);

/**
 * @brief  Flash 写任务入口（FreeRTOS 任务函数）
 *         唯一直接操作 SPI Flash 的任务，处理来自 xFlashReqQueue 的所有请求
 */
void flash_writer_task(void *pv);


 void flash_store_init(void);
#endif
