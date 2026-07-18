#include "reset_to_dl.h"
#include "driver/rtc_io.h"
#include "esp_system.h"
#include "esp_log.h"

void reboot_to_download(void)
{
    ESP_LOGI("dl", "Rebooting to download mode...");

    /* IO0 (BOOT): 拉低 → 进入下载模式 */
    rtc_gpio_init(GPIO_NUM_0);
    rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_0, 0);
    rtc_gpio_hold_en(GPIO_NUM_0);

    /* IO46 (USB D+ / 烧录 strapping): 拉低 */
    rtc_gpio_init(GPIO_NUM_46);
    rtc_gpio_set_direction(GPIO_NUM_46, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_46, 0);
    rtc_gpio_hold_en(GPIO_NUM_46);

    /* 延时确保 hold 生效, 然后复位 */
    for (volatile int i = 0; i < 1000; i++) __asm__("nop");
    esp_restart();
}
