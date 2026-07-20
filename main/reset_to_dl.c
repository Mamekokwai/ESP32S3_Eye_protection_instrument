#include "reset_to_dl.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"

void reboot_to_download(void)
{
    ESP_LOGI("dl", "Rebooting to download mode...");

    /* IO0 拉低 → ROM bootloader 检测到后进入下载模式 */
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(GPIO_NUM_0),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_NUM_0, 0);
    gpio_hold_en(GPIO_NUM_0);
    gpio_deep_sleep_hold_en();  /* 确保 hold 在复位时生效 */

    for (volatile int i = 0; i < 10000; i++) __asm__("nop");
    esp_restart();
}
