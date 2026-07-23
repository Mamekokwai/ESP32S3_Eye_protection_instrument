/**
 * @brief  原始 RGB565 视频播放器 (DMA 优化 + tick 化)
 *
 * 从 SD 卡读取 .raw 文件, 每次 tick 读取一帧并推送到 LCD。
 */
#include "raw_player.h"
#include "spilcd.h"
#include "sd_card.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "raw_player"

extern esp_lcd_panel_handle_t panel_handle;
#define RAW_MAGIC   0x56574152  /* "RAWV" */
#define DMA_CHUNK   (16 * 1024)

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t total_frames;
} __attribute__((packed)) raw_header_t;

/* ====== 播放器上下文 ====== */
typedef struct {
    bool initialized;

    FIL           file;
    raw_header_t  hdr;
    uint32_t      frame_bytes;
    int64_t       frame_interval;

    uint16_t     *frame_buf;
    uint8_t      *rdma;          /* DMA 缓冲 */
    size_t        dma_size;
    int           sec_per_frame;
    int           sec_to_read;
    bool          use_chunks;

    uint16_t      offx, offy;
    DWORD         data_sec;

    uint32_t      current_frame;
    uint32_t      frame_count;
    int64_t       start_time;
    int64_t       next_frame_time;
} rp_ctx_t;

static rp_ctx_t g_rp = {0};

/* ====== 公开 API ====== */

esp_err_t raw_player_init(const char *filename)
{
    if (g_rp.initialized) raw_player_stop();
    memset(&g_rp, 0, sizeof(g_rp));

    char fatfs_path[272];
    if (strncmp(filename, "/0:/", 4) == 0) {
        snprintf(fatfs_path, sizeof(fatfs_path), "0:%s", filename + 4);
    } else {
        strncpy(fatfs_path, filename, sizeof(fatfs_path) - 1);
    }

    FRESULT fr = f_open(&g_rp.file, fatfs_path, FA_READ);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "Failed to open: %s (err=%d)", fatfs_path, fr);
        return ESP_ERR_NOT_FOUND;
    }

    UINT bytes_read;
    fr = f_read(&g_rp.file, &g_rp.hdr, sizeof(g_rp.hdr), &bytes_read);
    if (fr != FR_OK || bytes_read < sizeof(g_rp.hdr) || g_rp.hdr.magic != RAW_MAGIC) {
        ESP_LOGE(TAG, "Invalid header");
        f_close(&g_rp.file);
        return ESP_FAIL;
    }
    if (g_rp.hdr.width > 320 || g_rp.hdr.height > 240) {
        f_close(&g_rp.file);
        return ESP_ERR_NOT_SUPPORTED;
    }

    g_rp.frame_bytes    = (uint32_t)g_rp.hdr.width * g_rp.hdr.height * 2;
    g_rp.frame_interval = (g_rp.hdr.fps > 0) ? (1000000LL / g_rp.hdr.fps) : 0;
    g_rp.sec_per_frame  = (g_rp.frame_bytes + 511) / 512;
    g_rp.sec_to_read    = g_rp.sec_per_frame + 1;
    g_rp.dma_size       = g_rp.sec_to_read * 512;
    g_rp.data_sec       = g_rp.file.sect;

    /* 分配帧缓冲 */
    g_rp.frame_buf = (uint16_t *)heap_caps_malloc(g_rp.frame_bytes, MALLOC_CAP_SPIRAM);
    if (!g_rp.frame_buf) g_rp.frame_buf = (uint16_t *)malloc(g_rp.frame_bytes);
    if (!g_rp.frame_buf) { f_close(&g_rp.file); return ESP_ERR_NO_MEM; }

    /* DMA 读取缓冲 */
    g_rp.rdma = heap_caps_malloc(g_rp.dma_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    g_rp.use_chunks = false;
    if (!g_rp.rdma) {
        ESP_LOGW(TAG, "Large DMA buf OOM, using 16KB chunks");
        g_rp.use_chunks = true;
        g_rp.rdma = heap_caps_malloc(DMA_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!g_rp.rdma) { free(g_rp.frame_buf); f_close(&g_rp.file); return ESP_ERR_NO_MEM; }
    }

    g_rp.offx           = (320 - g_rp.hdr.width) / 2;
    g_rp.offy           = (240 - g_rp.hdr.height) / 2;
    g_rp.current_frame  = 0;
    g_rp.frame_count    = 0;
    g_rp.start_time     = esp_timer_get_time();
    g_rp.next_frame_time = g_rp.start_time;
    g_rp.initialized    = true;

    ESP_LOGI(TAG, "Init: %ux%u, %lu frames, %u fps (DMA=%s)",
             g_rp.hdr.width, g_rp.hdr.height, g_rp.hdr.total_frames, g_rp.hdr.fps,
             g_rp.use_chunks ? "16K chunks" : "full frame");

    return ESP_OK;
}

player_ret_t raw_player_tick(void)
{
    if (!g_rp.initialized) return PLAYER_ERROR;
    sdmmc_card_t *card = sd_card_handle();
    if (!card) return PLAYER_ERROR;

    /* 检查是否播完 */
    if (g_rp.current_frame >= g_rp.hdr.total_frames) {
        /* 循环回到开头 */
        g_rp.current_frame = 0;
        g_rp.next_frame_time = esp_timer_get_time();
    }

    /* 帧率控制 (非阻塞) */
    if (g_rp.frame_interval > 0 && g_rp.current_frame > 0) {
        if (esp_timer_get_time() < g_rp.next_frame_time)
            return PLAYER_BUSY;
        g_rp.next_frame_time += g_rp.frame_interval;
    }

    /* 读取一帧 */
    DWORD frame_sec = g_rp.data_sec + g_rp.current_frame * g_rp.sec_per_frame;
    esp_err_t ret;

    if (!g_rp.use_chunks) {
        ret = sdmmc_read_sectors(card, g_rp.rdma, frame_sec, g_rp.sec_to_read);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "read err frm %lu", g_rp.current_frame); return PLAYER_ERROR; }
        memcpy((uint8_t *)g_rp.frame_buf, g_rp.rdma + 14, g_rp.frame_bytes);
    } else {
        uint8_t *dst = (uint8_t *)g_rp.frame_buf;
        size_t remain = g_rp.frame_bytes;
        size_t dst_off = 0;
        DWORD cur_sec = frame_sec;
        int head_skip = 14;

        while (remain > 0) {
            size_t chunk = (remain + head_skip > DMA_CHUNK) ? DMA_CHUNK : remain + head_skip;
            size_t cs = (chunk + 511) / 512;
            ret = sdmmc_read_sectors(card, g_rp.rdma, cur_sec, cs);
            if (ret != ESP_OK) return PLAYER_ERROR;
            memcpy(dst + dst_off, g_rp.rdma + head_skip, chunk - head_skip);
            dst_off += chunk - head_skip;
            remain  -= chunk - head_skip;
            cur_sec += cs;
            head_skip = 0;
        }
    }

    /* 发送到 LCD (DMA 异步) */
    esp_lcd_panel_draw_bitmap(panel_handle,
                               g_rp.offx, g_rp.offy,
                               g_rp.offx + g_rp.hdr.width,
                               g_rp.offy + g_rp.hdr.height,
                               g_rp.frame_buf);
    g_rp.frame_count++;
    g_rp.current_frame++;

    if (g_rp.frame_count % 50 == 0) {
        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "frm %lu: avg %.1f fps",
                 g_rp.frame_count,
                 g_rp.frame_count * 1000000.0 / (now - g_rp.start_time));
    }

    return PLAYER_OK;
}

void raw_player_stop(void)
{
    if (!g_rp.initialized) return;

    int64_t elapsed = esp_timer_get_time() - g_rp.start_time;
    ESP_LOGI(TAG, "Done: %lu frames in %.1fs (%.1f fps)",
             g_rp.frame_count, elapsed / 1000000.0,
             g_rp.frame_count / (elapsed / 1000000.0));

    if (g_rp.file.obj.fs) f_close(&g_rp.file);
    if (g_rp.frame_buf)   { free(g_rp.frame_buf); g_rp.frame_buf = NULL; }
    if (g_rp.rdma)        { free(g_rp.rdma);      g_rp.rdma = NULL; }

    g_rp.initialized = false;
    ESP_LOGI(TAG, "Stopped");
}

/* ---- 兼容旧 API ---- */
esp_err_t raw_player_play(const char *filename)
{
    esp_err_t ret = raw_player_init(filename);
    if (ret != ESP_OK) return ret;

    while (1) {
        player_ret_t r = raw_player_tick();
        if (r == PLAYER_ERROR) { raw_player_stop(); return ESP_FAIL; }
        if (r == PLAYER_BUSY) vTaskDelay(1);
    }
}
