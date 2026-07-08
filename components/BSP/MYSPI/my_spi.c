#include "my_spi.h"
#include "spi_sd.h"       /* SD_PROTOCOL 宏 */
#include "driver/gpio.h"
#include "esp_log.h"

/* SD 卡设备句柄 (仅用作初始化标志) */
spi_device_handle_t MY_SD_Handle = NULL;

/**
 * @brief       SPI 总线初始化 (LCD 用)
 *
 *   SPI 模式: SDSPI 驱动自行添加 SD 设备到总线
 *   SDMMC 模式: SPI 总线仅供 LCD, SDMMC 独立使用 (同组引脚, 分时复用)
 *
 * @param       无
 * @retval      esp_err_t
 */
esp_err_t my_spi_init(void)
{
    static bool bus_initialized = false;
    if (bus_initialized) {
        MY_SD_Handle = (spi_device_handle_t)1;
        return ESP_OK;
    }

#if SD_PROTOCOL == SD_PROTOCOL_SPI
    /* SPI 模式: 提高驱动强度改善信号质量 */
    gpio_set_drive_capability(SPI_SCLK_PIN, GPIO_DRIVE_CAP_3);  /* 40mA */
    gpio_set_drive_capability(SPI_MOSI_PIN, GPIO_DRIVE_CAP_3);  /* 40mA */
    gpio_set_pull_mode(SPI_MISO_PIN, GPIO_PULLUP_ONLY);
    ESP_LOGI("my_spi", "SPI pins: SCLK/MOSI drive=40mA, MISO pull-up");
#else
    /* SDMMC 模式: SPI 仅供 LCD, 不需驱动强度调整 */
    ESP_LOGI("my_spi", "SPI bus for LCD only (SDMMC handles TF card)");
#endif

    spi_bus_config_t buscfg = {
        .sclk_io_num     = SPI_SCLK_PIN,
        .mosi_io_num     = SPI_MOSI_PIN,
        .miso_io_num     = SPI_MISO_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 320 * 240 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(MY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    MY_SD_Handle = (spi_device_handle_t)1;
    bus_initialized = true;
    return ESP_OK;
}
