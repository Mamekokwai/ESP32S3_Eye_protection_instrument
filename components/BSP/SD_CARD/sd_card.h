#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SD_CARD_MOUNT_POINT "/0:"

esp_err_t sd_card_mount(void);
/** 检查已挂载卡是否仍在线；拔卡时返回错误。 */
esp_err_t sd_card_probe(void);
/** 卸载文件系统并清除驱动状态，支持拔卡后的重新挂载。 */
esp_err_t sd_card_unmount(void);
bool sd_card_is_mounted(void);
sdmmc_card_t *sd_card_handle(void);

#endif
