/**
 * @brief  Flash 存储分区视频播放器 (tick 化)
 *
 * 使用 flash_media 的常驻mmap索引零拷贝读取AVI，不占用SPI2总线。
 *
 * CPU0 运行显示主循环和 Flash JPEG 解码，CPU1 专用于音频。
 */
#include "flash_player.h"
#include "avi.h"
#include "flash_media.h"
#include "mjpeg.h"
#include "spilcd.h"
#include "esp_lcd_jd9855.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

#define TAG "flash_player"
#define FRAME_BUF_SIZE (320 * 320 * sizeof(uint16_t)) /* 320x320 RGB565 */
#define MAX_JPEG_SIZE (48 * 1024)
#define FP_STRIP_H 40
#define FP_STRIP_BYTES (FP_STRIP_H * 320 * sizeof(uint16_t))
#define FP_STRIP_BUFS 2
#define PROF_EVERY 10
#define FP_STATS_EVERY 100
#define FP_LCD_CONTINUOUS_WRITE 1 /* 1=RAMWR+RAMWRC 流水线, 0=逐条带兼容模式 */
#define FP_PROF_ENABLE 0   /* 性能分析: 1=每10帧打印耗时占比, 0=关闭 */

extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;
extern volatile uint32_t refresh_done_count;

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
            if (sz > MAX_JPEG_SIZE)
            {
                mf->pos += sz + (sz & 1);
                continue;
            }
            memcpy(jpeg_out, mf->data + mf->pos, sz);
            *jpeg_sz_out = sz;
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
static esp_err_t s_decode_result = ESP_OK;
static uint32_t s_decode_width = 0;
static uint32_t s_decode_height = 0;

static void decode_task(void *arg)
{
    decode_job_t job;
    while (1)
    {
        if (xQueueReceive(s_q, &job, portMAX_DELAY) == pdTRUE)
        {
            if (!job.jpeg_data)
                break;
            s_decode_result = mjpeg_decoder_decode(job.jpeg_data, job.jpeg_size,
                                                   job.out_buf, FRAME_BUF_SIZE,
                                                   &s_decode_width, &s_decode_height);
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

    const uint8_t *flash_ptr;
    size_t flash_size;
    char name[FLASH_MEDIA_NAME_SIZE];

    AVI_INFO avi;
    mmap_file_t mf;
    uint32_t movi_pos;

    uint16_t *frame_buf[2];
    uint8_t *jpeg_buf[2];
    size_t jpeg_size[2];
    uint16_t *strip_buf[FP_STRIP_BUFS];
    uint8_t strip_buf_count;

    int cur, next;
    bool have_next;
    bool frame_decoded;
    bool next_queued;

    uint16_t offx, offy;
    int64_t next_ts;
    bool first_vf;

    uint32_t frame_count;
    int64_t start_ts;
    int64_t stats_ts;
    int wraps;

    /* 非阻塞条带发送状态机 */
    int16_t strip_ys;      /* -1=帧间等待, 0..height=条带发送中 */
    uint16_t strip_submitted;
    uint32_t lcd_done_base;
    int64_t last_write_ts; /* 上次 LCD 写入开始时间 (17ms 最小间隔) */
} fp_ctx_t;

static fp_ctx_t g_fp = {0};

/* ====== 公开 API ====== */

int flash_player_list_files(char *output, size_t output_size)
{
    return flash_media_list(FLASH_MEDIA_VIDEO, output, output_size);
}

const char *flash_player_name(void)
{
    return g_fp.name;
}

esp_err_t flash_player_start(const char *selection)
{
    if (g_fp.initialized)
        flash_player_stop();
    memset(&g_fp, 0, sizeof(g_fp));

    /* 1. 从Flash媒体索引选择AVI。 */
    const flash_media_entry_t *entry = NULL;
    esp_err_t ret = flash_media_resolve(
        FLASH_MEDIA_VIDEO, selection, &entry);
    if (ret != ESP_OK)
        return ret;
    g_fp.flash_ptr = flash_media_data(entry);
    if (!g_fp.flash_ptr)
        return ESP_ERR_INVALID_RESPONSE;
    g_fp.flash_size = entry->size;
    snprintf(g_fp.name, sizeof(g_fp.name), "%s", entry->name);

    /* 从这里开始fail路径可统一释放解码器、任务和缓冲。 */
    g_fp.initialized = true;
    ESP_LOGI(TAG, "Selected %s: %lu bytes at %p",
             g_fp.name, (unsigned long)g_fp.flash_size, g_fp.flash_ptr);

    /* 2. 解析 AVI 头 */
    AVISTATUS ar = avi_init(g_fp.flash_ptr, g_fp.flash_size, &g_fp.avi);
    if (ar != AVI_OK)
    {
        ESP_LOGE(TAG, "AVI init failed: %d", ar);
        ret = ESP_FAIL;
        goto fail;
    }
    if (g_fp.avi.Width > 320 || g_fp.avi.Height > 320)
    {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }

    /* 3. 初始化流水线 */
    ret = mjpeg_decoder_init(g_fp.avi.Width, g_fp.avi.Height);
    if (ret != ESP_OK)
        goto fail;
    s_q = xQueueCreate(1, sizeof(decode_job_t));
    s_done = xSemaphoreCreateBinary();
    if (!s_q || !s_done ||
        xTaskCreatePinnedToCore(decode_task, "jpeg_f", 4096, NULL, 5,
                                &s_task, 0) != pdPASS)
    {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

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
    for (int i = 0; i < FP_STRIP_BUFS; i++)
    {
        g_fp.strip_buf[i] = heap_caps_aligned_alloc(
            64, FP_STRIP_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!g_fp.strip_buf[i])
            break;
        g_fp.strip_buf_count++;
    }
    if (g_fp.strip_buf_count == 0)
    {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (g_fp.strip_buf_count < FP_STRIP_BUFS)
        ESP_LOGW(TAG, "Only one SRAM strip buffer; DMA overlap disabled");

    /* 4. 设置 mmap 读取器并跳到 movi */
    g_fp.mf.data = g_fp.flash_ptr;
    g_fp.mf.size = g_fp.flash_size;
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
    if (xQueueSend(s_q, &job0, 0) != pdTRUE)
    {
        ret = ESP_FAIL;
        goto fail;
    }

    g_fp.offx = (320 - g_fp.avi.Width) / 2;
    g_fp.offy = (320 - g_fp.avi.Height) / 2; /* 320x320 圆屏 */
    g_fp.first_vf = true;
    g_fp.have_next = true;
    g_fp.frame_decoded = false;
    g_fp.next_queued = false;
    g_fp.cur = 0;
    g_fp.next = 1;
    g_fp.strip_ys = -1; /* 帧间等待状态 */
    g_fp.last_write_ts = 0;
    g_fp.start_ts = esp_timer_get_time();
    g_fp.stats_ts = g_fp.start_ts;
    g_fp.next_ts = g_fp.start_ts + g_fp.avi.SecPerFrame;
    refresh_done_flag = 1; /* 初始化阶段没有播放器 DMA，允许首帧立即启动。 */
    ESP_LOGI(TAG, "Init OK: %lux%lu, %lu us/frame",
             g_fp.avi.Width, g_fp.avi.Height, g_fp.avi.SecPerFrame);
    return ESP_OK;

fail:
    flash_player_stop();
    return ret;
}

esp_err_t flash_player_init(void)
{
    return flash_player_start("1");
}

player_ret_t flash_player_tick(void)
{
    if (!g_fp.initialized)
        return PLAYER_ERROR;

#if FP_PROF_ENABLE
    static int64_t t_dec, t_read, t_lcd, t_te, t_ctl;
    int64_t t0;
#endif

    /* === 帧间等待状态: 检查所有前置条件 === */
    if (g_fp.strip_ys < 0)
    {
        /* Step A: 非阻塞检查解码完成 */
#if FP_PROF_ENABLE
        t0 = esp_timer_get_time();
#endif
        if (!g_fp.frame_decoded)
        {
            if (xSemaphoreTake(s_done, 0) != pdTRUE)
                return PLAYER_BUSY;
            if (s_decode_result != ESP_OK ||
                s_decode_width != g_fp.avi.Width || s_decode_height != g_fp.avi.Height)
            {
                ESP_LOGE(TAG, "decode failed: err=%d size=%lux%lu", s_decode_result,
                         (unsigned long)s_decode_width, (unsigned long)s_decode_height);
                return PLAYER_ERROR;
            }
            g_fp.frame_decoded = true;
        }
#if FP_PROF_ENABLE
        t_dec += esp_timer_get_time() - t0;
#endif

        /* Step B/C 只推进一次: 提交下一帧解码, 再复用已释放的 JPEG 缓冲预读 */
        if (!g_fp.next_queued)
        {
            if (!g_fp.have_next)
                return PLAYER_ERROR;
            decode_job_t job = {
                g_fp.jpeg_buf[g_fp.next], g_fp.jpeg_size[g_fp.next],
                g_fp.frame_buf[g_fp.next], ESP_OK};
            if (xQueueSend(s_q, &job, 0) != pdTRUE)
                return PLAYER_BUSY;

#if FP_PROF_ENABLE
        t0 = esp_timer_get_time();
#endif
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
#if FP_PROF_ENABLE
            t_read += esp_timer_get_time() - t0;
#endif
            g_fp.next_queued = true;
        }

        /* Step D: 非阻塞帧率控制 (按视频帧率) */
#if FP_PROF_ENABLE
        t0 = esp_timer_get_time();
#endif
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
#if FP_PROF_ENABLE
        t_ctl += esp_timer_get_time() - t0;
#endif

        /* Step E: LCD 空闲检查 */
        if (!lcd_is_ready())
            return PLAYER_BUSY;

        /* Step E2: 帧同步 — 距上次写入最小 17ms (60Hz 对齐) + TE 硬件 */
#if FP_PROF_ENABLE
        t0 = esp_timer_get_time();
#endif
        if (g_fp.last_write_ts != 0)
        {
            if (esp_timer_get_time() < g_fp.last_write_ts + 17000)
            {
#if FP_PROF_ENABLE
                t_te += esp_timer_get_time() - t0;
#endif
                return PLAYER_BUSY;
            }
        }
        spilcd_wait_te(); /* 硬件 TE (LCD_TE_ENABLE=1 时等待脉冲) */
#if FP_PROF_ENABLE
        t_te += esp_timer_get_time() - t0;
#endif

        /* 所有前置条件满足, 开始发送条带 */
        g_fp.strip_ys = 0;
        g_fp.strip_submitted = 0;
        g_fp.lcd_done_base = refresh_done_count;
        refresh_done_flag = 0;
        g_fp.last_write_ts = esp_timer_get_time();
    }

    /* === 双 SRAM 条带流水线: CPU memcpy 与上一条 DMA 重叠 === */
#if FP_PROF_ENABLE
    t0 = esp_timer_get_time();
#endif

    int ys = g_fp.strip_ys;
    uint32_t completed = refresh_done_count - g_fp.lcd_done_base;
    if (completed > g_fp.strip_submitted)
        completed = g_fp.strip_submitted;

    /* 所有条带已提交且 DMA 全部完成后才交换帧缓冲 */
    if (ys >= (int)g_fp.avi.Height)
    {
        if (completed < g_fp.strip_submitted)
            return PLAYER_BUSY;
        g_fp.strip_ys = -1; /* 回到帧间等待状态 */
        g_fp.frame_count++;
        if (g_fp.frame_count % FP_STATS_EVERY == 0)
        {
            int64_t stats_now = esp_timer_get_time();
            float fps = FP_STATS_EVERY * 1000000.0f / (stats_now - g_fp.stats_ts);
            ESP_LOGI(TAG, "perf: %.1f fps, frames=%lu, wraps=%d", fps,
                     (unsigned long)g_fp.frame_count, g_fp.wraps);
            g_fp.stats_ts = stats_now;
        }
#if FP_PROF_ENABLE
        ESP_LOGI(TAG, "frame %lu done", (unsigned long)g_fp.frame_count);
#endif
#if FP_PROF_ENABLE
        t_lcd += esp_timer_get_time() - t0;
#endif

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
        g_fp.frame_decoded = false;
        g_fp.next_queued = false;

        return PLAYER_OK;
    }

    if (g_fp.strip_submitted - completed >= g_fp.strip_buf_count)
        return PLAYER_BUSY;

    /* PSRAM→memcpy→内部 SRAM→DMA；双缓冲避免 DMA 等待下一次 memcpy */
    {
        int h = (ys + FP_STRIP_H > (int)g_fp.avi.Height)
                    ? (int)g_fp.avi.Height - ys
                    : FP_STRIP_H;
        uint16_t *fb = (uint16_t *)g_fp.frame_buf[g_fp.cur];
        uint16_t *strip_buf = g_fp.strip_buf[g_fp.strip_submitted % g_fp.strip_buf_count];
        size_t strip_bytes = h * g_fp.avi.Width * sizeof(uint16_t);
        memcpy(strip_buf, fb + ys * g_fp.avi.Width, strip_bytes);
        esp_err_t ret;
#if FP_LCD_CONTINUOUS_WRITE
        if (g_fp.strip_submitted == 0)
        {
            ret = esp_lcd_jd9855_draw_bitmap_start(
                panel_handle,
                g_fp.offx, g_fp.offy,
                g_fp.offx + g_fp.avi.Width,
                g_fp.offy + g_fp.avi.Height,
                strip_buf, strip_bytes);
        }
        else
        {
            ret = esp_lcd_jd9855_draw_bitmap_continue(panel_handle,
                                                       strip_buf, strip_bytes);
        }
#else
        ret = esp_lcd_panel_draw_bitmap(panel_handle,
                                        g_fp.offx, g_fp.offy + ys,
                                        g_fp.offx + g_fp.avi.Width,
                                        g_fp.offy + ys + h,
                                        strip_buf);
#endif
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "LCD submit failed: %d", ret);
            return PLAYER_ERROR;
        }
        g_fp.strip_submitted++;
        g_fp.strip_ys = ys + h;
    }

#if FP_PROF_ENABLE
    t_lcd += esp_timer_get_time() - t0;
#endif
    return PLAYER_BUSY; /* 还有条带要发 */
}

void flash_player_stop(void)
{
    if (!g_fp.initialized)
        return;

    bool lcd_idle = true;
    if (g_fp.strip_submitted > 0)
    {
        int wait = 0;
        while (refresh_done_count - g_fp.lcd_done_base < g_fp.strip_submitted && wait < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            wait++;
        }
        lcd_idle = refresh_done_count - g_fp.lcd_done_base >= g_fp.strip_submitted;
        if (!lcd_idle)
            ESP_LOGE(TAG, "LCD DMA stop timeout; preserving strip buffers");
    }

    if (s_q && s_task)
    {
        TaskHandle_t decode_task_handle = s_task;
        decode_job_t shutdown = {NULL, 0, NULL, ESP_OK};
        xQueueSend(s_q, &shutdown, portMAX_DELAY);
        int wait = 0;
        while (eTaskGetState(decode_task_handle) != eDeleted && wait < 1000)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            wait++;
        }
        /* 任务自删除后由 Idle 回收其栈，避免紧接着切换到 SD 视频时
         * 内部 RAM 暂未归还。 */
        if (eTaskGetState(decode_task_handle) != eDeleted)
        {
            /* Do not destroy the queue/decoder while JPEG decode may still
             * be running. Force-delete only after the graceful timeout. */
            ESP_LOGW(TAG, "JPEG decode task did not stop in 1 s; deleting it");
            vTaskDelete(decode_task_handle);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
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
    for (int i = 0; i < FP_STRIP_BUFS; i++)
    {
        if (g_fp.strip_buf[i] && lcd_idle)
        {
            heap_caps_free(g_fp.strip_buf[i]);
            g_fp.strip_buf[i] = NULL;
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
