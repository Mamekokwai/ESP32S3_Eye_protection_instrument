#ifndef __SPI_SD_H
#define __SPI_SD_H

#include <unistd.h>
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "my_spi.h"


/* 引脚定义 */
#define SD_NUM_CS       GPIO_NUM_0
#define MOUNT_POINT     "/0:"

/* 函数声明 */
esp_err_t sd_spi_init(void);    /* SD卡初始化 */
void sd_get_fatfs_usage(size_t *out_total_bytes, size_t *out_free_bytes);
#endif
