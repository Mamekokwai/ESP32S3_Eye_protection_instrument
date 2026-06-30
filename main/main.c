#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "beep.h"
#include "audio.h"
// #include "mp3_decoder.h"

// 引用 EMBED_FILES 内嵌的 canon.pcm
extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t canon_pcm_end[] asm("_binary_canon_pcm_end");

// 引用内嵌的 MP3 文件（把 xxx.mp3 加到 CMakeLists.txt 的 EMBED_FILES 后即可）
// extern const uint8_t xxx_mp3_start[] asm("_binary_xxx_mp3_start");
// extern const uint8_t xxx_mp3_end[]   asm("_binary_xxx_mp3_end");

void app_main(void)
{
    ESP_LOGI("APP", "init beep");
    beep_init();

    ESP_LOGI("APP", "init audio (ES8311)");
    if (audio_init() != ESP_OK)
    {
        ESP_LOGE("APP", "audio init failed");
    }

    // 播放 PCM
    size_t canon_len = canon_pcm_end - canon_pcm_start;
    ESP_LOGI("APP", "playing canon.pcm (%d bytes)", canon_len);
    audio_set_volume(80);
    audio_play(canon_pcm_start, canon_len);

    // 播放 MP3 示例（取消注释并把 .mp3 文件加到 EMBED_FILES）:
    // size_t mp3_len = xxx_mp3_end - xxx_mp3_start;
    // mp3_play(xxx_mp3_start, mp3_len);

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
