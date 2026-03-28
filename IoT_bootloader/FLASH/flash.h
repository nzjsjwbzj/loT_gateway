#ifndef _FLASH_H
#define _FLASH_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "stm32f429xx.h"
#include "core_cm4.h"

#include "boot.h"

#define BootLoader_addr      0x08000000U
#define BootLoader_Size      0x00010000U
#define META_Flag_Addr       0x08100000U
#define Application_1_Addr   0x08020000U
#define Application_2_Addr   0x08120000U
#define Application_Size     0x00080000U
#define OTA_META_MAGIC       0x54414F31U
#define OTA_META_STATE_NONE  0U
#define OTA_META_STATE_PENDING 1U


typedef struct
{
    uint32_t magic;
    uint32_t state;
    uint32_t active_addr;
    uint32_t pending_addr;
    uint32_t image_size;
    uint8_t md5[16];
    char target_version[16];
    char ota_token[40];
    uint32_t reserved[4];
} OtaBootMeta;
uint32_t OTA_GetDownloadAddr(void);
uint32_t OTA_GetDownloadMaxSize(void);
int OTA_PrepareDownloadArea(uint32_t image_size);
int OTA_WriteDownloadChunk(uint32_t offset, const uint8_t *data, uint32_t len);
void OTA_ReadDownloadData(uint32_t offset, uint8_t *data, uint32_t len);
int OTA_ReadBootMeta(void *meta, uint32_t meta_size);
int OTA_SetPendingImage(uint32_t image_size, const uint8_t md5[16], const char *version, const char *token);
int OTA_ClearPendingImage(void);
void MoveCode(unsigned int src_addr, unsigned int des_addr, unsigned int byte_size);

#endif
