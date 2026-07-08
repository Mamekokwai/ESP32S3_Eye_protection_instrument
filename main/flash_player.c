/**
 * @brief  Flash 存储分区视频播放器
 *
 * 使用 esp_partition_mmap 将 storage 分区映射到内存，
 * 零拷贝读取 AVI 数据，不占用 SPI2 总线。
 */
#include "flash_player.h"
#include "avi.h"
#include "mjpeg.h"
#include "spilcd.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

#define TAG "flash_player"
#define FRAME_BUF_SIZE  (320 * 240 * sizeof(uint16_t))
#define MAX_JPEG_SIZE   (48 * 1024)
#define PROF_EVERY      10

extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;

/* ---- 内存映射文件读取器 ---- */
typedef struct {
    const uint8_t *data;   /* 映射基址 */
    size_t         size;   /* 总大小 */
    size_t         pos;    /* 当前读取位置 */
} mmap_file_t;

static inline size_t mmap_read(mmap_file_t *mf, void *dst, size_t len)
{
    if (mf->pos + len > mf->size) len = mf->size - mf->pos;
    memcpy(dst, mf->data + mf->pos, len);
    mf->pos += len;
    return len;
}

static inline size_t mmap_seek(mmap_file_t *mf, size_t offset)
{
    if (offset > mf->size) offset = mf->size;
    mf->pos = offset;
    return offset;
}

static inline size_t mmap_tell(mmap_file_t *mf) { return mf->pos; }

/* ---- 从 mmap file 读一个 AVI chunk ---- */
static bool read_chunk_mmap(mmap_file_t *mf, AVI_INFO *avi,
                            uint8_t *jpeg_out, size_t *jpeg_sz_out)
{
    while (1) {
        uint8_t fhdr[8];
        if (mmap_read(mf, fhdr, 8) < 8) return false;
        if (avi_get_streaminfo(fhdr, avi) != AVI_OK) return false;

        uint32_t sz = avi->StreamSize;
        if (mf->pos + sz > mf->size) return false;

        if (memcmp(fhdr, avi->VideoFLAG, 4) == 0) {
            if (sz <= MAX_JPEG_SIZE) {
                memcpy(jpeg_out, mf->data + mf->pos, sz);
                *jpeg_sz_out = sz;
            }
            mf->pos += sz;
            if (sz & 1) mf->pos++;  /* padding */
            return true;
        } else if (memcmp(fhdr, avi->AudioFLAG, 4) == 0) {
            mf->pos += sz;           /* 跳过音频 */
            if (sz & 1) mf->pos++;
        } else {
            mf->pos += sz;           /* 跳过未知 */
            if (sz & 1) mf->pos++;
        }
    }
}

/* ---- 双核流水线 (和 video_player 一样) ---- */
typedef struct {
    const uint8_t *jpeg_data;
    size_t         jpeg_size;
    uint16_t      *out_buf;
    esp_err_t      result;
} decode_job_t;

static QueueHandle_t s_q = NULL;
static SemaphoreHandle_t s_done = NULL;
static TaskHandle_t s_task = NULL;

static void decode_task(void *arg)
{
    decode_job_t job;
    while (1) {
        if (xQueueReceive(s_q, &job, portMAX_DELAY) == pdTRUE) {
            if (!job.jpeg_data) break;
            uint32_t w, h;
            job.result = mjpeg_decoder_decode(job.jpeg_data, job.jpeg_size,
                                               job.out_buf, FRAME_BUF_SIZE, &w, &h);
            xSemaphoreGive(s_done);
        }
    }
    vTaskDelete(NULL);
}

/* ---- 主函数 ---- */
esp_err_t flash_video_play(void)
{
    /* 1. 找到 storage 分区并映射 */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!part) {
        ESP_LOGE(TAG, "storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    spi_flash_mmap_handle_t mmap_handle;
    const void *flash_ptr;
    esp_err_t ret = esp_partition_mmap(part, 0, part->size,
                                        SPI_FLASH_MMAP_DATA, &flash_ptr, &mmap_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "Flash mapped: %lu bytes at %p", (unsigned long)part->size, flash_ptr);

    /* 2. 解析 AVI 头 */
    AVI_INFO avi;
    AVISTATUS ar = avi_init((const uint8_t *)flash_ptr, part->size, &avi);
    if (ar != AVI_OK) {
        ESP_LOGE(TAG, "AVI init failed: %d (no video in flash?)", ar);
        spi_flash_munmap(mmap_handle);
        return ESP_FAIL;
    }
    if (avi.Width > 320 || avi.Height > 240) {
        spi_flash_munmap(mmap_handle);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 3. 初始化解码器 + 流水线 */
    mjpeg_decoder_init(avi.Width, avi.Height);
    s_q = xQueueCreate(2, sizeof(decode_job_t));
    s_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(decode_task, "jpeg", 4096, NULL, 5, &s_task, 1);

    uint16_t *frame_buf[2];
    uint8_t  *jpeg_buf[2];
    size_t    jpeg_size[2];
    for (int i = 0; i < 2; i++) {
        frame_buf[i] = heap_caps_aligned_alloc(16, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        jpeg_buf[i]  = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        if (!frame_buf[i] || !jpeg_buf[i]) { ret = ESP_ERR_NO_MEM; goto cleanup; }
    }

    /* 4. 设置 mmap 读取器 */
    mmap_file_t mf = { .data = flash_ptr, .size = part->size, .pos = 0 };
    uint32_t movi_pos = avi.MoviOffset + 4;
    mmap_seek(&mf, movi_pos);

    /* 预读前两帧 */
    if (!read_chunk_mmap(&mf, &avi, jpeg_buf[0], &jpeg_size[0]) ||
        !read_chunk_mmap(&mf, &avi, jpeg_buf[1], &jpeg_size[1]))
        { ESP_LOGE(TAG, "prefetch fail"); ret = ESP_FAIL; goto cleanup; }

    decode_job_t job0 = { jpeg_buf[0], jpeg_size[0], frame_buf[0] };
    xQueueSend(s_q, &job0, 0);

    /* 5. 主循环 */
    uint16_t offx = (320 - avi.Width) / 2, offy = (240 - avi.Height) / 2;
    uint32_t frame_count = 0;
    int64_t start_ts = esp_timer_get_time(), next_ts = start_ts + avi.SecPerFrame;
    bool first_vf = true;
    int cur = 0, next = 1, pending = 1;
    bool have_next = true;
    int64_t t_dec = 0, t_read = 0, t_lcd = 0, t_ctl = 0;
    int wraps = 0;
    uint32_t total_visible_frames = avi.TotalFrame;  /* 一轮循环的帧数 */

    ESP_LOGI(TAG, "Playing from Flash: %lux%lu, %lu us/frame",
             avi.Width, avi.Height, avi.SecPerFrame);

    while (1) {
        int64_t t0 = esp_timer_get_time();
        xSemaphoreTake(s_done, portMAX_DELAY);
        t_dec += esp_timer_get_time() - t0;
        pending--;

        if (have_next) {
            decode_job_t job = { jpeg_buf[next], jpeg_size[next], frame_buf[next] };
            xQueueSend(s_q, &job, 0);
            pending++;
        }

        t0 = esp_timer_get_time();
        have_next = read_chunk_mmap(&mf, &avi, jpeg_buf[cur], &jpeg_size[cur]);
        if (!have_next) {
            /* 绕回 movi 头 */
            mmap_seek(&mf, movi_pos);
            have_next = read_chunk_mmap(&mf, &avi, jpeg_buf[cur], &jpeg_size[cur]);
            wraps++;
        }
        t_read += esp_timer_get_time() - t0;

        t0 = esp_timer_get_time();
        if (!first_vf) {
            while (esp_timer_get_time() < next_ts) vTaskDelay(1);
            next_ts += avi.SecPerFrame;
        } else { first_vf = false; next_ts = esp_timer_get_time() + avi.SecPerFrame; }
        t_ctl += esp_timer_get_time() - t0;

        t0 = esp_timer_get_time();
        while (!refresh_done_flag) vTaskDelay(1);
        refresh_done_flag = 0;
        t_lcd += esp_timer_get_time() - t0;
        esp_lcd_panel_draw_bitmap(panel_handle, offx, offy,
                                   offx + avi.Width, offy + avi.Height, frame_buf[cur]);
        frame_count++;

        if (frame_count % PROF_EVERY == 0) {
            int64_t elapsed = esp_timer_get_time() - start_ts;
            ESP_LOGI(TAG, "--- profile %lu frames (wrap %d) ---", frame_count, wraps);
            ESP_LOGI(TAG, "  elapsed       : %lld ms", elapsed / 1000);
            ESP_LOGI(TAG, "  avg fps       : %.1f", frame_count * 1e6 / elapsed);
            ESP_LOGI(TAG, "  wait decode   : %lld ms (%.0f%%)",
                     t_dec / 1000, 100.0 * t_dec / elapsed);
            ESP_LOGI(TAG, "  read chunk    : %lld ms (%.0f%%)",
                     t_read / 1000, 100.0 * t_read / elapsed);
            ESP_LOGI(TAG, "  wait lcd      : %lld ms (%.0f%%)",
                     t_lcd / 1000, 100.0 * t_lcd / elapsed);
            ESP_LOGI(TAG, "  rate control  : %lld ms (%.0f%%)",
                     t_ctl / 1000, 100.0 * t_ctl / elapsed);
        }
        cur = next; next = 1 - next;
    }

cleanup:
    if (s_q)    { decode_job_t s = {NULL}; xQueueSend(s_q, &s, portMAX_DELAY); vQueueDelete(s_q); s_q = NULL; }
    if (s_done) { vSemaphoreDelete(s_done); s_done = NULL; }
    mjpeg_decoder_deinit();
    spi_flash_munmap(mmap_handle);
    for (int i = 0; i < 2; i++) { free(frame_buf[i]); free(jpeg_buf[i]); }
    return ret;
}
