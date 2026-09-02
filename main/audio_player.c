/**
 * @brief  SD 卡 PCM/MP3 音频播放器 (tick 化)
 *
 * PCM 直接分块输出；MP3 流式解码为 16-bit PCM 后输出到 ES8311。
 */
#include "audio_player.h"
#include "audio.h"
#include "media_catalog.h"
#include "mp3_decoder_wrapper.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "audio_player"
#define CHUNK_SIZE  2048  /* 每次 tick 读取的字节数 */
#define MP3_INPUT_SIZE 4096
#define MP3_PCM_BYTES  4608
#define AUDIO_TASK_STACK_SIZE 6144
#define AUDIO_TASK_PRIORITY   5
#define AUDIO_TASK_CORE       1

typedef enum {
    AUDIO_FMT_PCM,
    AUDIO_FMT_MP3,
} audio_format_t;

typedef struct {
    bool initialized;
    bool muted;
    int  volume;       /* 0-100 */

    FIL  file;
    char filename[MEDIA_CATALOG_PATH_MAX + 4];
    FSIZE_t file_size;
    FSIZE_t pos;

    uint32_t chunks_done;
    int64_t  start_time;
    bool     loop;       /* 循环播放 */
    audio_format_t format;
    mp3_decoder_wrapper_t *decoder;
    uint8_t *mp3_input;
    int16_t *mp3_pcm;
    size_t input_pos;
    size_t input_len;
} ap_ctx_t;

static ap_ctx_t g_ap = { .volume = 70, .loop = true };
static SemaphoreHandle_t g_audio_mutex;
static TaskHandle_t g_audio_task;

static void audio_player_stop_locked(void);

static void audio_lock(void)
{
    if (g_audio_mutex)
        xSemaphoreTakeRecursive(g_audio_mutex, portMAX_DELAY);
}

static void audio_unlock(void)
{
    if (g_audio_mutex)
        xSemaphoreGiveRecursive(g_audio_mutex);
}

/* ====== 公开 API ====== */

static esp_err_t audio_player_init_locked(const char *filename)
{
    if (!audio_is_ready()) {
        ESP_LOGE(TAG, "ES8311 is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    int volume = g_ap.volume ? g_ap.volume : 70;
    bool muted = g_ap.muted;
    if (g_ap.initialized)
        audio_player_stop_locked();
    memset(&g_ap, 0, sizeof(g_ap));
    g_ap.volume = volume;
    g_ap.muted   = muted;
    g_ap.loop    = true;

    /* 构建 FatFS 路径 */
    char fatfs_path[MEDIA_CATALOG_PATH_MAX + 4];
    if (strncmp(filename, "/0:/", 4) == 0) {
        snprintf(fatfs_path, sizeof(fatfs_path), "0:%s", filename + 4);
    } else if (filename[0] == '/') {
        snprintf(fatfs_path, sizeof(fatfs_path), "0:%s", filename);
    } else {
        strncpy(fatfs_path, filename, sizeof(fatfs_path) - 1);
    }

    FRESULT fr = f_open(&g_ap.file, fatfs_path, FA_READ);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "Failed to open: %s (err=%d)", fatfs_path, fr);
        return ESP_ERR_NOT_FOUND;
    }

    g_ap.file_size = f_size(&g_ap.file);
    g_ap.pos       = 0;
    g_ap.start_time = esp_timer_get_time();
    g_ap.initialized = true;
    strncpy(g_ap.filename, filename, sizeof(g_ap.filename) - 1);

    size_t name_len = strlen(filename);
    g_ap.format = (name_len > 4 &&
                   strcasecmp(filename + name_len - 4, ".mp3") == 0)
                      ? AUDIO_FMT_MP3 : AUDIO_FMT_PCM;
    if (g_ap.format == AUDIO_FMT_MP3) {
        g_ap.decoder = mp3_decoder_create();
        g_ap.mp3_input = heap_caps_malloc(MP3_INPUT_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        g_ap.mp3_pcm = heap_caps_malloc(MP3_PCM_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!g_ap.decoder || !g_ap.mp3_input || !g_ap.mp3_pcm) {
            ESP_LOGE(TAG, "MP3 buffer allocation failed");
            audio_player_stop_locked();
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t amp_ret = audio_amp_set_enabled(!g_ap.muted);
    if (amp_ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot enable amplifier: %s",
                 esp_err_to_name(amp_ret));
        audio_player_stop_locked();
        return amp_ret;
    }

    ESP_LOGI(TAG, "Init %s: %s (%lu bytes)",
             g_ap.format == AUDIO_FMT_MP3 ? "MP3" : "PCM",
             g_ap.filename, (unsigned long)g_ap.file_size);
    return ESP_OK;
}

static player_ret_t audio_player_tick_locked(void)
{
    if (!g_ap.initialized) return PLAYER_ERROR;
    if (g_ap.muted) return PLAYER_BUSY;

    if (g_ap.format == AUDIO_FMT_MP3) {
        for (int attempt = 0; attempt < 12; attempt++) {
            if (g_ap.input_pos >= g_ap.input_len) {
                UINT br = 0;
                FRESULT fr = f_read(&g_ap.file, g_ap.mp3_input, MP3_INPUT_SIZE, &br);
                if (fr != FR_OK) {
                    ESP_LOGE(TAG, "MP3 read error: %d", fr);
                    return PLAYER_ERROR;
                }
                if (br == 0) {
                    if (!g_ap.loop) {
                        audio_player_stop_locked();
                        return PLAYER_OK;
                    }
                    f_lseek(&g_ap.file, 0);
                    g_ap.pos = 0;
                    g_ap.chunks_done = 0;
                    mp3_decoder_reset(g_ap.decoder);
                    ESP_LOGI(TAG, "MP3 loop restart");
                    continue;
                }
                g_ap.input_pos = 0;
                g_ap.input_len = br;
                g_ap.pos += br;
            }

            size_t consumed = 0;
            size_t samples = 0;
            int result = mp3_decoder_decode(g_ap.decoder,
                                            g_ap.mp3_input + g_ap.input_pos,
                                            g_ap.input_len - g_ap.input_pos,
                                            g_ap.mp3_pcm, MP3_PCM_BYTES,
                                            &consumed, &samples);
            g_ap.input_pos += consumed;

            if (result == 2 || result == -5) {
                uint32_t rate = mp3_decoder_sample_rate(g_ap.decoder);
                uint8_t channels = mp3_decoder_channels(g_ap.decoder);
                ESP_LOGI(TAG, "MP3 stream: %lu Hz, %u ch, %lu kbps",
                         (unsigned long)rate, channels,
                         (unsigned long)mp3_decoder_bitrate(g_ap.decoder));
                continue;
            }
            if (result == -4) {
                ESP_LOGW(TAG, "Skipping damaged MP3 frame");
                continue;
            }
            if (result < 0) {
                ESP_LOGE(TAG, "MP3 decode failed: %d", result);
                return PLAYER_ERROR;
            }
            if (samples > 0) {
                if (audio_write_pcm(g_ap.mp3_pcm, samples,
                                    (int)mp3_decoder_sample_rate(g_ap.decoder),
                                    mp3_decoder_channels(g_ap.decoder)) != ESP_OK) {
                    return PLAYER_ERROR;
                }
                g_ap.chunks_done++;
                return PLAYER_OK;
            }
        }
        return PLAYER_BUSY;
    }

    /* PCM: 检查是否播放完毕 */
    if (g_ap.pos >= g_ap.file_size) {
        if (g_ap.loop) {
            /* 循环: 回到开头 */
            f_lseek(&g_ap.file, 0);
            g_ap.pos = 0;
            g_ap.chunks_done = 0;
            ESP_LOGI(TAG, "Loop restart");
        } else {
            audio_player_stop_locked();
            return PLAYER_OK;
        }
    }

    /* 读一块数据 */
    int16_t buf[CHUNK_SIZE / sizeof(int16_t)];
    FSIZE_t remain = g_ap.file_size - g_ap.pos;
    size_t to_read = (remain < CHUNK_SIZE) ? (size_t)remain : CHUNK_SIZE;
    UINT br;
    FRESULT fr = f_read(&g_ap.file, buf, to_read, &br);
    if (fr != FR_OK || br == 0) {
        ESP_LOGE(TAG, "Read err: fr=%d, br=%u", fr, br);
        return PLAYER_ERROR;
    }

    /* 写入 I2S — 直接调用 audio_play 的内部机制 */
    /* audio_play 是阻塞的, 但小块数据写入很快 (~2KB / 32KBps = ~64ms 的音频) */
    /* 实际上 2KB PCM = 1024 samples @16bit = 64ms @16kHz — 写入 I2S DMA 很快 */
    if (audio_write_pcm(buf, br / sizeof(int16_t),
                        16000, 1) != ESP_OK)
        return PLAYER_ERROR;

    g_ap.pos += br;
    g_ap.chunks_done++;

    return PLAYER_OK;
}

static void audio_player_stop_locked(void)
{
    esp_err_t amp_ret = audio_amp_set_enabled(false);
    if (amp_ret != ESP_OK && amp_ret != ESP_ERR_INVALID_STATE)
        ESP_LOGE(TAG, "Cannot mute amplifier: %s",
                 esp_err_to_name(amp_ret));
    if (!g_ap.initialized)
        return;

    int64_t elapsed = esp_timer_get_time() - g_ap.start_time;
    ESP_LOGI(TAG, "Stopped: %lu chunks in %lld ms",
             g_ap.chunks_done, elapsed / 1000);

    if (g_ap.file.obj.fs) f_close(&g_ap.file);
    mp3_decoder_destroy(g_ap.decoder);
    free(g_ap.mp3_input);
    free(g_ap.mp3_pcm);
    g_ap.decoder = NULL;
    g_ap.mp3_input = NULL;
    g_ap.mp3_pcm = NULL;
    g_ap.initialized = false;
}

static void audio_service_task(void *arg)
{
    (void)arg;
    while (1)
    {
        bool active = false;
        audio_lock();
        active = g_ap.initialized;
        if (active && audio_player_tick_locked() == PLAYER_ERROR)
        {
            ESP_LOGE(TAG, "Audio service playback error");
            audio_player_stop_locked();
        }
        audio_unlock();
        vTaskDelay(pdMS_TO_TICKS(active ? 5 : 50));
    }
}

esp_err_t audio_player_start_service(void)
{
    if (g_audio_task)
        return ESP_OK;

    if (!g_audio_mutex)
    {
        g_audio_mutex = xSemaphoreCreateRecursiveMutex();
        if (!g_audio_mutex)
            return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        audio_service_task, "audio_player", AUDIO_TASK_STACK_SIZE,
        NULL, AUDIO_TASK_PRIORITY, &g_audio_task, AUDIO_TASK_CORE);
    if (created != pdPASS)
    {
        vSemaphoreDelete(g_audio_mutex);
        g_audio_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Audio service started on CPU%d", AUDIO_TASK_CORE);
    return ESP_OK;
}

esp_err_t audio_player_init(const char *filename)
{
    audio_lock();
    esp_err_t ret = audio_player_init_locked(filename);
    audio_unlock();
    return ret;
}

esp_err_t audio_player_start(const char *selection)
{
    char name[MEDIA_CATALOG_PATH_MAX];
    esp_err_t ret = media_catalog_resolve(
        MEDIA_AUDIO, selection, name, sizeof(name));
    if (ret != ESP_OK)
        return ret;

    char path[MEDIA_CATALOG_PATH_MAX + 4];
    int written = snprintf(path, sizeof(path), "0:/%s", name);
    if (written <= 0 || written >= (int)sizeof(path))
        return ESP_ERR_INVALID_SIZE;
    return audio_player_init(path);
}

player_ret_t audio_player_tick(void)
{
    audio_lock();
    player_ret_t ret = g_ap.initialized
                           ? audio_player_tick_locked()
                           : PLAYER_BUSY;
    audio_unlock();
    return ret;
}

void audio_player_stop(void)
{
    audio_lock();
    audio_player_stop_locked();
    audio_unlock();
}

bool audio_player_is_active(void)
{
    audio_lock();
    bool active = g_ap.initialized;
    audio_unlock();
    return active;
}

esp_err_t audio_player_set_volume(int vol)
{
    audio_lock();
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    esp_err_t ret = audio_set_volume(vol);
    if (ret == ESP_OK)
        g_ap.volume = vol;
    audio_unlock();
    return ret;
}

bool audio_player_toggle_mute(void)
{
    audio_lock();
    g_ap.muted = !g_ap.muted;
    if (g_ap.muted) {
        audio_amp_set_enabled(false);
    } else {
        if (g_ap.initialized)
            audio_amp_set_enabled(true);
    }
    bool muted = g_ap.muted;
    audio_unlock();
    return muted;
}

const char *audio_player_current_file(void)
{
    audio_lock();
    const char *filename = g_ap.initialized ? g_ap.filename : NULL;
    audio_unlock();
    return filename;
}

int audio_player_list_files(char *out_buf, size_t out_len)
{
    return media_catalog_list(MEDIA_AUDIO, out_buf, out_len);
}
