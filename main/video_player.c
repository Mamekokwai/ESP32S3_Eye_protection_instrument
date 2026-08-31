/**
 * @brief  AVI 视频播放器 (双核流水线, tick 化)
 *
 * Core0: SD读取 + JPEG解码任务 + LCD发送
 * Core1: 保留给独立音频任务，避免视频解码与音频争抢 CPU
 *
 * 流水线 (每次 video_player_tick() 推进一步):
 *   Step A: 非阻塞检查 CPU0 解码任务完成 → 未完则 return BUSY
 *   Step B: 提交下一帧给 CPU0 解码任务
 *   Step C: 预读下一帧 JPEG 数据
 *   Step D: 非阻塞帧率控制 → 未到时间则 return BUSY
 *   Step E: 非阻塞 LCD TE 同步 → LCD 忙则 return BUSY
 *   Step F: esp_lcd_panel_draw_bitmap (DMA, 立即返回)
 *   Step G: 交换 buffer
 */

#include "video_player.h"
#include "avi.h"
#include "mjpeg.h"
#include "media_catalog.h"
#include "spilcd.h"
#include "ff.h"
#include "esp_cache.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* ====== 用户配置 ====== */
#define PROF_EVERY 100
/* ===================== */

#define TAG "video_player"

#define DMA_BUF_SIZE (32 * 1024)
#define FRAME_BUF_SIZE (320 * 320 * sizeof(uint16_t))
#define MAX_JPEG_SIZE (96 * 1024)
#define VP_STRIP_H 160 // 写入条带高度
#define VP_STRIP_BYTES (VP_STRIP_H * 320 * sizeof(uint16_t))
#define VP_STRIP_BUFS 1
#define VP_DECODE_TASK_CORE 1
#define VP_DECODE_TASK_PRIORITY 4 /* audio service priority is 5 */

extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;
extern volatile uint32_t refresh_done_count;

/* ---- 双核流水线 ---- */
typedef struct
{
    const uint8_t *jpeg_data;
    size_t jpeg_size;
    uint16_t *out_buf;
    esp_err_t result;
} decode_job_t;

static QueueHandle_t s_decode_q = NULL;
static SemaphoreHandle_t s_decode_done = NULL;
static TaskHandle_t s_decode_task = NULL;
static esp_err_t s_decode_result = ESP_OK;
static uint32_t s_decode_width = 0;
static uint32_t s_decode_height = 0;
static volatile uint32_t s_decode_started = 0;
static volatile uint32_t s_decode_completed = 0;
static volatile int64_t s_decode_time_us = 0;
static volatile int64_t s_decode_cache_time_us = 0;

static void decode_task(void *arg)
{
    decode_job_t job;
    while (1)
    {
        if (xQueueReceive(s_decode_q, &job, portMAX_DELAY) == pdTRUE)
        {
            if (job.jpeg_data == NULL)
                break;
            s_decode_started++;
            if (s_decode_started == 1)
                ESP_LOGI(TAG, "First JPEG decode started on CPU%d",
                         xPortGetCoreID());
            int64_t decode_started_at = esp_timer_get_time();
            s_decode_result = mjpeg_decoder_decode(
                job.jpeg_data, job.jpeg_size, job.out_buf, FRAME_BUF_SIZE,
                &s_decode_width, &s_decode_height);
            s_decode_time_us += esp_timer_get_time() - decode_started_at;
            if (s_decode_result == ESP_OK)
            {
                /*
                 * 解码输出位于 PSRAM。显式回写并失效 cache，避免随后
                 * 的 SRAM 条带复制读到尚未提交的外部 RAM 数据。
                 */
                int64_t cache_started_at = esp_timer_get_time();
                esp_cache_msync(
                    job.out_buf, FRAME_BUF_SIZE,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                s_decode_cache_time_us +=
                    esp_timer_get_time() - cache_started_at;
            }
            s_decode_completed++;
            if (s_decode_completed == 1)
                ESP_LOGI(TAG, "First JPEG decode completed: %s",
                         esp_err_to_name(s_decode_result));
            xSemaphoreGive(s_decode_done);
        }
    }
    vTaskDelete(NULL);
}

/* ---- 流式读取器 (SD卡) ---- */
typedef struct
{
    FIL *file;
    uint8_t *buf;
    size_t buf_size, pos, valid;
} sr_t;

static uint8_t *s_refill_buf = NULL; /* DMA 对齐读取缓冲 */
/* LCD DMA 条带缓冲在视频切换时持久复用。停止瞬间 DMA 可能尚未回调，
 * 因此不能释放后立即重建，也不能丢失其指针。 */
static uint16_t *s_strip_buf[VP_STRIP_BUFS];

static esp_err_t sr_open(sr_t *sr, FIL *f, size_t sz)
{
    sr->file = f;
    sr->buf_size = sz;
    sr->pos = sr->valid = 0;
    sr->buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!sr->buf)
        sr->buf = heap_caps_malloc(sz, MALLOC_CAP_DMA);
    if (!sr->buf)
        sr->buf = malloc(sz);
    return sr->buf ? ESP_OK : ESP_ERR_NO_MEM;
}

static void sr_close(sr_t *sr)
{
    if (sr->buf)
    {
        free(sr->buf);
        sr->buf = NULL;
    }
}

static esp_err_t sr_refill(sr_t *sr)
{
    if (!s_refill_buf)
    {
        s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE + 32,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_refill_buf)
            s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE, MALLOC_CAP_DMA);
    }
    uint8_t *ab = s_refill_buf ? s_refill_buf : sr->buf;
    UINT br;
    FRESULT fr = f_read(sr->file, ab, sr->buf_size, &br);
    if (fr)
        return ESP_FAIL;
    if (ab != sr->buf)
        memcpy(sr->buf, ab, br);
    sr->pos = 0;
    sr->valid = br;
    return br ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t sr_ensure(sr_t *sr, size_t want)
{
    if (sr->valid - sr->pos < want)
    {
        if (sr->pos && sr->valid > sr->pos)
            memmove(sr->buf, sr->buf + sr->pos, sr->valid - sr->pos);
        sr->valid -= sr->pos;
        sr->pos = 0;
        if (!s_refill_buf)
        {
            s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE + 32,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_refill_buf)
                s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE, MALLOC_CAP_DMA);
        }
        uint8_t *ab = s_refill_buf
                          ? s_refill_buf
                          : sr->buf + sr->valid;
        UINT br;
        FRESULT fr = f_read(sr->file, ab, sr->buf_size - sr->valid, &br);
        if (fr)
            return ESP_FAIL;
        if (s_refill_buf)
            memcpy(sr->buf + sr->valid, ab, br);
        sr->valid += br;
    }
    return (sr->valid - sr->pos >= want) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t sr_read(sr_t *sr, uint8_t *d, size_t n)
{
    if (sr_ensure(sr, n))
        return ESP_FAIL;
    memcpy(d, sr->buf + sr->pos, n);
    sr->pos += n;
    return ESP_OK;
}

static esp_err_t sr_read_bytes(sr_t *sr, uint8_t *dst, size_t n)
{
    while (n)
    {
        size_t available = sr->valid - sr->pos;
        if (available == 0)
        {
            if (sr_refill(sr) != ESP_OK)
                return ESP_FAIL;
            available = sr->valid - sr->pos;
        }
        size_t take = n < available ? n : available;
        memcpy(dst, sr->buf + sr->pos, take);
        sr->pos += take;
        dst += take;
        n -= take;
    }
    return ESP_OK;
}

static esp_err_t sr_skip_pad(sr_t *sr, size_t n)
{
    while (n)
    {
        size_t a = sr->valid - sr->pos;
        if (!a)
        {
            if (sr_refill(sr))
                return ESP_FAIL;
            a = sr->valid - sr->pos;
        }
        size_t s = n < a ? n : a;
        sr->pos += s;
        n -= s;
    }
    return ESP_OK;
}

/* ---- 非阻塞 LCD 检查 ---- */
static inline bool lcd_is_ready(void)
{
    if (!refresh_done_flag)
        return false;
    refresh_done_flag = 0;
    return true;
}

/* ---- 从流中读取一个 AVI chunk ---- */
static bool read_one_chunk(sr_t *sr, AVI_INFO *avi,
                           uint8_t *jpeg_out, size_t *jpeg_sz_out)
{
    while (1)
    {
        uint8_t fhdr[8];
        if (sr_read(sr, fhdr, 8))
            return false;
        if (avi_get_streaminfo(fhdr, avi) != AVI_OK)
            return false;

        uint32_t sz = avi->StreamSize;
        if (memcmp(fhdr, avi->VideoFLAG, 4) == 0)
        {
            if (sz <= MAX_JPEG_SIZE)
            {
                if (sr_read_bytes(sr, jpeg_out, sz) != ESP_OK)
                    return false;
                *jpeg_sz_out = sz;
            }
            else
            {
                if (sr_skip_pad(sr, sz) != ESP_OK)
                    return false;
                if (sz & 1)
                    sr_skip_pad(sr, 1);
                continue;
            }
            if (sz & 1)
                sr_skip_pad(sr, 1);
            return true;
        }
        else if (memcmp(fhdr, avi->AudioFLAG, 4) == 0)
        {
            /* 视频播放器只处理 MJPEG 图像；AVI 内的音频块始终跳过。 */
            sr_skip_pad(sr, sz);
            if (sz & 1)
                sr_skip_pad(sr, 1);
        }
        else
        {
            sr_skip_pad(sr, sz);
            if (sz & 1)
                sr_skip_pad(sr, 1);
        }
    }
}

/* ====== 播放器上下文 (原 video_player_play 的栈变量) ====== */
typedef struct
{
    bool initialized;

    FIL file;
    sr_t sr;
    AVI_INFO avi;

    uint16_t *frame_buf[2];
    uint8_t *jpeg_buf[2];
    size_t jpeg_size[2];
    uint16_t *strip_buf[VP_STRIP_BUFS];
    uint8_t strip_buf_count;

    int cur, next;
    bool have_next;
    bool frame_decoded;
    bool next_queued;

    uint32_t movi_pos;
    uint16_t offx, offy;

    /* 帧率控制 */
    int64_t next_ts;
    bool first_vf;

    /* 性能分析 */
    uint32_t frame_count;
    int64_t start_ts;
    int64_t stats_ts;
    int loop_wraps;

    /* 每 PROF_EVERY 帧输出一次的性能累计值。 */
    int64_t prof_decode_base;
    int64_t prof_decode_cache_base;
    int64_t prof_wait_decode;
    int64_t prof_read;
    int64_t prof_cache;
    int64_t prof_rate_wait;
    int64_t prof_lcd_gate_wait;
    int64_t prof_copy;
    int64_t prof_lcd_submit;
    int64_t prof_lcd_refresh;
    int64_t wait_decode_started_at;
    int64_t rate_wait_started_at;
    int64_t lcd_gate_wait_started_at;

    int16_t strip_ys;
    uint16_t strip_submitted;
    uint32_t lcd_done_base;
    int64_t last_write_ts;
    int64_t decode_wait_log_ts;
    char name[MEDIA_CATALOG_PATH_MAX];
} vp_ctx_t;

static vp_ctx_t g_vp = {0};

/* ====== 公开 API ====== */

int video_player_list_files(char *output, size_t output_size)
{
    return media_catalog_list(MEDIA_VIDEO, output, output_size);
}

esp_err_t video_player_start(const char *selection)
{
    char name[MEDIA_CATALOG_PATH_MAX];
    esp_err_t ret = media_catalog_resolve(
        MEDIA_VIDEO, selection, name, sizeof(name));
    if (ret != ESP_OK)
        return ret;

    ret = video_player_init(name);
    if (ret == ESP_OK)
        snprintf(g_vp.name, sizeof(g_vp.name), "%s", name);
    return ret;
}

const char *video_player_name(void)
{
    return g_vp.name[0] ? g_vp.name : NULL;
}

esp_err_t video_player_init(const char *filename)
{
    if (!filename || !filename[0])
        return ESP_ERR_INVALID_ARG;
    if (g_vp.initialized)
        video_player_stop();
    memset(&g_vp, 0, sizeof(g_vp));
    g_vp.initialized = true;

    esp_err_t ret = ESP_OK;

    /* 分配内存 */
    for (int i = 0; i < 2; i++)
    {
        g_vp.frame_buf[i] = heap_caps_aligned_alloc(
            64, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_vp.frame_buf[i])
        {
            ESP_LOGE(TAG, "OOM fb");
            ret = ESP_ERR_NO_MEM;
            goto fail;
        }

        g_vp.jpeg_buf[i] = heap_caps_aligned_alloc(
            64, MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_vp.jpeg_buf[i])
        {
            ESP_LOGE(TAG, "OOM jb");
            ret = ESP_ERR_NO_MEM;
            goto fail;
        }
    }
    for (int i = 0; i < VP_STRIP_BUFS; i++)
    {
        if (!s_strip_buf[i])
            s_strip_buf[i] = heap_caps_aligned_alloc(
                64, VP_STRIP_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        g_vp.strip_buf[i] = s_strip_buf[i];
        if (!g_vp.strip_buf[i])
        {
            ESP_LOGE(TAG, "OOM LCD strip buffer");
            break;
        }
        g_vp.strip_buf_count++;
    }
    if (g_vp.strip_buf_count == 0)
    {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* 打开文件, 解析 AVI 头 */
    char fp[MEDIA_CATALOG_PATH_MAX + 4];
    if (strncmp(filename, "/0:/", 4) == 0)
        snprintf(fp, sizeof(fp), "0:/%s", filename + 4);
    else if (strncmp(filename, "0:/", 3) == 0)
        snprintf(fp, sizeof(fp), "%s", filename);
    else
        snprintf(fp, sizeof(fp), "0:/%s", filename[0] == '/' ? filename + 1 : filename);
    if (f_open(&g_vp.file, fp, FA_READ))
    {
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    if (sr_open(&g_vp.sr, &g_vp.file, DMA_BUF_SIZE) || sr_refill(&g_vp.sr) || g_vp.sr.valid < 1024)
    {
        ESP_LOGE(TAG, "header");
        ret = ESP_FAIL;
        goto fail;
    }
    if (avi_init(g_vp.sr.buf, g_vp.sr.valid, &g_vp.avi) != AVI_OK)
    {
        ret = ESP_FAIL;
        goto fail;
    }
    if (g_vp.avi.Width == 0 || g_vp.avi.Height == 0 ||
        g_vp.avi.Width > 320 || g_vp.avi.Height > 320 ||
        g_vp.avi.SecPerFrame == 0)
    {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    ret = mjpeg_decoder_init(g_vp.avi.Width, g_vp.avi.Height);
    if (ret != ESP_OK)
        goto fail;

    /* 视频解码留在 CPU0；CPU1 专用于独立音频服务。 */
    /* CPU0 performs SD reads in app_main; CPU1 decodes concurrently.
     * The decoder stays below the CPU1 audio task priority. */
    s_decode_q = xQueueCreate(1, sizeof(decode_job_t));
    s_decode_done = xSemaphoreCreateBinary();
    if (!s_decode_q || !s_decode_done ||
        xTaskCreatePinnedToCore(decode_task, "jpeg_sd", 4096, NULL,
                                VP_DECODE_TASK_PRIORITY, &s_decode_task,
                                VP_DECODE_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "OOM JPEG decode task");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    g_vp.movi_pos = g_vp.avi.MoviOffset + 4;
    g_vp.offx = (320 - g_vp.avi.Width) / 2;
    g_vp.offy = (320 - g_vp.avi.Height) / 2;

    /* 定位到 movi 数据 */
    f_lseek(&g_vp.file, g_vp.movi_pos);
    g_vp.sr.pos = g_vp.sr.valid = 0;

    /* 预读前两帧 */
    if (!read_one_chunk(&g_vp.sr, &g_vp.avi, g_vp.jpeg_buf[0], &g_vp.jpeg_size[0]) ||
        !read_one_chunk(&g_vp.sr, &g_vp.avi, g_vp.jpeg_buf[1], &g_vp.jpeg_size[1]))
    {
        ESP_LOGE(TAG, "prefetch fail");
        ret = ESP_FAIL;
        goto fail;
    }

    /* 提交 F0 到 CPU0 解码任务 */
    s_decode_started = 0;
    s_decode_completed = 0;
    s_decode_time_us = 0;
    s_decode_cache_time_us = 0;
    ret = esp_cache_msync(
        g_vp.jpeg_buf[0], MAX_JPEG_SIZE,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
            ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (ret != ESP_OK)
        goto fail;
    decode_job_t job0 = {g_vp.jpeg_buf[0], g_vp.jpeg_size[0], g_vp.frame_buf[0], ESP_OK};
    if (xQueueSend(s_decode_q, &job0, 0) != pdTRUE)
    {
        ret = ESP_FAIL;
        goto fail;
    }

    g_vp.first_vf = true;
    g_vp.have_next = true;
    g_vp.frame_decoded = false;
    g_vp.next_queued = false;
    g_vp.cur = 0;
    g_vp.next = 1;
    g_vp.strip_ys = -1;
    g_vp.start_ts = esp_timer_get_time();
    g_vp.stats_ts = g_vp.start_ts;
    g_vp.prof_decode_base = 0;
    g_vp.prof_decode_cache_base = 0;
    g_vp.next_ts = g_vp.start_ts + g_vp.avi.SecPerFrame;
    g_vp.decode_wait_log_ts = g_vp.start_ts;
    refresh_done_flag = 1;

    ESP_LOGI(TAG, "Init OK: %lux%lu, %lu us/frame",
             g_vp.avi.Width, g_vp.avi.Height, g_vp.avi.SecPerFrame);
    return ESP_OK;

fail:
    video_player_stop();
    return ret;
}

player_ret_t video_player_tick(void)
{
    if (!g_vp.initialized)
        return PLAYER_ERROR;

    if (g_vp.strip_ys < 0)
    {
        if (!g_vp.frame_decoded)
        {
            if (xSemaphoreTake(s_decode_done, 0) != pdTRUE)
            {
                int64_t now = esp_timer_get_time();
                if (g_vp.wait_decode_started_at == 0)
                    g_vp.wait_decode_started_at = now;
                if (now - g_vp.decode_wait_log_ts >= 1000000)
                {
                    ESP_LOGW(TAG,
                             "Waiting JPEG decode: started=%lu completed=%lu",
                             (unsigned long)s_decode_started,
                             (unsigned long)s_decode_completed);
                    g_vp.decode_wait_log_ts = now;
                }
                return PLAYER_BUSY;
            }
            if (g_vp.wait_decode_started_at != 0)
            {
                g_vp.prof_wait_decode +=
                    esp_timer_get_time() - g_vp.wait_decode_started_at;
                g_vp.wait_decode_started_at = 0;
            }
            if (s_decode_result != ESP_OK ||
                s_decode_width != g_vp.avi.Width ||
                s_decode_height != g_vp.avi.Height)
            {
                ESP_LOGE(TAG, "decode failed: %s, %lux%lu",
                         esp_err_to_name(s_decode_result),
                         (unsigned long)s_decode_width,
                         (unsigned long)s_decode_height);
                return PLAYER_ERROR;
            }
            g_vp.frame_decoded = true;
        }

        if (!g_vp.next_queued)
        {
            if (!g_vp.have_next)
                return PLAYER_ERROR;
            decode_job_t job = {
                g_vp.jpeg_buf[g_vp.next], g_vp.jpeg_size[g_vp.next],
                g_vp.frame_buf[g_vp.next], ESP_OK};
            int64_t stage_started_at = esp_timer_get_time();
            esp_err_t sync_ret = esp_cache_msync(
                g_vp.jpeg_buf[g_vp.next], MAX_JPEG_SIZE,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            g_vp.prof_cache += esp_timer_get_time() - stage_started_at;
            if (sync_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "JPEG cache sync failed: %s",
                         esp_err_to_name(sync_ret));
                return PLAYER_ERROR;
            }
            if (xQueueSend(s_decode_q, &job, 0) != pdTRUE)
                return PLAYER_BUSY;

            stage_started_at = esp_timer_get_time();
            g_vp.have_next = read_one_chunk(
                &g_vp.sr, &g_vp.avi, g_vp.jpeg_buf[g_vp.cur],
                &g_vp.jpeg_size[g_vp.cur]);
            if (!g_vp.have_next)
            {
                f_lseek(&g_vp.file, g_vp.movi_pos);
                g_vp.sr.pos = g_vp.sr.valid = 0;
                g_vp.have_next = read_one_chunk(
                    &g_vp.sr, &g_vp.avi, g_vp.jpeg_buf[g_vp.cur],
                    &g_vp.jpeg_size[g_vp.cur]);
                g_vp.loop_wraps++;
            }
            g_vp.prof_read += esp_timer_get_time() - stage_started_at;
            g_vp.next_queued = true;
        }

        if (!g_vp.first_vf)
        {
            int64_t now = esp_timer_get_time();
            if (now < g_vp.next_ts)
            {
                if (g_vp.rate_wait_started_at == 0)
                    g_vp.rate_wait_started_at = now;
                return PLAYER_BUSY;
            }
            if (g_vp.rate_wait_started_at != 0)
            {
                g_vp.prof_rate_wait += now - g_vp.rate_wait_started_at;
                g_vp.rate_wait_started_at = 0;
            }
            g_vp.next_ts += g_vp.avi.SecPerFrame;
        }
        else
        {
            g_vp.first_vf = false;
            g_vp.next_ts = esp_timer_get_time() + g_vp.avi.SecPerFrame;
        }

        int64_t now = esp_timer_get_time();
        if (g_vp.last_write_ts && now < g_vp.last_write_ts + 17000)
        {
            if (g_vp.lcd_gate_wait_started_at == 0)
                g_vp.lcd_gate_wait_started_at = now;
            return PLAYER_BUSY;
        }
        if (!lcd_is_ready())
        {
            if (g_vp.lcd_gate_wait_started_at == 0)
                g_vp.lcd_gate_wait_started_at = now;
            return PLAYER_BUSY;
        }
        if (g_vp.lcd_gate_wait_started_at != 0)
        {
            g_vp.prof_lcd_gate_wait +=
                now - g_vp.lcd_gate_wait_started_at;
            g_vp.lcd_gate_wait_started_at = 0;
        }

        int64_t te_started_at = esp_timer_get_time();
        spilcd_wait_te();
        g_vp.prof_lcd_gate_wait += esp_timer_get_time() - te_started_at;
        g_vp.strip_ys = 0;
        g_vp.strip_submitted = 0;
        g_vp.lcd_done_base = refresh_done_count;
        refresh_done_flag = 0;
        g_vp.last_write_ts = esp_timer_get_time();
    }

    int ys = g_vp.strip_ys;
    uint32_t completed = refresh_done_count - g_vp.lcd_done_base;
    if (completed > g_vp.strip_submitted)
        completed = g_vp.strip_submitted;

    if (ys >= (int)g_vp.avi.Height)
    {
        if (completed < g_vp.strip_submitted)
            return PLAYER_BUSY;

        int64_t now = esp_timer_get_time();
        g_vp.prof_lcd_refresh += now - g_vp.last_write_ts;
        g_vp.strip_ys = -1;
        g_vp.frame_count++;
        if (g_vp.frame_count % PROF_EVERY == 0)
        {
            int64_t window = now - g_vp.stats_ts;
            int64_t decode = s_decode_time_us - g_vp.prof_decode_base;
            int64_t cache = g_vp.prof_cache +
                            s_decode_cache_time_us -
                            g_vp.prof_decode_cache_base;
            double percent_scale = window > 0 ? 100.0 / window : 0.0;
            ESP_LOGI(TAG, "--- VID profile %d frames (total=%lu wrap=%d) ---",
                     PROF_EVERY, (unsigned long)g_vp.frame_count,
                     g_vp.loop_wraps);
            ESP_LOGI(TAG, "  window/avg    : %lld ms, %.1f fps, %.2f ms/frame",
                     window / 1000,
                     PROF_EVERY * 1000000.0 / window,
                     window / (1000.0 * PROF_EVERY));
            ESP_LOGI(TAG, "  wait decode   : %lld ms (%.1f%%)",
                     g_vp.prof_wait_decode / 1000,
                     g_vp.prof_wait_decode * percent_scale);
            ESP_LOGI(TAG, "  SD read       : %lld ms (%.1f%%)",
                     g_vp.prof_read / 1000,
                     g_vp.prof_read * percent_scale);
            ESP_LOGI(TAG, "  rate control  : %lld ms (%.1f%%)",
                     g_vp.prof_rate_wait / 1000,
                     g_vp.prof_rate_wait * percent_scale);
            ESP_LOGI(TAG, "  LCD gate wait : %lld ms (%.1f%%)",
                     g_vp.prof_lcd_gate_wait / 1000,
                     g_vp.prof_lcd_gate_wait * percent_scale);
            ESP_LOGI(TAG, "  LCD refresh   : %lld ms (%.1f%%)",
                     g_vp.prof_lcd_refresh / 1000,
                     g_vp.prof_lcd_refresh * percent_scale);
            ESP_LOGI(TAG, "  JPEG decode*  : %lld ms (%.1f%%)",
                     decode / 1000, decode * percent_scale);
            ESP_LOGI(TAG, "  cache sync*   : %lld ms (%.1f%%)",
                     cache / 1000, cache * percent_scale);
            ESP_LOGI(TAG, "  PSRAM->SRAM*  : %lld ms (%.1f%%)",
                     g_vp.prof_copy / 1000,
                     g_vp.prof_copy * percent_scale);
            ESP_LOGI(TAG, "  LCD submit*   : %lld ms (%.1f%%)",
                     g_vp.prof_lcd_submit / 1000,
                     g_vp.prof_lcd_submit * percent_scale);
            ESP_LOGI(TAG, "  * CPU/async stages may overlap wall-time groups");

            g_vp.stats_ts = now;
            g_vp.prof_decode_base = s_decode_time_us;
            g_vp.prof_decode_cache_base = s_decode_cache_time_us;
            g_vp.prof_wait_decode = 0;
            g_vp.prof_read = 0;
            g_vp.prof_cache = 0;
            g_vp.prof_rate_wait = 0;
            g_vp.prof_lcd_gate_wait = 0;
            g_vp.prof_copy = 0;
            g_vp.prof_lcd_submit = 0;
            g_vp.prof_lcd_refresh = 0;
        }

        g_vp.cur = g_vp.next;
        g_vp.next = 1 - g_vp.next;
        g_vp.frame_decoded = false;
        g_vp.next_queued = false;
        return PLAYER_OK;
    }

    if (g_vp.strip_submitted - completed >= g_vp.strip_buf_count)
        return PLAYER_BUSY;

    int rows = (ys + VP_STRIP_H > (int)g_vp.avi.Height)
                   ? (int)g_vp.avi.Height - ys
                   : VP_STRIP_H;
    uint16_t *strip =
        g_vp.strip_buf[g_vp.strip_submitted % g_vp.strip_buf_count];
    size_t bytes = (size_t)rows * g_vp.avi.Width * sizeof(uint16_t);
    if (ys == 0)
    {
        int64_t cache_started_at = esp_timer_get_time();
        esp_err_t sync_ret = esp_cache_msync(
            g_vp.frame_buf[g_vp.cur], FRAME_BUF_SIZE,
            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        g_vp.prof_cache += esp_timer_get_time() - cache_started_at;
        if (sync_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Frame cache sync failed: %s",
                     esp_err_to_name(sync_ret));
            return PLAYER_ERROR;
        }
    }
    int64_t copy_started_at = esp_timer_get_time();
    memcpy(strip, g_vp.frame_buf[g_vp.cur] + ys * g_vp.avi.Width, bytes);
    g_vp.prof_copy += esp_timer_get_time() - copy_started_at;

    /*
     * 与 IMG 已验证路径保持一致：每条带独立设置窗口并使用 RAMWR。
     * SD 视频优先保证正确性，不使用双缓冲 RAMWRC 连续写。
     */
    int64_t submit_started_at = esp_timer_get_time();
    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        panel_handle,
        g_vp.offx, g_vp.offy + ys,
        g_vp.offx + g_vp.avi.Width, g_vp.offy + ys + rows,
        strip);
    g_vp.prof_lcd_submit += esp_timer_get_time() - submit_started_at;
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD submit failed: %s", esp_err_to_name(ret));
        return PLAYER_ERROR;
    }
    if (g_vp.frame_count == 0 && g_vp.strip_submitted == 0)
        ESP_LOGI(TAG, "First LCD strip submitted");
    g_vp.strip_submitted++;
    g_vp.strip_ys = ys + rows;
    return PLAYER_BUSY;
}

void video_player_stop(void)
{
    if (!g_vp.initialized)
        return;

    bool lcd_idle = true;
    if (g_vp.strip_submitted > 0)
    {
        int wait = 0;
        while (refresh_done_count - g_vp.lcd_done_base <
                   g_vp.strip_submitted &&
               wait++ < 500)
            vTaskDelay(pdMS_TO_TICKS(1));
        lcd_idle = refresh_done_count - g_vp.lcd_done_base >=
                   g_vp.strip_submitted;
    }

    /* 通知解码任务退出 */
    if (s_decode_q && s_decode_task)
    {
        decode_job_t shutdown = {NULL, 0, NULL, ESP_OK};
        xQueueSend(s_decode_q, &shutdown, portMAX_DELAY);
        int wait = 0;
        while (eTaskGetState(s_decode_task) != eDeleted && wait < 100)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            wait++;
        }
        /* 自删除任务的栈由 Idle 任务回收；让出一个 tick，避免下一次
         * VID 立即创建 jpeg_sd 时误报 ESP_ERR_NO_MEM。 */
        vTaskDelay(pdMS_TO_TICKS(2));
        s_decode_task = NULL;
    }
    if (s_decode_q)
    {
        vQueueDelete(s_decode_q);
        s_decode_q = NULL;
    }
    if (s_decode_done)
    {
        vSemaphoreDelete(s_decode_done);
        s_decode_done = NULL;
    }

    mjpeg_decoder_deinit();

    if (g_vp.file.obj.fs)
        f_close(&g_vp.file);
    sr_close(&g_vp.sr);

    for (int i = 0; i < 2; i++)
    {
        if (g_vp.frame_buf[i])
        {
            heap_caps_free(g_vp.frame_buf[i]);
            g_vp.frame_buf[i] = NULL;
        }
        if (g_vp.jpeg_buf[i])
        {
            free(g_vp.jpeg_buf[i]);
            g_vp.jpeg_buf[i] = NULL;
        }
    }
    if (!lcd_idle)
        ESP_LOGW(TAG, "LCD DMA stop timeout; retaining persistent strip buffer");
    for (int i = 0; i < VP_STRIP_BUFS; i++)
        g_vp.strip_buf[i] = NULL;

    g_vp.initialized = false;
    ESP_LOGI(TAG, "Stopped");
}

/* ---- 兼容旧 API (阻塞版) ---- */
esp_err_t video_player_play(const char *filename)
{
    esp_err_t ret = video_player_init(filename);
    if (ret != ESP_OK)
        return ret;

    while (1)
    {
        player_ret_t r = video_player_tick();
        if (r == PLAYER_ERROR)
        {
            video_player_stop();
            return ESP_FAIL;
        }
        /* PLAYER_BUSY → continue looping (non-blocking in tick mode,
         * but in blocking wrapper we just retry) */
        if (r == PLAYER_BUSY)
        {
            vTaskDelay(1);
        }
    }
}
