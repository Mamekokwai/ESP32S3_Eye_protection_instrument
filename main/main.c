#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "beep.h"
#include "audio.h"

void app_main(void)
{
    ESP_LOGI("APP", "init beep");
    beep_init();

    ESP_LOGI("APP", "init audio (ES8311)");
    if (audio_init() != ESP_OK) {
        ESP_LOGE("APP", "audio init failed");
        // 音频不可用也不阻塞，继续运行
    }

    while (1)
    {
        ESP_LOGI("APP", "beep on");
        beep_on();
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI("APP", "beep off");
        beep_off();
        vTaskDelay(pdMS_TO_TICKS(900));
    }
}
