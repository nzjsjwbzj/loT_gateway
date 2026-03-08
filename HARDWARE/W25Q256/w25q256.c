#include "w25q256.h"
#include "delay.h"
#include "stm32f4xx_hal_spi.h"

#define W25Q256_CS_GPIO_PORT GPIOF
#define W25Q256_CS_GPIO_PIN  GPIO_PIN_6

#define W25Q256_CMD_RDID     0x9F
#define W25Q256_CMD_READ     0x03
#define W25Q256_CMD_WREN     0x06
#define W25Q256_CMD_RDSR1    0x05
#define W25Q256_CMD_PP       0x02
#define W25Q256_CMD_SE       0x20

static SPI_HandleTypeDef hspi5;

static void W25Q256_CS_Low(void)
{
    HAL_GPIO_WritePin(W25Q256_CS_GPIO_PORT, W25Q256_CS_GPIO_PIN, GPIO_PIN_RESET);
}

static void W25Q256_CS_High(void)
{
    HAL_GPIO_WritePin(W25Q256_CS_GPIO_PORT, W25Q256_CS_GPIO_PIN, GPIO_PIN_SET);
}

static void W25Q256_WriteEnable(void)
{
    uint8_t cmd = W25Q256_CMD_WREN;
    W25Q256_CS_Low();
    HAL_SPI_Transmit(&hspi5, &cmd, 1, 100);
    W25Q256_CS_High();
}

static uint8_t W25Q256_ReadSR1(void)
{
    uint8_t cmd = W25Q256_CMD_RDSR1;
    uint8_t val = 0;
    W25Q256_CS_Low();
    HAL_SPI_Transmit(&hspi5, &cmd, 1, 100);
    HAL_SPI_Receive(&hspi5, &val, 1, 100);
    W25Q256_CS_High();
    return val;
}

static void W25Q256_WaitBusy(void)
{
    while (W25Q256_ReadSR1() & 0x01)
    {
        delay_ms(1);
    }
}

static void W25Q256_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio_init_struct;

    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_SPI5_CLK_ENABLE();

    gpio_init_struct.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9;
    gpio_init_struct.Mode = GPIO_MODE_AF_PP;
    gpio_init_struct.Pull = GPIO_NOPULL;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init_struct.Alternate = GPIO_AF5_SPI5;
    HAL_GPIO_Init(GPIOF, &gpio_init_struct);

    gpio_init_struct.Pin = W25Q256_CS_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init_struct.Pull = GPIO_NOPULL;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(W25Q256_CS_GPIO_PORT, &gpio_init_struct);

    W25Q256_CS_High();
}

uint8_t W25Q256_Init(void)
{
    W25Q256_GPIO_Init();

    hspi5.Instance = SPI5;
    hspi5.Init.Mode = SPI_MODE_MASTER;
    hspi5.Init.Direction = SPI_DIRECTION_2LINES;
    hspi5.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi5.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi5.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi5.Init.NSS = SPI_NSS_SOFT;
    hspi5.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi5.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi5.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi5.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi5.Init.CRCPolynomial = 7;

    if (HAL_SPI_Init(&hspi5) != HAL_OK)
    {
        return 1;
    }

    return 0;
}

uint16_t W25Q256_ReadID(void)
{
    uint8_t cmd = W25Q256_CMD_RDID;
    uint8_t id[3] = {0};
    W25Q256_CS_Low();
    HAL_SPI_Transmit(&hspi5, &cmd, 1, 100);
    HAL_SPI_Receive(&hspi5, id, 3, 100);
    W25Q256_CS_High();
    return (uint16_t)((id[0] << 8) | id[1]);
}

void W25Q256_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint8_t cmd[4];
    cmd[0] = W25Q256_CMD_READ;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);
    W25Q256_CS_Low();
    HAL_SPI_Transmit(&hspi5, cmd, 4, 100);
    HAL_SPI_Receive(&hspi5, buf, len, 1000);
    W25Q256_CS_High();
}

void W25Q256_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t offset = 0;
    while (offset < len)
    {
        uint32_t page_off = addr & 0xFF;
        uint32_t page_left = 256 - page_off;
        uint32_t chunk = (len - offset < page_left) ? (len - offset) : page_left;

        W25Q256_WriteEnable();

        uint8_t cmd[4];
        cmd[0] = W25Q256_CMD_PP;
        cmd[1] = (uint8_t)(addr >> 16);
        cmd[2] = (uint8_t)(addr >> 8);
        cmd[3] = (uint8_t)(addr);

        W25Q256_CS_Low();
        HAL_SPI_Transmit(&hspi5, cmd, 4, 100);
        HAL_SPI_Transmit(&hspi5, (uint8_t *)(buf + offset), chunk, 1000);
        W25Q256_CS_High();

        W25Q256_WaitBusy();

        addr += chunk;
        offset += chunk;
    }
}

void W25Q256_EraseSector(uint32_t addr)
{
    W25Q256_WriteEnable();

    uint8_t cmd[4];
    cmd[0] = W25Q256_CMD_SE;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    W25Q256_CS_Low();
    HAL_SPI_Transmit(&hspi5, cmd, 4, 100);
    W25Q256_CS_High();

    W25Q256_WaitBusy();
}
