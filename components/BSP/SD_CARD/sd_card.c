#include "sd_card.h"

#include "sd_card_config.h"

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#define TAG "sd_card"

static sdmmc_card_t *s_card;
static esp_err_t s_mount_result = ESP_ERR_INVALID_STATE;

#if SD_CARD_PROTOCOL == SD_CARD_PROTOCOL_SPI
static bool s_spi_bus_initialized;

static esp_err_t init_spi_bus(void)
{
    if (s_spi_bus_initialized)
        return ESP_OK;

    gpio_set_drive_capability(SD_CARD_CLK_GPIO, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SD_CARD_CMD_MOSI_GPIO, GPIO_DRIVE_CAP_3);
    gpio_set_pull_mode(SD_CARD_D0_MISO_GPIO, GPIO_PULLUP_ONLY);

    const spi_bus_config_t bus_config = {
        .sclk_io_num = SD_CARD_CLK_GPIO,
        .mosi_io_num = SD_CARD_CMD_MOSI_GPIO,
        .miso_io_num = SD_CARD_D0_MISO_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 64 * 1024,
    };
    esp_err_t err = spi_bus_initialize(
        SD_CARD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err == ESP_OK)
        s_spi_bus_initialized = true;
    return err;
}
#endif

static esp_err_t unmount_if_needed(void)
{
    if (!sd_card_is_mounted())
        return ESP_OK;

    esp_err_t err = esp_vfs_fat_sdcard_unmount(
        SD_CARD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mount_result = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Unmount failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t sd_card_mount(void)
{
    esp_err_t err = unmount_if_needed();
    if (err != ESP_OK)
        return err;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };

#if SD_CARD_PROTOCOL == SD_CARD_PROTOCOL_SPI
    err = init_spi_bus();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI2 init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SD_CARD_SPI_FREQ_KHZ;

    const sdspi_device_config_t slot_config = {
        .host_id = SD_CARD_SPI_HOST,
        .gpio_cs = SD_CARD_CS_GPIO,
        .gpio_cd = GPIO_NUM_NC,
        .gpio_wp = GPIO_NUM_NC,
        .gpio_int = GPIO_NUM_NC,
        .duty_cycle_pos = 128,
    };
    ESP_LOGI(TAG, "Protocol: SPI, target freq: %d kHz",
             host.max_freq_khz);
    s_mount_result = esp_vfs_fat_sdspi_mount(
        SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);

#elif SD_CARD_PROTOCOL == SD_CARD_PROTOCOL_SDMMC_1BIT
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SD_CARD_SDMMC_FREQ_KHZ;
    host.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_CARD_CLK_GPIO;
    slot_config.cmd = SD_CARD_CMD_MOSI_GPIO;
    slot_config.d0 = SD_CARD_D0_MISO_GPIO;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Protocol: SDMMC 1-bit, target freq: %d kHz",
             host.max_freq_khz);
    s_mount_result = esp_vfs_fat_sdmmc_mount(
        SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);

#else
#error "Unsupported SD_CARD_PROTOCOL"
#endif

    if (s_mount_result != ESP_OK)
    {
        s_card = NULL;
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(s_mount_result));
        return s_mount_result;
    }

    ESP_LOGI(TAG, "Mounted: actual=%d kHz, card_limit=%lu kHz",
             s_card->real_freq_khz,
             (unsigned long)s_card->max_freq_khz);
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_mount_result == ESP_OK && s_card != NULL;
}

sdmmc_card_t *sd_card_handle(void)
{
    return sd_card_is_mounted() ? s_card : NULL;
}
