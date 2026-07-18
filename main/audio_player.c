/**
 * @brief  SD 卡 PCM 音频播放器 (tick 化, 非阻塞)
 *
 * 每次 tick 从 SD 卡读取一块 PCM 数据写入 I2S codec。
 * 格式: 16-bit signed, mono, 默认 16kHz (同 ES8311 配置)。
 */
#include "audio_player.h"
#include "audio.h"
#include "spi_sd.h"
#include "ff.h"
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "audio_player"
#define CHUNK_SIZE  2048  /* 每次 tick 读取的字节数 */

typedef struct {
    bool initialized;
    bool muted;
    int  volume;       /* 0-100 */

    FIL  file;
    char filename[272];
    FSIZE_t file_size;
    FSIZE_t pos;

    uint32_t chunks_done;
    int64_t  start_time;
    bool     loop;       /* 循环播放 */
} ap_ctx_t;

static ap_ctx_t g_ap = { .volume = 70, .loop = true };

/* ====== 公开 API ====== */

esp_err_t audio_player_init(const char *filename)
{
    if (g_ap.initialized) audio_player_stop();
    memset(&g_ap, 0, sizeof(g_ap));
    g_ap.volume = 70;
    g_ap.loop    = true;

    /* 构建 FatFS 路径 */
    char fatfs_path[272];
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

    ESP_LOGI(TAG, "Init: %s (%lu bytes)", g_ap.filename, (unsigned long)g_ap.file_size);
    return ESP_OK;
}

player_ret_t audio_player_tick(void)
{
    if (!g_ap.initialized) return PLAYER_ERROR;
    if (g_ap.muted) return PLAYER_BUSY;

    /* 检查是否播放完毕 */
    if (g_ap.pos >= g_ap.file_size) {
        if (g_ap.loop) {
            /* 循环: 回到开头 */
            f_lseek(&g_ap.file, 0);
            g_ap.pos = 0;
            g_ap.chunks_done = 0;
            ESP_LOGI(TAG, "Loop restart");
        } else {
            audio_player_stop();
            return PLAYER_OK;
        }
    }

    /* 读一块数据 */
    uint8_t buf[CHUNK_SIZE];
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
    audio_play(buf, br);

    g_ap.pos += br;
    g_ap.chunks_done++;

    return PLAYER_OK;
}

void audio_player_stop(void)
{
    if (!g_ap.initialized) return;

    int64_t elapsed = esp_timer_get_time() - g_ap.start_time;
    ESP_LOGI(TAG, "Stopped: %lu chunks in %lld ms",
             g_ap.chunks_done, elapsed / 1000);

    if (g_ap.file.obj.fs) f_close(&g_ap.file);
    g_ap.initialized = false;
}

void audio_player_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_ap.volume = vol;
    audio_set_volume(vol);
}

bool audio_player_toggle_mute(void)
{
    g_ap.muted = !g_ap.muted;
    if (g_ap.muted) {
        audio_set_volume(0);
    } else {
        audio_set_volume(g_ap.volume);
    }
    return g_ap.muted;
}

const char *audio_player_current_file(void)
{
    return g_ap.initialized ? g_ap.filename : NULL;
}

int audio_player_list_files(char *out_buf, size_t out_len)
{
    if (!out_buf || out_len == 0) return 0;

    DIR *dir = opendir("/0:");
    if (!dir) {
        snprintf(out_buf, out_len, "ERR no sd");
        return -1;
    }

    int count = 0;
    size_t pos = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        /* 匹配 .pcm 文件 */
        if (len > 4 && strcasecmp(name + len - 4, ".pcm") == 0) {
            int n = snprintf(out_buf + pos, out_len - pos,
                             "%d=%s\n", count + 1, name);
            if (n > 0 && pos + n < out_len) {
                pos += n;
                count++;
            }
        }
    }
    closedir(dir);

    if (count == 0) {
        snprintf(out_buf, out_len, "NONE");
    }
    return count;
}
