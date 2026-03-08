#ifndef __W25Q256_H
#define __W25Q256_H
#include "sys.h"

uint8_t W25Q256_Init(void);
uint16_t W25Q256_ReadID(void);
void W25Q256_Read(uint32_t addr, uint8_t *buf, uint32_t len);
void W25Q256_Write(uint32_t addr, const uint8_t *buf, uint32_t len);
void W25Q256_EraseSector(uint32_t addr);

#endif
