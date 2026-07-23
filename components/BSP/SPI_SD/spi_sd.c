#include "spi_sd.h"

sdmmc_card_t *card;                     /* SD / MMC卡结构 */
const char mount_point[] = MOUNT_POINT; /* 挂载点/根目录 */
esp_err_t ret = ESP_OK;
esp_err_t mount_ret = ESP_FAIL;

/**
 * @brief       SD卡初始化 (SPI 或 SDMMC 1-bit)
 * @param       无
 * @retval      esp_err_t
 */
esp_err_t sd_spi_init(void)
{
    ret = ESP_OK;

    if (MY_SD_Handle != NULL) /* 再一次挂载或者初始化SD卡 */
    {
        if (mount_ret == ESP_OK)
        {
            esp_vfs_fat_sdcard_unmount(mount_point, card); /* 取消挂载 */
            mount_ret = ESP_FAIL;
        }
    }
    else if (MY_SD_Handle == NULL) /* 未初始化驱动 */
    {
        my_spi_init(); /* 初始化SPI总线(LCD需要) */
    }

    /* 文件系统挂载配置 */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,                    /* 如果挂载失败：true会重新分区和格式化/false不会重新分区和格式化 */
        .max_files = 5,                                     /* 打开文件最大数量 */
        .allocation_unit_size = 64 * 1024 * sizeof(uint8_t) /* 硬盘分区簇的大小 64KB */
    };

#if SD_PROTOCOL == SD_PROTOCOL_SPI
    /* ================================================================
     *  SPI 模式
     *  引脚: SCLK=21, MOSI=47, MISO=14, CS=0 (共享 SPI2_HOST)
     *  速度: 20MHz → 读写 ~1.6 MB/s (CMD18多块读)
     * ================================================================ */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    // host.max_freq_khz = SDMMC_FREQ_DEFAULT;  /* 20MHz — 模块硬件上限 */
    host.max_freq_khz = 40000; /* SPI 40 MHz 测试 */
    ESP_LOGI("spi_sd", "Protocol: SPI, target freq: %d kHz", (int)host.max_freq_khz);

    sdspi_device_config_t slot_config = {0};
    slot_config.host_id = host.slot;
    slot_config.gpio_cs = SD_NUM_CS;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    slot_config.gpio_int = GPIO_NUM_NC;
    slot_config.duty_cycle_pos = 128; /* 50/50 占空比 */

    mount_ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

#elif SD_PROTOCOL == SD_PROTOCOL_SDMMC_1BIT
    /* ================================================================
     *  SDMMC 1-bit 模式
     *  引脚: CLK=21, CMD=47, D0=14 (和 SPI 共用, 不需改硬件)
     *  速度: 40MHz → 读写 ~5 MB/s
     *  注意: 初始化顺序必须先 SPI(LCD) 再 SDMMC, 否则引脚冲突
     * ================================================================ */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; /* 40MHz → 先试高速 */
    host.flags = SDMMC_HOST_FLAG_1BIT;
    ESP_LOGI("spi_sd", "Protocol: SDMMC 1-bit, target freq: %d kHz", (int)host.max_freq_khz);

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = GPIO_NUM_21; /* 和 SPI SCLK 同引脚 */
    slot_config.cmd = GPIO_NUM_47; /* 和 SPI MOSI 同引脚 */
    slot_config.d0 = GPIO_NUM_14;  /* 和 SPI MISO 同引脚 */
    slot_config.d1 = GPIO_NUM_NC;  /* 1-bit 模式不需 D1 */
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 1;

    mount_ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

#else
#error "SD_PROTOCOL must be SD_PROTOCOL_SPI or SD_PROTOCOL_SDMMC_1BIT"
#endif

    if (mount_ret == ESP_OK && card != NULL)
    {
        ESP_LOGI("spi_sd", "Actual freq: %d kHz, card limit: %lu kHz",
                 card->real_freq_khz,
                 (unsigned long)card->max_freq_khz);
    }

    ret |= mount_ret;
    vTaskDelay(pdMS_TO_TICKS(10));
    return ret;
}

bool sd_spi_is_mounted(void)
{
    return mount_ret == ESP_OK && card != NULL;
}

/**
 * @brief       获取SD卡相关信息
 * @param       out_total_bytes：总大小
 * @param       out_free_bytes：剩余大小
 * @retval      无
 */
void sd_get_fatfs_usage(size_t *out_total_bytes, size_t *out_free_bytes)
{
    FATFS *fs;
    size_t free_clusters;
    int res = f_getfree("0:", (DWORD *)&free_clusters, &fs);
    assert(res == FR_OK);
    size_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    size_t free_sectors = free_clusters * fs->csize;

    size_t sd_total = total_sectors / 1024;
    size_t sd_total_KB = sd_total * fs->ssize;
    size_t sd_free = free_sectors / 1024;
    size_t sd_free_KB = sd_free * fs->ssize;

    /* 假设总大小小于4GiB，对于SPI Flash应该为true */
    if (out_total_bytes != NULL)
    {
        *out_total_bytes = sd_total_KB;
    }

    if (out_free_bytes != NULL)
    {
        *out_free_bytes = sd_free_KB;
    }
}
