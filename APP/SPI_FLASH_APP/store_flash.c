#include "store_flash.h"
#include "w25qxx.h"
#include <stdio.h>
#include <string.h>

/* ====================================================================
 * 全局状态变量
 * ==================================================================== */
uint32_t flash_read_index  = 0;   // 读游标：下一个待发送的有效数据
uint32_t flash_write_index = 0;   // 写游标：下一个可用的空位
uint32_t flash_valid_count = 0;   // 有效未发送数据计数
uint8_t  flash_ready        = 1;  // Flash 芯片是否可用
uint32_t g_flash_seq        = 0;  // 全局递增序列号

QueueHandle_t xFlashReqQueue = NULL;  // Flash 请求队列

// PEEK 同步：记录当前等待应答的任务句柄（仅 net_task 调用 peek，不存在并发）
static TaskHandle_t g_peek_caller = NULL;

/* ====================================================================
 * 内部工具函数
 * ==================================================================== */

static uint32_t flash_record_addr(uint32_t index)
{
    return FLASH_DATA_START + (index * FLASH_RECORD_SIZE);
}

static void flash_store_erase_sector(uint32_t index)
{
    if (!flash_ready) return;
    uint32_t sector_index = (index / FLASH_RECORDS_PER_SECTOR);
    uint32_t base_sector   = FLASH_DATA_START / FLASH_SECTOR_SIZE;
    W25QXX_Erase_Sector(base_sector + sector_index);
}

/* ====================================================================
 * 断电恢复：上电扫描 Flash 重建读写指针
 * 通过 seq 字段确定数据写入先后顺序，而非依赖物理地址
 * ==================================================================== */
static void flash_store_rescan(void)
{
    uint8_t  flag            = 0;
    uint32_t first_empty     = 0xFFFFFFFF;
    uint32_t valid_count     = 0;
    uint32_t min_seq         = 0xFFFFFFFF;
    uint32_t min_seq_index   = 0xFFFFFFFF;
    uint32_t max_seq         = 0;
    uint32_t max_seq_index   = 0xFFFFFFFF;

    // 调试：统计异常 flag（既非 EMPTY 也非 VALID 也非 SENT 的值）
    uint32_t abnormal_count = 0;
    uint8_t  abnormal_flags[8];
    uint32_t abnormal_idx[8];
    uint32_t abnormal_n = 0;

    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        W25QXX_Read(&flag, flash_record_addr(i), 1);

        if (flag == FLASH_FLAG_VALID)
        {
            valid_count++;

            uint8_t seq_bytes[4];
            W25QXX_Read(seq_bytes, flash_record_addr(i) + 1, 4);
            uint32_t seq = (uint32_t)seq_bytes[0]
                         | ((uint32_t)seq_bytes[1] << 8)
                         | ((uint32_t)seq_bytes[2] << 16)
                         | ((uint32_t)seq_bytes[3] << 24);

            if (seq < min_seq) { min_seq = seq; min_seq_index = i; }//遍历4096个页面，找到A6里面，时间戳最小的那个A6的位置
            if (seq > max_seq) { max_seq = seq; max_seq_index = i; }//遍历4096个页面，找到A6里面，时间戳最大的那个A6的位置
        }
        else if (flag != FLASH_FLAG_EMPTY && flag != FLASH_FLAG_SENT)
        {
            // 异常 flag：不是 0xFF/0xA6/0x00 的任何值（如 0x13、0x5A）
            abnormal_count++;
            if (abnormal_n < 8)
            {
                abnormal_flags[abnormal_n] = flag;
                abnormal_idx[abnormal_n]   = i;
                abnormal_n++;
            }
        }

        if ((flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT) && first_empty == 0xFFFFFFFF)
        {
            first_empty = i;
        }
    }

    flash_valid_count = valid_count;

    // 调试：打印异常 flag 的数量和前 8 个的位置/值
    if (abnormal_count > 0)
    {
        printf("DEBUG rescan: abnormal_flags=%lu\r\n", (unsigned long)abnormal_count);
        for (uint32_t k = 0; k < abnormal_n; k++)
        {
            printf("  idx=%lu flag=0x%02X\r\n",
                   (unsigned long)abnormal_idx[k], abnormal_flags[k]);
        }
    }
    //min_seq_index未被赋值，没有找到任何有效数据，说明Flash是空的，读写指针都指向第一个空位
    if (min_seq_index == 0xFFFFFFFF)
    {
        flash_read_index  = (first_empty == 0xFFFFFFFF) ? 0 : first_empty;
        flash_write_index = flash_read_index;
        g_flash_seq       = 0;
        printf("DEBUG flash_store_rescan: EMPTY (first_empty=%lu)\r\n", first_empty);
    }
    else
    {
        // 读指针 = seq 最小的A6记录所在位置（该位置必定是 A6）
        flash_read_index  = min_seq_index;
        // 写指针 = seq 最大的A6里记录的下一个位置（该位置必定是 FF 或 00）
        flash_write_index = (max_seq_index + 1U) % FLASH_RECORD_COUNT;
        g_flash_seq       = max_seq + 1;
        printf("DEBUG flash_store_rescan: min_seq=%lu(idx=%lu) max_seq=%lu empty=%lu valid=%lu\r\n",
               min_seq, min_seq_index, max_seq, first_empty, valid_count);
    }
}

/* ====================================================================
 * 格式化：擦除全部数据区（对外导出，供按键等外部触发）
 * ==================================================================== */
void flash_store_format_all(void)
{
    if (!flash_ready) return;
    printf("DEBUG: Formatting Flash (erasing all sectors)...\r\n");

    // uint32_t base_sector = FLASH_DATA_START / FLASH_SECTOR_SIZE;
    // for (uint32_t sector = 0; sector < (FLASH_DATA_SIZE / FLASH_SECTOR_SIZE); sector++)
    // {
    //     W25QXX_Erase_Sector(base_sector + sector);
        
    //     if ((sector + 1) % 64 == 0)
    //         printf("  Erased %lu sectors...\r\n", sector + 1);
    // }
    taskENTER_CRITICAL(); // 进入临界区，防止中断打断擦除操作
    W25QXX_Erase_Chip();   // 擦除整片 Flash
    taskEXIT_CRITICAL();  // 退出临界区   

    printf("DEBUG: Flash format complete!\r\n");

    // 擦除后抽查：读数据区前 8 条记录的 flag，确认是否真的全擦成 0xFF
    // （0xFF=擦除干净；出现 0xA5 等其它值 = 擦除没生效）
    uint8_t chk[8];
    W25QXX_Read(chk, FLASH_DATA_START, 8);
    printf("[format] post-check flags(0x200000): %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
           chk[0], chk[1], chk[2], chk[3], chk[4], chk[5], chk[6], chk[7]);
    // 再抽查数据区末尾（最后一个扇区首地址），确认整个 1MB 都擦了
    uint32_t last_sector_addr = FLASH_DATA_START + FLASH_DATA_SIZE - FLASH_SECTOR_SIZE;
    W25QXX_Read(chk, last_sector_addr, 8);
    printf("[format] post-check flags(0x%lX): %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
           (unsigned long)last_sector_addr,
           chk[0], chk[1], chk[2], chk[3], chk[4], chk[5], chk[6], chk[7]);

    flash_read_index  = 0;
    flash_write_index = 0;
    flash_valid_count = 0;
    g_flash_seq       = 0;
}

/* ====================================================================
 * 初始化：校验 Flash 健康度，扫描恢复索引
 * ==================================================================== */
 void flash_store_init(void)
{
    if (!flash_ready)
    {
        flash_read_index  = 0;
        flash_write_index = 0;
        flash_valid_count = 0;
        return;
    }

  volatile  uint8_t first_flag = 0XAA;
    W25QXX_Read(&first_flag, FLASH_DATA_START, 1);

    if (first_flag != FLASH_FLAG_EMPTY &&
        first_flag != FLASH_FLAG_VALID &&
        first_flag != FLASH_FLAG_SENT)
    {
        printf("DEBUG: Flash appears corrupted (first_flag=0x%02X), formatting...\r\n", first_flag);
        flash_store_format_all();
        return;
    }

    flash_store_rescan();
    printf("Flash Init Done: read_idx=%lu write_idx=%lu valid_count=%lu\r\n",
           flash_read_index, flash_write_index, flash_valid_count);
}

/* ====================================================================
 * 核心操作：存一条离线数据到 Flash（仅由 flash_writer_task 调用）
 * ==================================================================== */
static void flash_store_push(const char *data, uint16_t len)
{
    if (!flash_ready) return;

    uint8_t  flag      = 0;
    uint32_t addr      = 0;
    uint32_t empty_idx = 0xFFFFFFFF;

    // 从 write_index 出发找第一个可用空位
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t idx = (flash_write_index + i) % FLASH_RECORD_COUNT;
        W25QXX_Read(&flag, flash_record_addr(idx), 1);
        if (flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT)
        {
            empty_idx = idx;
            break;
        }
    }

    if (empty_idx != 0xFFFFFFFF)
    {
        flash_write_index = empty_idx;
    }
    else
    {
        // 缓冲区满 → 擦除最旧数据所在扇区 (read_index)，保证 FIFO
        uint32_t sector_start = (flash_read_index / FLASH_RECORDS_PER_SECTOR) * FLASH_RECORDS_PER_SECTOR;
        uint32_t sector_end   = sector_start + FLASH_RECORDS_PER_SECTOR;

        // 统计被擦除的有效记录数
        uint32_t lost = 0;
        for (uint32_t i = sector_start; i < sector_end; i++)
        {
            uint8_t f;
            W25QXX_Read(&f, flash_record_addr(i), 1);
            if (f == FLASH_FLAG_VALID) lost++;
        }

        flash_store_erase_sector(flash_read_index);

        if (flash_valid_count >= lost)
            flash_valid_count -= lost;
        else
            flash_valid_count = 0;

        // read_index 跳到擦除扇区之后第一个有效记录
        flash_read_index = sector_end % FLASH_RECORD_COUNT;
        if (flash_valid_count > 0)
        {
            uint32_t start = flash_read_index;
            uint8_t  f;
            for (;;)
            {
                W25QXX_Read(&f, flash_record_addr(flash_read_index), 1);
                if (f == FLASH_FLAG_VALID) break;
                flash_read_index = (flash_read_index + 1) % FLASH_RECORD_COUNT;
                if (flash_read_index == start) break;
            }
        }

        flash_write_index = sector_start;
    }

    addr = flash_record_addr(flash_write_index);

    // 强制截断保护
    if (len > sizeof(((FlashRecord *)0)->data))
        len = sizeof(((FlashRecord *)0)->data);

    // 拼装 256 字节帧: [flag:1B][seq:4B][len:2B][data:249B]
    uint8_t buf[FLASH_RECORD_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    buf[0] = FLASH_FLAG_VALID;
    buf[1] = (uint8_t)(g_flash_seq & 0xFF);
    buf[2] = (uint8_t)((g_flash_seq >> 8) & 0xFF);
    buf[3] = (uint8_t)((g_flash_seq >> 16) & 0xFF);
    buf[4] = (uint8_t)((g_flash_seq >> 24) & 0xFF);
    buf[5] = (uint8_t)(len & 0xFF);
    buf[6] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(&buf[7], data, len);
    g_flash_seq++;

    W25QXX_Write(buf, addr, FLASH_RECORD_SIZE);

    flash_write_index = (flash_write_index + 1) % FLASH_RECORD_COUNT;
    if (flash_valid_count < FLASH_RECORD_COUNT) flash_valid_count++;
}

/* ====================================================================
 * 核心操作：读取最旧的有效数据（仅由 flash_writer_task 调用）
 * ==================================================================== */
static int flash_store_peek(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index)
{
    if (!flash_ready)      return 0;
    if (flash_valid_count == 0) return 0;

    uint8_t  flag  = 0;
    uint32_t start = flash_read_index;
    uint32_t idx   = flash_read_index;

    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t addr = flash_record_addr(idx);
        W25QXX_Read(&flag, addr, 1);

        if (flag == FLASH_FLAG_VALID)
        {
            // 布局: [flag:1B][seq:4B][len:2B][data:...]
            uint8_t len_bytes[2];
            W25QXX_Read(len_bytes, addr + 5, 2);
            uint16_t len = (uint16_t)(len_bytes[0] | (len_bytes[1] << 8));

            if (len >= max_len) len = max_len - 1;

            W25QXX_Read((uint8_t *)out, addr + 7, len);
            out[len] = '\0';

            *out_len   = len;
            *out_index = idx;
            return 1;
        }

        idx = (idx + 1) % FLASH_RECORD_COUNT;
        if (idx == start) break;
    }

    flash_store_rescan();
    return 0;
}

/* ====================================================================
 * 核心操作：标记一条记录为已发送（仅由 flash_writer_task 调用）
 * ==================================================================== */
static void flash_store_mark_sent(uint32_t index)
{
    if (!flash_ready) return;

    uint8_t sent_flag = FLASH_FLAG_SENT;
    W25QXX_Write(&sent_flag, flash_record_addr(index), 1);

    if (flash_valid_count > 0) flash_valid_count--;
    flash_read_index = (index + 1) % FLASH_RECORD_COUNT;
}

/* ====================================================================
 * 对外异步 API：发送请求到队列，立即返回
 * ==================================================================== */

int flash_store_push_async(const char *data, uint16_t len)
{
    if (xFlashReqQueue == NULL) return 0;

    FlashRequest req;
    memset(&req, 0, sizeof(req));
    req.type = FLASH_REQ_PUSH;
    if (len > FLASH_REQ_DATA_MAX) len = FLASH_REQ_DATA_MAX;
    memcpy(req.push.data, data, len);
    req.push.len = len;

    if (xQueueSend(xFlashReqQueue, &req, 0) != pdTRUE)
    {
        // 队列满 → 数据无法入队，稍后重试
        return 0;
    }
    return 1;
}

int flash_store_peek_async(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index)
{
    if (xFlashReqQueue == NULL) return 0;

    FlashRequest req;
    memset(&req, 0, sizeof(req));
    req.type           = FLASH_REQ_PEEK;
    req.peek.out       = out;
    req.peek.max_len   = max_len;
    req.peek.out_len   = out_len;
    req.peek.out_index = out_index;
    req.peek.ret       = NULL;  // 由 flash_writer_task 写入

    int ret_val = 0;
    req.peek.ret = &ret_val;

    // 记录当前任务句柄，供 flash_writer_task 唤醒
    g_peek_caller = xTaskGetCurrentTaskHandle();

    // 发送请求（阻塞等待队列有空位）
    if (xQueueSend(xFlashReqQueue, &req, portMAX_DELAY) != pdTRUE)
    {
        return 0;
    }

    // 阻塞等待 flash_writer_task 处理完毕并通过 TaskNotify 唤醒
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    return ret_val;
}

void flash_store_mark_sent_async(uint32_t index)
{
    if (xFlashReqQueue == NULL) return;

    FlashRequest req;
    memset(&req, 0, sizeof(req));
    req.type            = FLASH_REQ_MARK_SENT;
    req.mark_sent.index = index;

    xQueueSend(xFlashReqQueue, &req, 0);
}

void flash_store_format_async(void)
{
    if (xFlashReqQueue == NULL) return;

    FlashRequest req;
    memset(&req, 0, sizeof(req));
    req.type = FLASH_REQ_FORMAT;

    xQueueSend(xFlashReqQueue, &req, 0);
}

/* ====================================================================
 * Flash 写任务：唯一直接操作 SPI Flash 的 FreeRTOS 任务
 * ==================================================================== */
void flash_writer_task(void *pv)
{
    // 启动时先初始化 Flash（扫描恢复）
    //flash_store_format_all();
   

    FlashRequest req;

    while (1)
    {
        // 阻塞等待请求
        if (xQueueReceive(xFlashReqQueue, &req, portMAX_DELAY) != pdTRUE)
            continue;

        switch (req.type)
        {
            case FLASH_REQ_PUSH:
                flash_store_push(req.push.data, req.push.len);
                break;

            case FLASH_REQ_PEEK:
                if (req.peek.ret != NULL)
                {
                    *req.peek.ret = flash_store_peek(req.peek.out,
                                                       req.peek.max_len,
                                                       req.peek.out_len,
                                                       req.peek.out_index);
                }
                // 唤醒等待的调用者
                if (g_peek_caller != NULL)
                {
                    xTaskNotifyGive(g_peek_caller);
                    g_peek_caller = NULL;
                }
                break;

            case FLASH_REQ_MARK_SENT:
                flash_store_mark_sent(req.mark_sent.index);
                break;

            case FLASH_REQ_FORMAT:
                flash_store_format_all();
               // delay_sms(100); // 等待擦除完成
                break;

            default:
                break;
        }
       // vTaskDelay(pdMS_TO_TICKS(100)); // 避免任务饥饿，给其他任务机会
    }
}
