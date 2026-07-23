#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SD_CARD_MOUNT_POINT "/0:"

esp_err_t sd_card_mount(void);
bool sd_card_is_mounted(void);
sdmmc_card_t *sd_card_handle(void);

#endif
