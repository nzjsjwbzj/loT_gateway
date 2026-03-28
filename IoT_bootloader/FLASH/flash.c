#include "main.h"
#include "flash.h"
#include "stm32f4xx_hal.h"
#include "stm32f429xx.h"
#include "core_cm4.h"
#include <stdio.h>
#include <string.h>


// 为了兼容正点原子或者老工程里的 u32, u8 定义
#include "sys.h" 
#include "delay.h"

/*! 
 * @brief 根据Flash绝对地址获取所属的扇区编号
 *
 * @param [in] Address    Flash 绝对地址
 *
 * @return 对应的扇区宏定义 (如 FLASH_SECTOR_0, FLASH_SECTOR_12 等)
 */
uint16_t GetSector(uint32_t Address)
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

/*! 
 * @brief 获取指定扇区的大小
 *
 * @param [in] Sector     扇区编号 (如 FLASH_SECTOR_0)
 *
 * @return 扇区的字节大小
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
			return 16 * 1024;
		case FLASH_SECTOR_4:
		case FLASH_SECTOR_16:
			return 64 * 1024;
		default:
			return 128 * 1024;
	}
}

/*! 
 * @brief 擦除指定地址范围对应的Flash扇区
 *
 * @param [in] startAddr  起始绝对地址
 * @param [in] sizeBytes  擦除的字节总大小
 *
 * @retval 1 成功
 * @retval 0 失败
 */
static int EraseRange(uint32_t startAddr, uint32_t sizeBytes)
{
    if (sizeBytes == 0) return 0;
	HAL_StatusTypeDef st;
	uint32_t startSector = GetSector(startAddr);
	uint32_t endAddr = startAddr + sizeBytes - 1;
	uint32_t endSector = GetSector(endAddr);
	uint32_t nb = (endSector - startSector) + 1;
	FLASH_EraseInitTypeDef cfg;
	uint32_t err = 0;
	cfg.TypeErase = FLASH_TYPEERASE_SECTORS;
	cfg.Banks = (startAddr >= 0x08100000) ? FLASH_BANK_2 : FLASH_BANK_1;
	cfg.Sector = startSector;
	cfg.NbSectors = nb;
	cfg.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	HAL_FLASH_Unlock();
	st = HAL_FLASHEx_Erase(&cfg, &err);
	HAL_FLASH_Lock();
	return (st == HAL_OK) ? 1 : 0;
}

/*! 
 * @brief 连续向Flash写入多个以Word(4字节)为单位的数据
 *
 * @param [in] addr       起始写入地址
 * @param [in] buff       指向要写入的数据缓冲区（通常为字对齐数据）
 * @param [in] word_size  要写入的Word数量
 *
 * @retval 1 成功
 * @retval 0 失败
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
            break;
        }
	}
	HAL_FLASH_Lock();
    return (st == HAL_OK) ? 1 : 0;
}

/*! 
 * @brief 从Flash读取多个Word(4字节)数据到缓冲区
 *
 * @param [in] addr       起始读取地址
 * @param [out] buff      用于存放读取数据的缓冲区
 * @param [in] word_size  要读取的Word数量
 */
static void ReadFlashWords(uint32_t addr, uint32_t *buff, uint16_t word_size)
{
	for(int i =0; i < word_size; i++)
	{
		buff[i] = *(__IO unsigned int*)(addr + 4 * i);
	}
	return;
}

/*! 
 * @brief 获取下载区（Backup/App2）的起始地址
 *
 * @return 下载区的绝对起始地址
 */
uint32_t OTA_GetDownloadAddr(void)
{
    return Application_2_Addr;
}

/*! 
 * @brief 获取下载区可用的最大尺寸限制
 *
 * @return 下载区的最大空间（字节）
 */
uint32_t OTA_GetDownloadMaxSize(void)
{
    return Application_Size;
}

/*! 
 * @brief 为接下来下载新的固件做准备，提前根据提供的大小擦除相关的下载扇区
 *
 * @param [in] image_size 即将下载的新固件的预期总大小
 *
 * @retval 1 成功
 * @retval 0 参数非法或擦除失败
 */
int OTA_PrepareDownloadArea(uint32_t image_size)
{
    if (image_size == 0 || image_size > Application_Size)
    {
        return 0;
    }
    return EraseRange(Application_2_Addr, image_size);
}

/*! 
 * @brief 下载新固件过程中，写入其中一个分块数据的方法
 *
 * @param [in] offset     相对于下载区起始地址的偏移量
 * @param [in] data       指向即将写入的有效载荷数组
 * @param [in] len        当前要写入的数据长度（不强求必须是4的整数倍，函数内部会补齐）
 *
 * @retval 1 写入成功
 * @retval 0 入参非法或写入失败
 */
int OTA_WriteDownloadChunk(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) return 0;
    if ((offset + len) > Application_Size) return 0;
    uint32_t addr = Application_2_Addr + offset;
    HAL_StatusTypeDef st = HAL_OK;
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 4)
    {
        uint32_t word = 0xFFFFFFFFU;
        uint32_t remain = len - i;
        uint32_t copy_len = (remain >= 4U) ? 4U : remain;
        memcpy(&word, data + i, copy_len);
        st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word);
        if (st != HAL_OK)
        {
            break;
        }
    }
    HAL_FLASH_Lock();
    return (st == HAL_OK) ? 1 : 0;
}

/*! 
 * @brief 读取下载区中已下载的阶段数据 (如用作对比校验等)
 *
 * @param [in] offset     相对于下载起始地址的偏移量
 * @param [out] data      读出的目标存放指针
 * @param [in] len        需要读取的长度
 */
void OTA_ReadDownloadData(uint32_t offset, uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) return;
    if ((offset + len) > Application_Size) return;
    memcpy(data, (const void *)(Application_2_Addr + offset), len);
}

/*! 
 * @brief 读取并检查当前的OTA升级元数据（包含Boot状态位信息等）
 *
 * @param [out] meta      期望读入的 OtaBootMeta 结构体指针
 * @param [in] meta_size  期望提供的大小要求 (sizeof(OtaBootMeta)) 防越界
 *
 * @retval 1 读取并且Magic效验成功
 * @retval 0 读取失败或者当前并不存在有效的OTA Meta记录
 */
int OTA_ReadBootMeta(void *meta, uint32_t meta_size)
{
    if (meta == NULL || meta_size != sizeof(OtaBootMeta)) return 0;
    memcpy(meta, (const void *)META_Flag_Addr, sizeof(OtaBootMeta));
    OtaBootMeta *m = (OtaBootMeta *)meta;
    if (m->magic != OTA_META_MAGIC) return 0;
    return 1;
}

/*! 
 * @brief 注册一个 "Pending" 的新待升级固件事件，引导 Bootloader 下次上电时搬运并更新
 *
 * @param [in] image_size 新固件的大小
 * @param [in] md5        新固件16字节 MD5 校检和特征码
 * @param [in] version    新固件版本字符信息 (可为NULL)
 * @param [in] token      可能需要的 OneNET Token 等额外鉴权数据结构 (可为NULL)
 *
 * @retval 1 成功把升级请求和相关信息写入 Meta 扇区
 * @retval 0 设置失败
 */
int OTA_SetPendingImage(uint32_t image_size, const uint8_t md5[16], const char *version, const char *token)
{
    if (md5 == NULL) return 0;
    if (image_size == 0 || image_size > Application_Size) return 0;
    OtaBootMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = OTA_META_MAGIC;
    meta.state = OTA_META_STATE_PENDING;
    meta.active_addr = Application_1_Addr;
    meta.pending_addr = Application_2_Addr;
    meta.image_size = image_size;
    memcpy(meta.md5, md5, 16);
    if (version != NULL) strncpy(meta.target_version, version, sizeof(meta.target_version) - 1U);
    if (token != NULL) strncpy(meta.ota_token, token, sizeof(meta.ota_token) - 1U);
    if (!EraseRange(META_Flag_Addr, sizeof(meta)))
    {
        return 0;
    }
    return WriteFlashWords(META_Flag_Addr, (const uint32_t *)&meta, sizeof(meta) / 4U);
}

/*! 
 * @brief 将当前的 OTA Meta 分区恢复为空闲非更新状态(升级成功后调用此清除Pending标记)
 *
 * @retval 1 清除成功
 * @retval 0 擦除或写入复位失败
 */
int OTA_ClearPendingImage(void)
{
    OtaBootMeta meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = OTA_META_MAGIC;
    meta.state = OTA_META_STATE_NONE;
    meta.active_addr = Application_1_Addr;
    meta.pending_addr = Application_2_Addr;
    if (!EraseRange(META_Flag_Addr, sizeof(meta)))
    {
        return 0;
    }
    return WriteFlashWords(META_Flag_Addr, (const uint32_t *)&meta, sizeof(meta) / 4U);
}

/*! 
 * @brief 从指定的Flash起始地址搬运代码到目标地址
 *
 * @param [in] pd_addr    源存储区起始地址（如 Application_2_Addr）
 * @param [in] app_addr   目标应用区起始地址（如 Application_1_Addr）
 * @param [in] app_size   需要搬运的固件字节大小
 */
void MoveCode(uint32_t pd_addr, uint32_t app_addr, uint32_t app_size)
{
	uint32_t len;
	uint32_t move_size = app_size;
	
	if(app_size % 1024 != 0)
	{
		move_size = (app_size / 1024) * 1024 + 1024;
	}
	
	uint32_t buff[256];
	
	printf("> Start erase app flash......\r\n");
	EraseRange(app_addr, move_size);
	printf("> Erase app flash down......\r\n");

	printf("> Start copy......\r\n");
    uint32_t total_blocks = (move_size + 1023) / 1024;
	for(int i = 0; i < total_blocks; i++)
	{
		len = 1024;
		if((i + 1) * 1024 > move_size)
		{
			len = move_size - i * 1024;
		}
		ReadFlashWords(pd_addr, buff, len / 4);
		WriteFlashWords(app_addr, buff, len / 4);
		app_addr += len;
		pd_addr += len;
	}
	printf("> Copy down......\r\n");

	printf("> Start erase src flash......\r\n");
	EraseRange(pd_addr, move_size);
	printf("> Erase src flash down......\r\n");
}
