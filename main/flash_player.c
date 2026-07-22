/**
 * @brief  Flash 存储分区视频播放器 (tick 化)
 *
 * 使用 esp_partition_mmap 将 storage 分区映射到内存，
 * 零拷贝读取 AVI 数据，不占用 SPI2 总线。
 *
 * 双核流水线: Core0 (主循环 tick) + Core1 (JPEG 解码任务)
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
#define FRAME_BUF_SIZE (320 * 320 * sizeof(uint16_t)) /* 320x320 RGB565 */
#define MAX_JPEG_SIZE (48 * 1024)
#define PROF_EVERY 10
#define FP_PROF_ENABLE 1   /* 性能分析: 1=每10帧打印耗时占比, 0=关闭 */

extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;

/* ---- 内存映射文件读取器 ---- */
typedef struct
{
    const uint8_t *data;
    size_t size;
    size_t pos;
} mmap_file_t;

static inline size_t mmap_read(mmap_file_t *mf, void *dst, size_t len)
{
    if (mf->pos + len > mf->size)
        len = mf->size - mf->pos;
    memcpy(dst, mf->data + mf->pos, len);
    mf->pos += len;
    return len;
}

static inline size_t mmap_seek(mmap_file_t *mf, size_t offset)
{
    if (offset > mf->size)
        offset = mf->size;
    mf->pos = offset;
    return offset;
}

/* ---- 从 mmap file 读一个 AVI chunk ---- */
static bool read_chunk_mmap(mmap_file_t *mf, AVI_INFO *avi,
                            uint8_t *jpeg_out, size_t *jpeg_sz_out)
{
    while (1)
    {
        uint8_t fhdr[8];
        if (mmap_read(mf, fhdr, 8) < 8)
            return false;
        if (avi_get_streaminfo(fhdr, avi) != AVI_OK)
            return false;

        uint32_t sz = avi->StreamSize;
        if (mf->pos + sz > mf->size)
            return false;

        if (memcmp(fhdr, avi->VideoFLAG, 4) == 0)
        {
            if (sz <= MAX_JPEG_SIZE)
            {
                memcpy(jpeg_out, mf->data + mf->pos, sz);
                *jpeg_sz_out = sz;
            }
            mf->pos += sz;
            if (sz & 1)
                mf->pos++;
            return true;
        }
        else if (memcmp(fhdr, avi->AudioFLAG, 4) == 0)
        {
            mf->pos += sz;
            if (sz & 1)
                mf->pos++;
        }
        else
        {
            mf->pos += sz;
            if (sz & 1)
                mf->pos++;
        }
    }
}

/* ---- 双核流水线 ---- */
typedef struct
{
    const uint8_t *jpeg_data;
    size_t jpeg_size;
    uint16_t *out_buf;
    esp_err_t result;
} decode_job_t;

static QueueHandle_t s_q = NULL;
static SemaphoreHandle_t s_done = NULL;
static TaskHandle_t s_task = NULL;

static void decode_task(void *arg)
{
    decode_job_t job;
    while (1)
    {
        if (xQueueReceive(s_q, &job, portMAX_DELAY) == pdTRUE)
        {
            if (!job.jpeg_data)
                break;
            uint32_t w, h;
            job.result = mjpeg_decoder_decode(job.jpeg_data, job.jpeg_size,
                                              job.out_buf, FRAME_BUF_SIZE, &w, &h);
            xSemaphoreGive(s_done);
        }
    }
    vTaskDelete(NULL);
}

/* ---- 非阻塞 LCD 检查 ---- */
static inline bool lcd_is_ready(void)
{
    if (!refresh_done_flag)
        return false;
    refresh_done_flag = 0;
    return true;
}

/* ====== 播放器上下文 ====== */
typedef struct
{
    bool initialized;

    const esp_partition_t *part;
    spi_flash_mmap_handle_t mmap_handle;
    const void *flash_ptr;

    AVI_INFO avi;
    mmap_file_t mf;
    uint32_t movi_pos;

    uint16_t *frame_buf[2];
    uint8_t *jpeg_buf[2];
    size_t jpeg_size[2];

    int cur, next, pending;
    bool have_next;

    uint16_t offx, offy;
    int64_t next_ts;
    bool first_vf;

    uint32_t frame_count;
    int64_t start_ts;
    int wraps;

    /* 非阻塞条带发送状态机 */
    int16_t strip_ys;      /* -1=帧间等待, 0..height=条带发送中 */
    int64_t last_write_ts; /* 上次 LCD 写入开始时间 (17ms 最小间隔) */
} fp_ctx_t;

static fp_ctx_t g_fp = {0};

/* ====== 公开 API ====== */

esp_err_t flash_player_init(void)
{
    if (g_fp.initialized)
        flash_player_stop();
    memset(&g_fp, 0, sizeof(g_fp));

    /* 1. 找到 storage 分区并映射 */
    g_fp.part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!g_fp.part)
    {
        ESP_LOGE(TAG, "storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = esp_partition_mmap(g_fp.part, 0, g_fp.part->size,
                                       SPI_FLASH_MMAP_DATA,
                                       &g_fp.flash_ptr, &g_fp.mmap_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "mmap failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "Flash mapped: %lu bytes at %p",
             (unsigned long)g_fp.part->size, g_fp.flash_ptr);

    /* 2. 解析 AVI 头 */
    AVISTATUS ar = avi_init((const uint8_t *)g_fp.flash_ptr, g_fp.part->size, &g_fp.avi);
    if (ar != AVI_OK)
    {
        ESP_LOGE(TAG, "AVI init failed: %d", ar);
        spi_flash_munmap(g_fp.mmap_handle);
        return ESP_FAIL;
    }
    if (g_fp.avi.Width > 320 || g_fp.avi.Height > 320)
    {
        spi_flash_munmap(g_fp.mmap_handle);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* 3. 初始化流水线 */
    mjpeg_decoder_init(g_fp.avi.Width, g_fp.avi.Height);
    s_q = xQueueCreate(2, sizeof(decode_job_t));
    s_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(decode_task, "jpeg_f", 4096, NULL, 5, &s_task, 1);

    for (int i = 0; i < 2; i++)
    {
        g_fp.frame_buf[i] = heap_caps_aligned_alloc(64, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        g_fp.jpeg_buf[i] = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_fp.frame_buf[i] || !g_fp.jpeg_buf[i])
        {
            ret = ESP_ERR_NO_MEM;
            goto fail;
        }
    }

    /* 4. 设置 mmap 读取器并跳到 movi */
    g_fp.mf.data = g_fp.flash_ptr;
    g_fp.mf.size = g_fp.part->size;
    g_fp.movi_pos = g_fp.avi.MoviOffset + 4;
    mmap_seek(&g_fp.mf, g_fp.movi_pos);

    /* 预读前两帧 */
    if (!read_chunk_mmap(&g_fp.mf, &g_fp.avi, g_fp.jpeg_buf[0], &g_fp.jpeg_size[0]) ||
        !read_chunk_mmap(&g_fp.mf, &g_fp.avi, g_fp.jpeg_buf[1], &g_fp.jpeg_size[1]))
    {
        ESP_LOGE(TAG, "prefetch fail");
        ret = ESP_FAIL;
        goto fail;
    }

    decode_job_t job0 = {g_fp.jpeg_buf[0], g_fp.jpeg_size[0], g_fp.frame_buf[0], ESP_OK};
    xQueueSend(s_q, &job0, 0);

    g_fp.offx = (320 - g_fp.avi.Width) / 2;
    g_fp.offy = (320 - g_fp.avi.Height) / 2; /* 320x320 圆屏 */
    g_fp.first_vf = true;
    g_fp.have_next = true;
    g_fp.pending = 1;
    g_fp.cur = 0;
    g_fp.next = 1;
    g_fp.strip_ys = -1; /* 帧间等待状态 */
    g_fp.last_write_ts = 0;
    g_fp.start_ts = esp_timer_get_time();
    g_fp.next_ts = g_fp.start_ts + g_fp.avi.SecPerFrame;
    g_fp.initialized = true;

    ESP_LOGI(TAG, "Init OK: %lux%lu, %lu us/frame",
             g_fp.avi.Width, g_fp.avi.Height, g_fp.avi.SecPerFrame);
    return ESP_OK;

fail:
    flash_player_stop();
    return ret;
}

player_ret_t flash_player_tick(void)
{
    if (!g_fp.initialized)
        return PLAYER_ERROR;

    static int64_t t_dec, t_read, t_lcd, t_te, t_ctl;
    int64_t t0, now;

#define FP_STRIP_H 40
    static uint16_t *strip_buf = NULL;
    if (!strip_buf)
    {
        strip_buf = heap_caps_malloc(FP_STRIP_H * 320 * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
#if FP_PROF_ENABLE
        ESP_LOGI(TAG, "strip buf %s", strip_buf ? "OK (internal DMA)" : "FAIL");
#endif
        if (!strip_buf)
            return PLAYER_ERROR;
    }

    now = esp_timer_get_time();

    /* === 帧间等待状态: 检查所有前置条件 === */
    if (g_fp.strip_ys < 0)
    {
        /* Step A: 非阻塞检查解码完成 */
        t0 = now;
        if (xSemaphoreTake(s_done, 0) != pdTRUE)
            return PLAYER_BUSY;
        g_fp.pending--;
        t_dec += esp_timer_get_time() - t0;

        /* Step B: 提交下一帧解码 */
        if (g_fp.have_next)
        {
            decode_job_t job = {
                g_fp.jpeg_buf[g_fp.next], g_fp.jpeg_size[g_fp.next],
                g_fp.frame_buf[g_fp.next], ESP_OK};
            xQueueSend(s_q, &job, 0);
            g_fp.pending++;
        }

        /* Step C: 预读下一帧 */
        t0 = esp_timer_get_time();
        g_fp.have_next = read_chunk_mmap(&g_fp.mf, &g_fp.avi,
                                         g_fp.jpeg_buf[g_fp.cur],
                                         &g_fp.jpeg_size[g_fp.cur]);
        if (!g_fp.have_next)
        {
            mmap_seek(&g_fp.mf, g_fp.movi_pos);
            g_fp.have_next = read_chunk_mmap(&g_fp.mf, &g_fp.avi,
                                             g_fp.jpeg_buf[g_fp.cur],
                                             &g_fp.jpeg_size[g_fp.cur]);
            g_fp.wraps++;
        }
        t_read += esp_timer_get_time() - t0;

        /* Step D: 非阻塞帧率控制 (按视频帧率) */
        t0 = esp_timer_get_time();
        if (!g_fp.first_vf)
        {
            if (esp_timer_get_time() < g_fp.next_ts)
                return PLAYER_BUSY;
            g_fp.next_ts += g_fp.avi.SecPerFrame;
        }
        else
        {
            g_fp.first_vf = false;
            g_fp.next_ts = esp_timer_get_time() + g_fp.avi.SecPerFrame;
        }
        t_ctl += esp_timer_get_time() - t0;

        /* Step E: LCD 空闲检查 */
        if (!lcd_is_ready())
            return PLAYER_BUSY;

        /* Step E2: 帧同步 — 距上次写入最小 17ms (60Hz 对齐) + TE 硬件 */
        t0 = esp_timer_get_time();
        if (g_fp.last_write_ts != 0)
        {
            if (esp_timer_get_time() < g_fp.last_write_ts + 17000)
            {
                t_te += esp_timer_get_time() - t0;
                return PLAYER_BUSY;
            }
        }
        spilcd_wait_te(); /* 硬件 TE (LCD_TE_ENABLE=1 时等待脉冲) */
        t_te += esp_timer_get_time() - t0;

        /* 所有前置条件满足, 开始发送条带 */
        g_fp.strip_ys = 0;
        g_fp.last_write_ts = esp_timer_get_time();
        /* refresh_done_flag 由上一帧最后一条带或 lcd_is_ready 置位, 直接可用 */
    }

    /* === 条带发送状态机: 每次调用发一条, 不阻塞 === */

    /* Step F: 等上一次 DMA 完成 (第一条带无需等) */
    t0 = esp_timer_get_time();
    if (g_fp.strip_ys > 0 && !refresh_done_flag)
    {
        t_lcd += esp_timer_get_time() - t0;
        return PLAYER_BUSY;
    }

    int ys = g_fp.strip_ys;

    /* 所有条带发完? (不消费 refresh_done_flag — 留给下一帧 Step E) */
    if (ys >= (int)g_fp.avi.Height)
    {
        g_fp.strip_ys = -1; /* 回到帧间等待状态 */
        g_fp.frame_count++;
#if FP_PROF_ENABLE
        ESP_LOGI(TAG, "frame %lu done", (unsigned long)g_fp.frame_count);
#endif
        t_lcd += esp_timer_get_time() - t0;

#if FP_PROF_ENABLE
        if (g_fp.frame_count % PROF_EVERY == 0)
        {
            int64_t elapsed = esp_timer_get_time() - g_fp.start_ts;
            int64_t t_sum = t_dec + t_read + t_ctl + t_te + t_lcd;
            int64_t t_wait = elapsed - t_sum;
            ESP_LOGI(TAG, "frm %lu: %.1f fps (wrap %d)", g_fp.frame_count,
                     g_fp.frame_count * 1e6 / elapsed, g_fp.wraps);
            ESP_LOGI(TAG, "  decode wait: %lld ms (%.0f%%)", t_dec / 1000, 100.0 * t_dec / elapsed);
            ESP_LOGI(TAG, "  read chunk : %lld ms (%.0f%%)", t_read / 1000, 100.0 * t_read / elapsed);
            ESP_LOGI(TAG, "  rate ctrl  : %lld ms (%.0f%%)", t_ctl / 1000, 100.0 * t_ctl / elapsed);
            ESP_LOGI(TAG, "  TE wait    : %lld ms (%.0f%%)", t_te / 1000, 100.0 * t_te / elapsed);
            ESP_LOGI(TAG, "  LCD draw   : %lld ms (%.0f%%)", t_lcd / 1000, 100.0 * t_lcd / elapsed);
            ESP_LOGI(TAG, "  idle/busy  : %lld ms (%.0f%%)", t_wait / 1000, 100.0 * t_wait / elapsed);
            ESP_LOGI(TAG, "  total      : %lld ms (100%%)", t_sum / 1000);
            t_dec = t_read = t_lcd = t_te = t_ctl = 0;
        }
#endif

        g_fp.cur = g_fp.next;
        g_fp.next = 1 - g_fp.next;

        return PLAYER_OK;
    }

    /* 发当前条带: PSRAM→memcpy→内部SRAM→DMA (PSRAM DMA 有数据损坏问题) */
    refresh_done_flag = 0;
    {
        int h = (ys + FP_STRIP_H > (int)g_fp.avi.Height)
                    ? (int)g_fp.avi.Height - ys
                    : FP_STRIP_H;
        uint16_t *fb = (uint16_t *)g_fp.frame_buf[g_fp.cur];
        size_t strip_bytes = h * g_fp.avi.Width * sizeof(uint16_t);
        memcpy(strip_buf, fb + ys * g_fp.avi.Width, strip_bytes);
        esp_lcd_panel_draw_bitmap(panel_handle,
                                  g_fp.offx, g_fp.offy + ys,
                                  g_fp.offx + g_fp.avi.Width,
                                  g_fp.offy + ys + h,
                                  strip_buf);
        g_fp.strip_ys = ys + FP_STRIP_H;
    }

    t_lcd += esp_timer_get_time() - t0;
    return PLAYER_BUSY; /* 还有条带要发 */
}

void flash_player_stop(void)
{
    if (!g_fp.initialized)
        return;

    if (s_q && s_task)
    {
        decode_job_t shutdown = {NULL, 0, NULL, ESP_OK};
        xQueueSend(s_q, &shutdown, portMAX_DELAY);
        int wait = 0;
        while (eTaskGetState(s_task) != eDeleted && wait < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            wait++;
        }
        s_task = NULL;
    }
    if (s_q)
    {
        vQueueDelete(s_q);
        s_q = NULL;
    }
    if (s_done)
    {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }

    mjpeg_decoder_deinit();

    if (g_fp.mmap_handle)
        spi_flash_munmap(g_fp.mmap_handle);
    g_fp.mmap_handle = 0;

    for (int i = 0; i < 2; i++)
    {
        if (g_fp.frame_buf[i])
        {
            free(g_fp.frame_buf[i]);
            g_fp.frame_buf[i] = NULL;
        }
        if (g_fp.jpeg_buf[i])
        {
            free(g_fp.jpeg_buf[i]);
            g_fp.jpeg_buf[i] = NULL;
        }
    }

    g_fp.initialized = false;
    ESP_LOGI(TAG, "Stopped");
}

/* ---- 兼容旧 API ---- */
esp_err_t flash_video_play(void)
{
    esp_err_t ret = flash_player_init();
    if (ret != ESP_OK)
        return ret;

    while (1)
    {
        player_ret_t r = flash_player_tick();
        if (r == PLAYER_ERROR)
        {
            flash_player_stop();
            return ESP_FAIL;
        }
        if (r == PLAYER_BUSY)
            vTaskDelay(1);
    }
}
