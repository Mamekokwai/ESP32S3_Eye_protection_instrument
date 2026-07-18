#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
/* YT06: beep removed */
#include "audio.h"
#include "my_spi.h"
#include "spilcd.h"
#include "spi_sd.h"
#include "video_player.h"
#include "raw_player.h"
#include "flash_player.h"
#include <dirent.h>
#include <sys/stat.h>
// #include "mp3_decoder.h"

// 引用 EMBED_FILES 内嵌的 canon.pcm
extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t canon_pcm_end[] asm("_binary_canon_pcm_end");

// 引用内嵌的 MP3 文件（把 xxx.mp3 加到 CMakeLists.txt 的 EMBED_FILES 后即可）
// extern const uint8_t xxx_mp3_start[] asm("_binary_xxx_mp3_start");
// extern const uint8_t xxx_mp3_end[]   asm("_binary_xxx_mp3_end");

void app_main(void)
{
    /* beep removed */
    /* beep_init removed */

    ESP_LOGI("APP", "init audio (ES8311)");
    if (audio_init() != ESP_OK)
    {
        ESP_LOGE("APP", "audio init failed");
    }

    ESP_LOGI("APP", "init SPI bus");
    my_spi_init();

    ESP_LOGI("APP", "init LCD");
    spilcd_init();

    // 显示一行文字（320x240 横屏，size=16 字体，居中）
    // spilcd_show_string(40, 100, 280, 130, 16, "Hello ESP32-S3!", RED);

    // 播放 PCM
    // size_t canon_len = canon_pcm_end - canon_pcm_start;
    // ESP_LOGI("APP", "playing canon.pcm (%d bytes)", canon_len);
    // audio_set_volume(80);
    // audio_play(canon_pcm_start, canon_len);

    // 尝试挂载 SD 卡并播放视频
    ESP_LOGI("APP", "mounting SD card...");
    if (sd_spi_init() == ESP_OK)
    {
        // 搜索: 优先 .raw (无需解码), 其次 .avi
        DIR *dir = opendir("/0:");
        char play_path[272] = {0};
        bool is_raw = false;
        if (dir != NULL)
        {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL)
            {
                const char *name = entry->d_name;
                size_t len = strlen(name);
                // 优先匹配 .raw
                if (len > 4 && strcasecmp(name + len - 4, ".raw") == 0)
                {
                    snprintf(play_path, sizeof(play_path), "/0:/%s", name);
                    is_raw = true;
                    break;
                }
                // 其次匹配 .avi
                if (len > 4 && play_path[0] == 0 && strcasecmp(name + len - 4, ".avi") == 0)
                {
                    snprintf(play_path, sizeof(play_path), "/0:/%s", name);
                }
            }
            closedir(dir);
        }

        if (play_path[0] != 0)
        {
            ESP_LOGI("APP", "Found: %s (%s)", play_path, is_raw ? "raw" : "avi");
            spilcd_show_string(0, 0, 320, 16, 16, play_path, BLACK);
            if (is_raw) {
                raw_player_play(play_path);
            } else {
                video_player_play(play_path);  /* 内部无缝循环 */
            }
        }
        else
        {
            ESP_LOGW("APP", "No video files (.raw/.avi) found");
            spilcd_show_string(10, 200, 300, 230, 16, "No video files", RED);
        }
    }
    else
    {
        ESP_LOGW("APP", "SD card mount failed, trying Flash...");
        esp_err_t flash_ret = flash_video_play();
        if (flash_ret != ESP_OK) {
            spilcd_show_string(10, 200, 300, 230, 16, "No SD card", RED);
        }
    }

    // 播放 MP3 示例（取消注释并把 .mp3 文件加到 EMBED_FILES）:
    // size_t mp3_len = xxx_mp3_end - xxx_mp3_start;
    // mp3_play(xxx_mp3_start, mp3_len);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
