#include "store_flash.h"
#include "w25qxx.h"
#include <stdio.h>
#include <string.h>

/** 读游标：指向下一个有效待发送数据所在的逻辑索引 */
uint32_t flash_read_index = 0;
/** 写游标：指向下一个可用的空位逻辑索引 */
uint32_t flash_write_index = 0;
/** 有效数据总数：当前 Flash 存储的未发送有效传感数据量 */
uint32_t flash_valid_count = 0;
/** 硬件就绪标志：1 表示 SPI Flash 正常可用 */
uint8_t flash_ready = 1;

/**
 * @brief       根据逻辑索引计算其对应数据在 Flash 中的真实物理地址
 * @param       index: 数据的逻辑索引 (0 ~ FLASH_RECORD_COUNT-1)
 * @retval      Flash 绝对物理地址
 */
static uint32_t flash_record_addr(uint32_t index)
{
    return FLASH_DATA_START + (index * FLASH_RECORD_SIZE);
}

/**
 * @brief       设备重启上电时扫描 Flash，寻找读写游标、统计尚存多少条未上传的离线数据
 *              扫描过程中会确定第一个有效数据所在的位置 (供发送用) 
 *              以及第一个空闲/已发送位置 (供存储新数据用)
 */
static void flash_store_rescan(void) 
{
    uint8_t flag = 0;
    uint32_t first_empty = 0xFFFFFFFF;
    uint32_t first_valid = 0xFFFFFFFF;
    uint32_t valid_count = 0;  // 临时变量，先计算有多少条有效数据
    
    // ✅ 第一遍扫描：遍历所有的 Block，统计有效数据并寻找环形队列首尾的关键位置
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        W25QXX_Read(&flag, flash_record_addr(i), 1);
        
        // 遇到有效数据包 0xA5
        if (flag == FLASH_FLAG_VALID)
        {
            valid_count++;
            if (first_valid == 0xFFFFFFFF) first_valid = i; // 记录最前面一条有效数据位置
        }
        
        // 遇到空块(0xFF)或被标记已发送作废的块(0x00)
        if ((flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT) && first_empty == 0xFFFFFFFF)
        {
            first_empty = i; // 记录最前面可以写入存放新数据的位置
        }
    }
    
    flash_valid_count = valid_count;  // 设置全局最终的计数
    
    if (first_valid == 0xFFFFFFFF)
    {
        // 若找不到有效数据，说明 Flash 是全空的（或者全是被打上已发送标签作废的）
        flash_read_index = (first_empty == 0xFFFFFFFF) ? 0 : first_empty;
        flash_write_index = flash_read_index;
        printf("DEBUG flash_store_rescan: EMPTY (first_empty=%lu)\r\n", first_empty);
    }
    else
    {
        // 存在历史有效断网数据，需要恢复续传
        flash_read_index = first_valid;  // 读指针指到最老的那条有效数据上
        flash_write_index = (first_empty == 0xFFFFFFFF) ? first_valid : first_empty; 
        printf("DEBUG flash_store_rescan: first_valid=%lu, first_empty=%lu, valid_count=%lu\r\n",
               first_valid, first_empty, valid_count);
    }
}

/**
 * @brief       强制清空离线存储所分配的全部 Flash 扇区
 *              只有当 Flash 数据由于各种原因整体损毁混乱时才调用
 */
static void flash_store_format(void)
{
    if (!flash_ready) return;
    
    printf("DEBUG: Formatting Flash (erasing all sectors)...\r\n");
    
    // 挨个擦除所划分的数据存储区内的所有 Flash Sector（W25QXX最小擦除单位为4KB扇区）
    for (uint32_t sector = 0; sector < (FLASH_DATA_SIZE / FLASH_SECTOR_SIZE); sector++)
    {
        uint32_t addr = FLASH_DATA_START + sector * FLASH_SECTOR_SIZE;
        // W25QXX API 的擦除粒度通常传的是扇区编号（这里做绝对物理地址到扇区号的换算）
        W25QXX_Erase_Sector((addr - FLASH_DATA_START) / FLASH_SECTOR_SIZE);
        
        if ((sector + 1) % 64 == 0)
        {
            printf("  Erased %lu sectors...\r\n", sector + 1); // 擦除进度打印
        }
    }
    
    printf("DEBUG: Flash format complete!\r\n");
    
    // 格式化完成，复原重置所有索引计数指针
    flash_read_index = 0;
    flash_write_index = 0;
    flash_valid_count = 0;
}

/**
 * @brief       模块初始化入口。验证 Flash 分区健康度并尝试恢复存储索引
 */
void flash_store_init(void)
{
    if (!flash_ready)
    {
        // 无外设可用时，全部清 0
        flash_read_index = 0;
        flash_write_index = 0;
        flash_valid_count = 0;
        return;
    }
    
    // 快速读取存储区最开头的第一个数据标志位，检测是否面临首次用或坏区
    uint8_t first_flag = 0xFF;
    W25QXX_Read(&first_flag, FLASH_DATA_START, 1);
    
    // 如果第一个位置既不是 0xFF（空），也不是 0xA5（有效），也不是 0x00（已发送作废）
    // 说明 Flash 内保存的标记完全是异常乱码数据，极有可能发生了大范围错位或者未初始化，直接格式化
    if (first_flag != FLASH_FLAG_EMPTY && first_flag != FLASH_FLAG_VALID && first_flag != FLASH_FLAG_SENT)
    {
        printf("DEBUG: Flash appears corrupted (first_flag=0x%02X), formatting...\r\n", first_flag);
        flash_store_format();
        return;
    }
    
    // 数据看着正常，进行全盘扫描以构建环形链表的读写指针映射
    flash_store_rescan();
    printf("Flash Init Done: read_idx=%lu, write_idx=%lu, valid_count=%lu\r\n",
           flash_read_index, flash_write_index, flash_valid_count);
}

/**
 * @brief       针对包含某条记录逻辑索引的 4KB 物理扇区进行擦除
 * @param       index: 此条记录对应的逻辑索引
 */
static void flash_store_erase_sector(uint32_t index)
{
    if (!flash_ready) return;
    uint32_t sector_index = (index / FLASH_RECORDS_PER_SECTOR);
    uint32_t sector_addr = FLASH_DATA_START + sector_index * FLASH_SECTOR_SIZE;
    W25QXX_Erase_Sector((sector_addr - FLASH_DATA_START) / FLASH_SECTOR_SIZE);
}

/**
 * @brief       将一条新的 JSON 字符串断网数据存入 Flash 环形队列中
 * @param       data: 指向字符串的指针
 * @param       len: 字符串的长度
 */
static void flash_store_push(const char *data, uint16_t len)
{
    if (!flash_ready) return;
    
    uint8_t flag = 0;
    uint32_t addr = 0;
    uint32_t empty_idx = 0xFFFFFFFF;
    
    // 从当前预期要写的位置开始，往后找是否有现成的空位或废弃位可以覆写
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t idx = (flash_write_index + i) % FLASH_RECORD_COUNT;
        W25QXX_Read(&flag, flash_record_addr(idx), 1);
        if (flag == FLASH_FLAG_EMPTY || flag == FLASH_FLAG_SENT)
        {
            empty_idx = idx;
            break;  // 找到能写的空位了，停止查找
        }
    }

    if (empty_idx != 0xFFFFFFFF)
    {
        // 若有现成的空位，就选它 
        flash_write_index = empty_idx;
    }
    else
    {
        // 【最极端情况】写满了，没有空位或废弃位 -> 需要覆写最旧的区，进行强行擦除
        // 算出当前写游标所在的宏观 4KB 扇区头和尾包含哪些消息条数区间
        uint32_t sector_start = (flash_write_index / FLASH_RECORDS_PER_SECTOR) * FLASH_RECORDS_PER_SECTOR;
        uint32_t sector_end = sector_start + FLASH_RECORDS_PER_SECTOR;

        // 直接抹除这个扇区，腾出整个 Sector 大小的空位
        flash_store_erase_sector(flash_write_index);

        // 如果强制擦除的扇区里面碰巧包含了当前还没来得及上传的“第一条数据”(读游标正停在这)
        // 那读游标就乱了，所以触发一次全局重新找寻恢复读游标
        if (flash_read_index >= sector_start && flash_read_index < sector_end)
        {
            flash_store_rescan();
        }
        flash_write_index = sector_start; // 更新写指针为刚刚擦除出来的位置第一条
    }

    addr = flash_record_addr(flash_write_index);
    // 强制截断保护，禁止越界写垮页
    if (len > sizeof(((FlashRecord *)0)->data)) len = sizeof(((FlashRecord *)0)->data);
    
    // 拼装 256 字节的完整写入帧
    uint8_t buf[FLASH_RECORD_SIZE];
    memset(buf, 0xFF, sizeof(buf));  // 未使用的部分保持 0xFF 可以减少 Flash 损耗
    buf[0] = FLASH_FLAG_VALID;       // 数据标记位 (0xA5)
    buf[1] = (uint8_t)(len & 0xFF);         // 长度的低 8 位
    buf[2] = (uint8_t)((len >> 8) & 0xFF);  // 长度的高 8 位
    memcpy(&buf[3], data, len);             // JSON 有效荷载
    
    W25QXX_Write(buf, addr, FLASH_RECORD_SIZE); // 按页一次性烧录写入到 Flash 闪存芯片
    
    // 写完本条后让指针往后推一格，遇到尾部就折返 0(环形队列逻辑)
    flash_write_index = (flash_write_index + 1) % FLASH_RECORD_COUNT;
    // 总有效数增加(封顶不超过总容量上限)
    if (flash_valid_count < FLASH_RECORD_COUNT) flash_valid_count++;
}

/**
 * @brief       预览并拷贝出当前存放的最老的一起数据 (供断网重连后上报处理)
 * @attention   该函数属于 peek 行为，仅供读取。只有发送真的成功后才会将其标记抹杀
 * @retval      返回 1 读出了数据，返回 0 没数据
 */
static int flash_store_peek(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index)
{
    if (!flash_ready) return 0;
    if (flash_valid_count == 0) return 0; // 若自认为没有效数据，直接返回
    
    uint8_t flag = 0;
    uint32_t start = flash_read_index;
    uint32_t idx = flash_read_index;
    
    // 顺着当前读游标往下遍历去找第一个含 0xA5 的有效标记块
    for (uint32_t i = 0; i < FLASH_RECORD_COUNT; i++)
    {
        uint32_t addr = flash_record_addr(idx);
        W25QXX_Read(&flag, addr, 1);
        
        if (flag == FLASH_FLAG_VALID)
        {
            // 抓到有效节点，提取出长度标志位
            uint8_t len_bytes[2];
            W25QXX_Read(len_bytes, addr + 1, 2);
            uint16_t len = (uint16_t)(len_bytes[0] | (len_bytes[1] << 8));
            
            // 安全限制避免数组撑爆
            if (len >= max_len) len = max_len - 1;
            
            // 掏出有效报文荷载
            W25QXX_Read((uint8_t *)out, addr + 3, len);
            out[len] = '\0'; // 强行补充字符末尾结尾符\0保证 printf 不乱跑
            
            *out_len = len;   // 传出长度
            *out_index = idx; // 传出该条数据所在游标
            return 1;
        }
        
        idx = (idx + 1) % FLASH_RECORD_COUNT; // 找不到往下个格推进寻找
        if (idx == start)
        {
            break; // 绕了一圈回来了还是没有有效，说明 Flash 实际上空了
        }
    }
    
    // 到了这里说明预期有 valid_count 但实际上找不到数据，缓存状态彻底不一致了，执行一次重扫
    flash_store_rescan();
    return 0;
}

/**
 * @brief       将指定游标位置的离线历史数据手动打上 “不可用的已发送(0x00)” 作废下架标签
 *              （将 FLASH_FLAG_VALID 改为 FLASH_FLAG_SENT）
 *              不需要执行耗时的页/扇区擦除操作。直接覆写 0x00 下去即可。
 */
static void flash_store_mark_sent(uint32_t index)
{
    if (!flash_ready) return;
    
    uint8_t sent_flag = FLASH_FLAG_SENT;
    // 只需要将头部的 1 字节 0xA5 变成 0x00，在不擦除的情况下 Flash 允许将 1 写为 0
    W25QXX_Write(&sent_flag, flash_record_addr(index), 1);
    
    // 成功上报剔除，有效数据减 1
    if (flash_valid_count > 0) flash_valid_count--;
    
    // 读游标顺理成章移到这一个逻辑的下一格去准备下一次 peek
    flash_read_index = (index + 1) % FLASH_RECORD_COUNT;
}

// ======================= 下面暴露给其它业务逻辑多任务使用的 互斥上锁版本 =======================

/**
 * @brief       多线程写保护的 push 入列函数 (存新数据)
 */
void flash_store_push_locked(const char *data, uint16_t len)
{
    if (xFlashMutex && xSemaphoreTake(xFlashMutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        flash_store_push(data, len);
        xSemaphoreGive(xFlashMutex);
    }
    else
    {
        // 如果实在拿不到锁或者锁还没创建也硬存一次试试
        flash_store_push(data, len);
    }
}

/**
 * @brief       多线程写保护的 peek 取出函数 (准备读去传)
 */
int flash_store_peek_locked(char *out, uint16_t max_len, uint16_t *out_len, uint32_t *out_index)
{
    int ret = 0;
    if (xFlashMutex && xSemaphoreTake(xFlashMutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        ret = flash_store_peek(out, max_len, out_len, out_index);
        xSemaphoreGive(xFlashMutex);
        return ret;
    }
    return flash_store_peek(out, max_len, out_len, out_index);
}

/**
 * @brief       多线程写保护的 mark sent 函数 (用完剔除了)
 */
void flash_store_mark_sent_locked(uint32_t index)
{
    if (xFlashMutex && xSemaphoreTake(xFlashMutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        flash_store_mark_sent(index);
        xSemaphoreGive(xFlashMutex);
    }
    else
    {
        flash_store_mark_sent(index);
    }
}
