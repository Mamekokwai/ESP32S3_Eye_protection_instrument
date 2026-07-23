#ifndef __SPI_SD_H
#define __SPI_SD_H

#include <unistd.h>
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "my_spi.h"
#include <stdbool.h>

/* ============================================================
 *  TF 卡协议选择
 *
 *   SD_PROTOCOL_SPI        — SPI 模式 (当前, 20MHz, ~1.6MB/s)
 *   SD_PROTOCOL_SDMMC_1BIT — SDMMC 1-bit (40MHz, ~5MB/s, 不改硬件)
 *
 *   SDMMC 1-bit 和 SPI 共用同一组引脚 (CLK=21, CMD/MOSI=47, D0/MISO=14)
 *   SPI 总线仍给 LCD 使用, SDMMC 独立访问 TF 卡, 两者不冲突
 * ============================================================ */
#define SD_PROTOCOL_SPI 1
#define SD_PROTOCOL_SDMMC_1BIT 2

#ifndef SD_PROTOCOL
#define SD_PROTOCOL SD_PROTOCOL_SPI
#endif

/* 引脚定义 */
#define SD_NUM_CS GPIO_NUM_0
#define MOUNT_POINT "/0:"

/* 函数声明 */
esp_err_t sd_spi_init(void); /* SD卡初始化 (兼容旧名称) */
bool sd_spi_is_mounted(void);
void sd_get_fatfs_usage(size_t *out_total_bytes, size_t *out_free_bytes);
#endif
