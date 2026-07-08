#include "my_spi.h"
#include "driver/gpio.h"
#include "esp_log.h"

/* SD 卡设备句柄 (仅用作初始化标志, SDSPI 驱动内部有自己的设备) */
spi_device_handle_t MY_SD_Handle = NULL;

/**
 * @brief       SPI 总线初始化
 *
 * 只初始化 SPI 总线, 不添加设备。
 * SDSPI 驱动 (sdspi_host_init_device) 会自行添加设备到总线上。
 *
 * @param       无
 * @retval      esp_err_t
 */
esp_err_t my_spi_init(void)
{
    /* 避免重复初始化 */
    static bool bus_initialized = false;
    if (bus_initialized) {
        MY_SD_Handle = (spi_device_handle_t)1;  /* 非 NULL 标记 */
        return ESP_OK;
    }

    /* 提高 SPI 引脚驱动强度: 40MHz 高速信号需要更强的驱动力 */
    gpio_set_drive_capability(SPI_SCLK_PIN, GPIO_DRIVE_CAP_3);  /* 40mA */
    gpio_set_drive_capability(SPI_MOSI_PIN, GPIO_DRIVE_CAP_3);  /* 40mA */
    /* MISO 上拉: SD卡 DO 线需要上拉, 帮助高速信号上升沿 */
    gpio_set_pull_mode(SPI_MISO_PIN, GPIO_PULLUP_ONLY);
    ESP_LOGI("my_spi", "SPI pins: SCLK/MOSI drive=40mA, MISO pull-up");

    spi_bus_config_t buscfg = {
        .sclk_io_num     = SPI_SCLK_PIN,    /* 时钟引脚 */
        .mosi_io_num     = SPI_MOSI_PIN,    /* 主机输出从机输入引脚 */
        .miso_io_num     = SPI_MISO_PIN,    /* 主机输入从机输出引脚 */
        .quadwp_io_num   = -1,              /* 用于Quad模式的WP引脚,未使用时设置为-1 */
        .quadhd_io_num   = -1,              /* 用于Quad模式的HD引脚,未使用时设置为-1 */
        .max_transfer_sz = 320 * 240 * sizeof(uint16_t),   /* 最大传输大小(整屏(RGB565格式)) */
    };
    /* 初始化SPI总线 */
    ESP_ERROR_CHECK(spi_bus_initialize(MY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* SDSPI 驱动会自行通过 sdspi_host_init_device() 添加设备,
     * 这里的 MY_SD_Handle 仅用作初始化标志, 不参与实际 SPI 通信 */
    MY_SD_Handle = (spi_device_handle_t)1;
    bus_initialized = true;

    return ESP_OK;
}
