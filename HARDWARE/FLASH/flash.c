#include "main.h"
#include "flash.h"
#include <stdio.h>
#include <stm32f4xx_hal.h>
#include <string.h>

/**
 * @brief       根据传入的绝对物理地址，计算出其所属的内部 Flash 扇区(Sector)编号
 * @param       Address: 内部 Flash 上的绝对地址
 * @retval      对应的扇区宏定义 (FLASH_SECTOR_0 ~ FLASH_SECTOR_23)
 * @note        STM32F429 属于双 Bank 闪存结构，1MB 或 2MB 容量，扇区大小并不均匀
 *              Sector 0~3 / 12~15 : 16KB
 *              Sector 4 / 16      : 64KB
 *              Sector 5~11 / 17~23: 128KB
 */
static uint32_t GetSector(uint32_t Address)
{
	if(Address < 0x08004000) return FLASH_SECTOR_0;
	if(Address < 0x08008000) return FLASH_SECTOR_1;
	if(Address < 0x0800C000) return FLASH_SECTOR_2;
	if(Address < 0x08010000) return FLASH_SECTOR_3;
	if(Address < 0x08020000) return FLASH_SECTOR_4;
	if(Address < 0x08040000) return FLASH_SECTOR_5;
	if(Address < 0x08060000) return FLASH_SECTOR_6;
	if(Address < 0x08080000) return FLASH_SECTOR_7;
	if(Address < 0x080A0000) return FLASH_SECTOR_8;
	if(Address < 0x080C0000) return FLASH_SECTOR_9;
	if(Address < 0x080E0000) return FLASH_SECTOR_10;
	if(Address < 0x08100000) return FLASH_SECTOR_11;
	if(Address < 0x08104000) return FLASH_SECTOR_12;
	if(Address < 0x08108000) return FLASH_SECTOR_13;
	if(Address < 0x0810C000) return FLASH_SECTOR_14;
	if(Address < 0x08110000) return FLASH_SECTOR_15;
	if(Address < 0x08120000) return FLASH_SECTOR_16;
	if(Address < 0x08140000) return FLASH_SECTOR_17;
	if(Address < 0x08160000) return FLASH_SECTOR_18;
	if(Address < 0x08180000) return FLASH_SECTOR_19;
	if(Address < 0x081A0000) return FLASH_SECTOR_20;
	if(Address < 0x081C0000) return FLASH_SECTOR_21;
	if(Address < 0x081E0000) return FLASH_SECTOR_22;
	return FLASH_SECTOR_23;
}

/**
 * @brief       根据扇区编号获取该扇区的物理容量大小
 * @param       Sector: 扇区宏定义
 * @retval      该扇区的字节数大小
 */
static uint32_t GetSectorSize(uint32_t Sector)
{
	switch(Sector)
	{
		case FLASH_SECTOR_0:
		case FLASH_SECTOR_1:
		case FLASH_SECTOR_2:
		case FLASH_SECTOR_3:
		case FLASH_SECTOR_12:
		case FLASH_SECTOR_13:
		case FLASH_SECTOR_14:
		case FLASH_SECTOR_15:
			return 16 * 1024;  // 16KB
		case FLASH_SECTOR_4:
		case FLASH_SECTOR_16:
			return 64 * 1024;  // 64KB
		default:
			return 128 * 1024; // 128KB
	}
}

/**
 * @brief       根据给定的起始地址和总字节数，擦除覆盖到的所有内部 Flash 扇区
 *              因为 Flash 必须先擦(变0xFF)后写，且擦除的最小粒度是扇区(Sector)
 * @param       startAddr: 擦除起点的物理基址
 * @param       sizeBytes: 需要擦除的总字节长度
 * @return      1: 成功; 0: 擦除失败
 */
static int EraseRange(uint32_t startAddr, uint32_t sizeBytes)
{
    if (sizeBytes == 0) return 0;
	HAL_StatusTypeDef st;
    
    // 计算起点和终点落在哪些扇区上
	uint32_t startSector = GetSector(startAddr);
	uint32_t endAddr = startAddr + sizeBytes - 1;
	uint32_t endSector = GetSector(endAddr);
    // 需要擦除的扇区总个数
	uint32_t nb = (endSector - startSector) + 1;
    
	FLASH_EraseInitTypeDef cfg;
	uint32_t err = 0;
    
	cfg.TypeErase = FLASH_TYPEERASE_SECTORS;
    // F429 地址大于 0x08100000 属于 Bank2 区域
	cfg.Banks = (startAddr >= 0x08100000) ? FLASH_BANK_2 : FLASH_BANK_1;
	cfg.Sector = startSector;
	cfg.NbSectors = nb;
    // 工作电压 2.7~3.6V (Range 3) 支持字(Word/32bit)并行编程，速度最快
	cfg.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    
	HAL_FLASH_Unlock();
	st = HAL_FLASHEx_Erase(&cfg, &err); // 调用 HAL 库阻塞擦除
	HAL_FLASH_Lock();
    
	return (st == HAL_OK) ? 1 : 0;
}

/**
 * @brief       将一系列 32 位整型数据批量写入内部 Flash (按字写入)
 * @param       addr: 目标写入物理基址 (需 4 字节对齐)
 * @param       buff: 待写入数据存放的内存数组指针
 * @param       word_size: 需要写入的字(Word=4Bytes)的总数量
 * @return      1: 成功; 0: 报错中止
 */
static int WriteFlashWords(uint32_t addr, const uint32_t *buff, uint32_t word_size)
{
    HAL_StatusTypeDef st = HAL_OK;
	HAL_FLASH_Unlock();
	for(uint32_t i = 0; i < word_size; i++)
	{
		st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + 4 * i, buff[i]);
        if (st != HAL_OK)
        {
            break; // 遇到写失败即中断
        }
	}
	HAL_FLASH_Lock();
    return (st == HAL_OK) ? 1 : 0;
}

/**
 * @brief       从内部 Flash 的指定地址读取批量 32 位(Word)数据到内存
 * @param       addr: 需要读取的 Flash 起始地址
 * @param       buff: 收容数据的数组指针
 * @param       word_size: 需要读取的字(Word = 4Byte)的数量
 * @note        STM32 的内部 Flash 直接映射到 CPU 地址总线，因此只需强转指针使用解引用 `*` 读取即可，
 *              无需解锁交互(与外置SPI不一样)。
 */
static void ReadFlashWords(uint32_t addr, uint32_t *buff, uint16_t word_size)
{
	for(int i =0; i < word_size; i++)
	{
		buff[i] = *(__IO unsigned int*)(addr + 4 * i);
	}
	return;
}

/**
 * @brief  获取 OTA 后场程序等待区(Application_2)的起始下载地址
 */
uint32_t OTA_GetDownloadAddr(void)
{
    return Application_2_Addr;
}

/**
 * @brief  获取分配给单个 APP 固件区的最大容量限制
 */
uint32_t OTA_GetDownloadMaxSize(void)
{
    return Application_Size;
}

/**
 * @brief       在 OTA 开始下载前做准备清理动作。基于即将要下载的新固件大小，
 *              将后场等待区 Flash(Application_2) 对应的那些扇区预先执行擦除。
 * @param       image_size: 要下载的 bin 包的文件总大小
 * @return      1: 擦除成功; 0: 擦除失败或固件超大
 */
int OTA_PrepareDownloadArea(uint32_t image_size)
{
    // 如果大小为 0 或者超过了设定允许的 Application_Size(512KB) 报错
    if (image_size == 0 || image_size > Application_Size)
    {
        return 0;
    }
    return EraseRange(Application_2_Addr, image_size);
}

/**
 * @brief       在 OTA 下载过程中(常用于 mqtt/http 分包接收)，
 *              把收到的一个固件分片数据实时存入到内部 Flash 里去，拼凑成完整固件。
 * @param       offset: 该分片相对于整个后场起始地址 Application_2_Addr 的游标偏移 (字节)
 * @param       data: 指向本次分片原始数组数据的指针
 * @param       len: 本次分片的字节数长度
 * @return      1: 写入成功; 0: 写入失败跨界了
 */
int OTA_WriteDownloadChunk(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) return 0;
    // 越界保护检查
    if ((offset + len) > Application_Size) return 0;
    
    // 换算得出该片所在的绝对写入物理地址
    uint32_t addr = Application_2_Addr + offset;
    HAL_StatusTypeDef st = HAL_OK;
    
    HAL_FLASH_Unlock();
    // 因为对 Flash 最快安全的编程是按Word(4字节)对齐写的
    for (uint32_t i = 0; i < len; i += 4)
    {
        uint32_t word = 0xFFFFFFFFU; // 默认空位保持 0xFF 不伤Flash
        uint32_t remain = len - i;
        // 如果尾部数据不够 4 个字节了，按实际余量拷贝来凑字，其余保留 0xFF
        uint32_t copy_len = (remain >= 4U) ? 4U : remain;
        memcpy(&word, data + i, copy_len);
        
        // 当心: 若 addr+i 未对齐到4字节整数倍，调用本API会报错。
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word);
        if (st != HAL_OK)
        {
            break;
        }
    }
    HAL_FLASH_Lock();
    return (st == HAL_OK) ? 1 : 0;
}

/**
 * @brief       从 OTA 下载后备区读取一定长度的纯二进制数据。
 *              主要用于固件下载完之后，本地进行全片扫描累加计算 MD5 来核对准确性。
 */
void OTA_ReadDownloadData(uint32_t offset, uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) return;
    if ((offset + len) > Application_Size) return;
    
    // 内部 Flash 能像普通内存一样直接按照指针偏移抓取值，不需要复杂的 API
    memcpy(data, (const void *)(Application_2_Addr + offset), len);
}              

/**
 * @brief       读取保存在元数据区内的整包【OTA 升级控制状态块】
 * @param       meta: 存放读取出来的实体的目标内存地址
 * @param       meta_size: 大小效验防越界 (填入 sizeof(OtaBootMeta) )
 * @return      1: 有效且魔数对应；0: 内容破损或无升级任务记录
 */
int OTA_ReadBootMeta(void *meta, uint32_t meta_size)
{
    if (meta == NULL || meta_size != sizeof(OtaBootMeta)) return 0;
    
    // 把该物理首地址上所有定义的数据结构字段整个抽出来
    memcpy(meta, (const void *)META_Flag_Addr, sizeof(OtaBootMeta));
    OtaBootMeta *m = (OtaBootMeta *)meta; 
    
    // 对比内部魔数识别码是不是约定好的 0x54414F31U ("1OAT")
    if (m->magic != OTA_META_MAGIC) return 0;
    
    return 1;
}

/**
 * @brief       将最新的升级固件信息登记到元数据控制区中。
 *              一旦写完生效，设备下一次重启时，Bootloader 将探测到 PENDING 状态并介入搬运！
 * @param       image_size: 审核验收完毕的那个新分区的固件大小
 * @param       md5: 16字节的 MD5 值。BootLoader 搬运前后可通过此字段反复二次验证
 * @param       version: 本次升级的目标版本号字符串文本
 * @param       token: 属于本条升级指令的网络任务流水号，备用。
 */
int OTA_SetPendingImage(uint32_t image_size, const uint8_t md5[16], const char *version, const char *token)
{
    if (md5 == NULL) return 0;
    if (image_size == 0 || image_size > Application_Size) return 0; // 防护：超大固件会抹掉别的区
    
    OtaBootMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = OTA_META_MAGIC;
    meta.state = OTA_META_STATE_PENDING;     // 关键步骤：设立为“拥有未完成的升级挂起任务”
    meta.active_addr = Application_1_Addr;
    meta.pending_addr = Application_2_Addr;
    meta.image_size = image_size;
    memcpy(meta.md5, md5, 16);
    
    if (version != NULL) strncpy(meta.target_version, version, sizeof(meta.target_version) - 1U);
    if (token != NULL) strncpy(meta.ota_token, token, sizeof(meta.ota_token) - 1U);
    
    // 元数据区也需要擦除(其在单独的一个扇区，避免擦到正常代码)
    if (!EraseRange(META_Flag_Addr, sizeof(meta)))
    {
        return 0;
    }
    
    // 把整合好的结构体按 Word(4字节单位) 写入
    return WriteFlashWords(META_Flag_Addr, (const uint32_t *)&meta, sizeof(meta) / 4U);
}

/**
 * @brief       把 OTA 元数据的状态置空消除。将其降级成普通的 NONE 无任务状态。
 *              通常由 BootLoader 搬运完成，或是发现 APP 运行异常不得不退回旧版本时使用。
 */
int OTA_ClearPendingImage(void)
{
    OtaBootMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = OTA_META_MAGIC;
    meta.state = OTA_META_STATE_NONE;        // 清空白板模式，BootLoader 会随之直通 Application区
    meta.active_addr = Application_1_Addr;
    meta.pending_addr = Application_2_Addr;
    if (!EraseRange(META_Flag_Addr, sizeof(meta)))
    {
        return 0;
    }
    return WriteFlashWords(META_Flag_Addr, (const uint32_t *)&meta, sizeof(meta) / 4U);
}

/**
 * @brief       这是一个极重量级操作的内网代码搬移函数（给 Bootloader 打底用）。
 *              逻辑很简单粗暴但是生效： 先擦除目的地区域 -> 循环一小片一小片复制过去 -> 最后反手抹除源区域。
 * @param       src_addr: 要拿走的代码的数据源起始地址 (如后台 APP2)
 * @param       des_addr: 存放这些代码的直接运行落脚点 (如主运行区 APP1)
 * @param       byte_size: 一共多大字节？
 */
void MoveCode(unsigned int src_addr, unsigned int des_addr, unsigned int byte_size)
{
	printf("> Start erase des flash......\r\n");
    // 1、清空准备容纳新兵的坑位（主运行代码区）
	EraseRange(des_addr, byte_size);
	printf("> Erase des flash down......\r\n");

	// 创建 1024 字节大小的一个数据中转站（1个块/由于256个int = 1024字节）
	static unsigned int temp[256];

	printf("> Start copy......\r\n");
    uint32_t total_blocks = (byte_size + 1023) / 1024;
	for(int i = 0; i < total_blocks; i++)
	{
        // 2、一片片从后备缓存扇区借出代码，放到临时池里
		ReadFlashWords((src_addr + i*1024), temp, 256);
        // 3、拍成 4字节对齐字，怼入目标位置
		WriteFlashWords((des_addr + i*1024), temp, 256);
	}
	printf("> Copy down......\r\n");

	printf("> Start erase src flash......\r\n");
    // 4、过河拆桥：为了安全和节省下次 OTA 时由于不需要的杂数据造成麻烦，搬完当场删掉下载备份区的数据
	EraseRange(src_addr, byte_size);
	printf("> Erase src flash down......\r\n");
}
