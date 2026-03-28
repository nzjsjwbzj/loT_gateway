#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>

/* ================== 片内 Flash 内存分区地址与大小映射 ================== */
/** BootLoader 起始地址，STM32F4 默认内部 Flash 基址 */
#define BootLoader_addr      0x08000000U
/** BootLoader 预留空间大小 (64KB - 扇区0~3) */
#define BootLoader_Size      0x00010000U

/** 
 * OTA 固件状态元数据区起始地址 
 * 此处用于记录当前运行哪个分区、是否有新固件等待刷入等信息
 */
#define META_Flag_Addr       0x08100000U

/** 应用程序分区 1：主运行区存放首地址 (常驻运行版本) */
#define Application_1_Addr   0x08020000U
/** 应用程序分区 2：OTA固件下载缓冲区首地址 (用于断点续传/后台下载新版本) */
#define Application_2_Addr   0x08120000U

/** 单个应用程序(Application)预留的 Flash 空间大小配额 (512KB) */
#define Application_Size     0x00080000U

/* ========================== OTA 元数据相关宏 ========================== */
/** OTA 元数据魔数："1OAT" (小端模式对应 HEX)，用于校验元数据结构的合法性 */
#define OTA_META_MAGIC       0x54414F31U

/** 元数据状态：没有需要处理的新固件任务 */
#define OTA_META_STATE_NONE    0U
/** 元数据状态：有完整的新固件已经下载完毕，正在等待 BootLoader 搬运/升级 */
#define OTA_META_STATE_PENDING 1U

/**
 * @brief BootLoader 与 APP 交互约定的【OTA 升级控制块】元数据结构体
 *        保存在 META_Flag_Addr (0x08100000)
 */
typedef struct
{
    uint32_t magic;           /**< 魔数标志，合法方说明此处记录有效 (OTA_META_MAGIC) */
    uint32_t state;           /**< 当前的 OTA 更新状态 (NONE 或 PENDING) */
    uint32_t active_addr;     /**< 当前正在运行的 APP 地址 (通常为 Application_1_Addr) */
    uint32_t pending_addr;    /**< 暂存的新固件数据地址 (通常为 Application_2_Addr) */
    uint32_t image_size;      /**< 已经下载完成的待更新固件总大小 (字节) */
    uint8_t  md5[16];         /**< 待更新固件的 16 字节完整 MD5 校验哈希值 */
    char     target_version[16]; /**< 将要升级到的目标版本号字符串，如 "V1.0.1" */
    char     ota_token[40];   /**< 升级任务令牌/流水号，用于对接云端回执和溯源 */
    uint32_t reserved[4];     /**< 保留字段，凑齐字节对齐以及兼顾未来功能扩展 */
} OtaBootMeta;

/* =============================== OTA 操作 API =============================== */

/**
 * @brief  获取当前允许下载存放 Ota 固件的起始写入地址 
 * @return 后台下载区首地址 Application_2_Addr
 */
uint32_t OTA_GetDownloadAddr(void);

/**
 * @brief  获取系统分配给新固件下载区的最大可用空间
 * @return 允许存放的最大字节数 Application_Size
 */
uint32_t OTA_GetDownloadMaxSize(void);

/**
 * @brief  准备升级下载区域
 *         通常在开启 OTA 下载前调用，用于对比文件大小并擦除必要的后场片内 Flash 扇区
 * @param  image_size: 预计要下载的固件大小
 * @return 0 成功；非0 失败 (通常代表空间不足或者擦除失败)
 */
int OTA_PrepareDownloadArea(uint32_t image_size);

/**
 * @brief  向后场下载区写入一段接收到的 OTA 固件数据 (用于断点续传/分包接收)
 * @param  offset: 基于下载区起始地址的当前数据段偏移字节量
 * @param  data:   指向本次收到的固件分包数据
 * @param  len:    本次写入的数据长度
 * @return 0 成功；非 0 失败
 */
int OTA_WriteDownloadChunk(uint32_t offset, const uint8_t *data, uint32_t len);

/**
 * @brief  从设定的地址偏移处，提取对应长度的后台下载区固件数据
 *         常用于下载完毕后读取数据供本地 MD5 累加进行完整性校验
 * @param  offset: 起始偏移位置
 * @param  data:   数据读出后存放的目标缓冲区
 * @param  len:    需要读取的字节总数
 */
void OTA_ReadDownloadData(uint32_t offset, uint8_t *data, uint32_t len);

/**
 * @brief  读取保存在指定位置的 OTA 状态元数据
 * @param  meta:      用来存放查到的元数据的结构体指针
 * @param  meta_size: 本次需要读取的大小，通常即 sizeof(OtaBootMeta)
 * @return 0 成功；非 0 失败
 */
int OTA_ReadBootMeta(void *meta, uint32_t meta_size);

/**
 * @brief  配置并保存一条挂起的升级任务，让 Bootloader 重启后实施搬运
 * @param  image_size: 已下载的实际有效固件大小
 * @param  md5:        该新固件在下载完毕后本地计算出的 md5 或云端下发的预期 md5 值
 * @param  version:    新固件对应的版本号文本
 * @param  token:      升级任务的记录标识信标（回执用）
 * @return 0 成功；非 0 失败
 */
int OTA_SetPendingImage(uint32_t image_size, const uint8_t md5[16], const char *version, const char *token);

/**
 * @brief  清除现有的未决(挂起) OTA 升级影像标志
 *         通常于成功完成升级后或者发现下发版 md5 严重错乱时调用，恢复到 NONE 正常运行态
 * @return 0 成功；非 0 失败
 */
int OTA_ClearPendingImage(void);

/**
 * @brief  片内 Flash 数据批量深度物理迁移函数 (Bootloader 底层用)
 *         将数据从源地址提取并写入目的地址
 * @param  src_addr:   源数据绝对基地址
 * @param  des_addr:   目的存放绝对基地址
 * @param  byte_size:  搬运长
 */
void MoveCode(unsigned int src_addr, unsigned int des_addr, unsigned int byte_size);

#endif
