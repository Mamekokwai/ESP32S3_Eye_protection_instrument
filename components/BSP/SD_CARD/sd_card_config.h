#ifndef SD_CARD_CONFIG_H
#define SD_CARD_CONFIG_H

#include "driver/gpio.h"
#include "driver/spi_common.h"

/* TF 卡协议：当前量产配置为 SDMMC 1-bit 40MHz。 */
#define SD_CARD_PROTOCOL_SPI          1
#define SD_CARD_PROTOCOL_SDMMC_1BIT   2
#ifndef SD_PROTOCOL_SPI
#define SD_PROTOCOL_SPI               SD_CARD_PROTOCOL_SPI
#endif
#ifndef SD_PROTOCOL_SDMMC_1BIT
#define SD_PROTOCOL_SDMMC_1BIT        SD_CARD_PROTOCOL_SDMMC_1BIT
#endif
#ifndef SD_CARD_PROTOCOL
#ifdef SD_PROTOCOL
#define SD_CARD_PROTOCOL              SD_PROTOCOL
#else
#define SD_CARD_PROTOCOL              SD_CARD_PROTOCOL_SDMMC_1BIT
#endif
#endif

/* SPI 回退配置和 SDMMC 当前配置。 */
#ifndef SD_CARD_SPI_FREQ_KHZ
#define SD_CARD_SPI_FREQ_KHZ          20000
#endif
#ifndef SD_CARD_SDMMC_FREQ_KHZ
#ifdef SDMMC_1BIT_FREQ_KHZ
#define SD_CARD_SDMMC_FREQ_KHZ        SDMMC_1BIT_FREQ_KHZ
#else
#define SD_CARD_SDMMC_FREQ_KHZ        40000
#endif
#endif
#define SD_CARD_SPI_HOST              SPI2_HOST

/* SPI 与 SDMMC 1-bit 复用同一组 PCB 走线。 */
#define SD_CARD_CLK_GPIO              GPIO_NUM_21
#define SD_CARD_CMD_MOSI_GPIO         GPIO_NUM_47
#define SD_CARD_D0_MISO_GPIO          GPIO_NUM_14
#define SD_CARD_CS_GPIO               GPIO_NUM_0

#endif
