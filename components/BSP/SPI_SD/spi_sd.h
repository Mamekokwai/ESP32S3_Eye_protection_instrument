#ifndef SPI_SD_H
#define SPI_SD_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "sd_card.h"
#include "sd_card_config.h"

/*
 * Legacy compatibility API.
 * New code should include sd_card.h and use the sd_card_* names directly.
 */
#ifndef SD_PROTOCOL
#define SD_PROTOCOL SD_CARD_PROTOCOL
#endif
#ifndef SDMMC_1BIT_FREQ_KHZ
#define SDMMC_1BIT_FREQ_KHZ SD_CARD_SDMMC_FREQ_KHZ
#endif
#define SD_NUM_CS SD_CARD_CS_GPIO
#define MOUNT_POINT SD_CARD_MOUNT_POINT

esp_err_t sd_spi_init(void);
bool sd_spi_is_mounted(void);
void sd_get_fatfs_usage(size_t *out_total_kb, size_t *out_free_kb);

#endif
