/**
 * @brief  AVI 视频播放器 (双核流水线, tick 化)
 *
 * Core0: SPI (SD读取 + LCD发送) — 主循环每 tick 推进一步
 * Core1: JPEG 解码 (独立 FreeRTOS 任务)
 *
 * 流水线 (每次 video_player_tick() 推进一步):
 *   Step A: 非阻塞检查 Core1 解码完成 → 未完则 return BUSY
 *   Step B: 提交下一帧给 Core1 解码
 *   Step C: 预读下一帧 JPEG 数据
 *   Step D: 非阻塞帧率控制 → 未到时间则 return BUSY
 *   Step E: 非阻塞 LCD TE 同步 → LCD 忙则 return BUSY
 *   Step F: esp_lcd_panel_draw_bitmap (DMA, 立即返回)
 *   Step G: 交换 buffer
 */

#include "video_player.h"
#include "avi.h"
#include "mjpeg.h"
#include "audio.h"
#include "spilcd.h"
#include "spi_sd.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* ====== 用户配置 ====== */
#define ENABLE_AUDIO 0
#define PROF_EVERY   10
/* ===================== */

#define TAG "video_player"

#define DMA_BUF_SIZE   (32 * 1024)
#define FRAME_BUF_SIZE (320 * 240 * sizeof(uint16_t))
#define MAX_JPEG_SIZE  (48 * 1024)

extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;

/* ---- 双核流水线 ---- */
typedef struct {
    const uint8_t *jpeg_data;
    size_t         jpeg_size;
    uint16_t      *out_buf;
    esp_err_t      result;
} decode_job_t;

static QueueHandle_t     s_decode_q    = NULL;
static SemaphoreHandle_t s_decode_done = NULL;
static TaskHandle_t      s_decode_task = NULL;

static void decode_task(void *arg)
{
    decode_job_t job;
    while (1) {
        if (xQueueReceive(s_decode_q, &job, portMAX_DELAY) == pdTRUE) {
            if (job.jpeg_data == NULL) break;
            uint32_t w, h;
            job.result = mjpeg_decoder_decode(job.jpeg_data, job.jpeg_size,
                                               job.out_buf, FRAME_BUF_SIZE, &w, &h);
            xSemaphoreGive(s_decode_done);
        }
    }
    vTaskDelete(NULL);
}

/* ---- 流式读取器 (SD卡) ---- */
typedef struct {
    FIL    *file;
    uint8_t *buf;
    size_t   buf_size, pos, valid;
} sr_t;

static uint8_t *s_refill_buf = NULL;  /* DMA 对齐读取缓冲 */

static esp_err_t sr_open(sr_t *sr, FIL *f, size_t sz)
{
    sr->file = f;
    sr->buf_size = sz;
    sr->pos = sr->valid = 0;
    sr->buf = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!sr->buf) sr->buf = heap_caps_malloc(sz, MALLOC_CAP_DMA);
    if (!sr->buf) sr->buf = malloc(sz);
    return sr->buf ? ESP_OK : ESP_ERR_NO_MEM;
}

static void sr_close(sr_t *sr)
{
    if (sr->buf) { free(sr->buf); sr->buf = NULL; }
}

static esp_err_t sr_refill(sr_t *sr)
{
    if (!s_refill_buf) {
        s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE + 32,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_refill_buf)
            s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE, MALLOC_CAP_DMA);
    }
    uint8_t *ab = s_refill_buf ? s_refill_buf : sr->buf;
    UINT br;
    FRESULT fr = f_read(sr->file, ab, sr->buf_size, &br);
    if (fr) return ESP_FAIL;
    if (ab != sr->buf) memcpy(sr->buf, ab, br);
    sr->pos = 0;
    sr->valid = br;
    return br ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t sr_ensure(sr_t *sr, size_t want)
{
    if (sr->valid - sr->pos < want) {
        if (sr->pos && sr->valid > sr->pos)
            memmove(sr->buf, sr->buf + sr->pos, sr->valid - sr->pos);
        sr->valid -= sr->pos;
        sr->pos = 0;
        if (!s_refill_buf) {
            s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE + 32,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_refill_buf)
                s_refill_buf = heap_caps_malloc(DMA_BUF_SIZE, MALLOC_CAP_DMA);
        }
        uint8_t *ab = s_refill_buf ? s_refill_buf : sr->buf;
        UINT br;
        FRESULT fr = f_read(sr->file, ab, sr->buf_size - sr->valid, &br);
        if (fr) return ESP_FAIL;
        memcpy(sr->buf + sr->valid, ab, br);
        sr->valid += br;
    }
    return (sr->valid - sr->pos >= want) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t sr_read(sr_t *sr, uint8_t *d, size_t n)
{
    if (sr_ensure(sr, n)) return ESP_FAIL;
    memcpy(d, sr->buf + sr->pos, n);
    sr->pos += n;
    return ESP_OK;
}

static void sr_adv(sr_t *sr, size_t n) { sr->pos += n; }

static esp_err_t sr_skip_pad(sr_t *sr, size_t n)
{
    while (n) {
        size_t a = sr->valid - sr->pos;
        if (!a) {
            if (sr_refill(sr)) return ESP_FAIL;
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
    if (!refresh_done_flag) return false;
    refresh_done_flag = 0;
    return true;
}

/* ---- 从流中读取一个 AVI chunk ---- */
static bool read_one_chunk(sr_t *sr, AVI_INFO *avi,
                            uint8_t *jpeg_out, size_t *jpeg_sz_out,
                            int *audio_cnt)
{
    while (1) {
        uint8_t fhdr[8];
        if (sr_read(sr, fhdr, 8)) return false;
        if (avi_get_streaminfo(fhdr, avi) != AVI_OK) return false;

        uint32_t sz = avi->StreamSize;
        if (memcmp(fhdr, avi->VideoFLAG, 4) == 0) {
            if (sr_ensure(sr, sz)) return false;
            if (sz <= MAX_JPEG_SIZE) {
                memcpy(jpeg_out, sr->buf + sr->pos, sz);
                *jpeg_sz_out = sz;
            }
            sr_adv(sr, sz);
            if (sz & 1) sr_skip_pad(sr, 1);
            return true;
        } else if (memcmp(fhdr, avi->AudioFLAG, 4) == 0) {
#if ENABLE_AUDIO
            if (sr_ensure(sr, sz) == ESP_OK) {
                if (avi->AudioType == 1)
                    audio_write_pcm((const int16_t *)(sr->buf + sr->pos),
                                    sz / 2, avi->SampleRate, avi->Channels);
                sr_adv(sr, sz);
            }
#else
            sr_skip_pad(sr, sz);
#endif
            if (sz & 1) sr_skip_pad(sr, 1);
            if (audio_cnt) (*audio_cnt)++;
        } else {
            sr_skip_pad(sr, sz);
            if (sz & 1) sr_skip_pad(sr, 1);
        }
    }
}

/* ====== 播放器上下文 (原 video_player_play 的栈变量) ====== */
typedef struct {
    bool initialized;

    FIL        file;
    sr_t       sr;
    AVI_INFO   avi;

    uint16_t  *frame_buf[2];
    uint8_t   *jpeg_buf[2];
    size_t     jpeg_size[2];

    int        cur, next, pending;
    bool       have_next;

    uint32_t   movi_pos;
    uint16_t   offx, offy;

    /* 帧率控制 */
    int64_t    next_ts;
    bool       first_vf;

    /* 性能分析 */
    uint32_t   frame_count;
    int64_t    start_ts;
    int64_t    t_wait_dec, t_read, t_wait_lcd, t_rate_ctl;
    int        audio_frames, video_chunks, loop_wraps;
} vp_ctx_t;

static vp_ctx_t g_vp = {0};

/* ====== 公开 API ====== */

esp_err_t video_player_init(const char *filename)
{
    if (g_vp.initialized) video_player_stop();
    memset(&g_vp, 0, sizeof(g_vp));

    esp_err_t ret = ESP_OK;

    /* 分配内存 */
    for (int i = 0; i < 2; i++) {
        g_vp.frame_buf[i] = heap_caps_aligned_alloc(16, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_vp.frame_buf[i])
            g_vp.frame_buf[i] = heap_caps_aligned_alloc(16, FRAME_BUF_SIZE, MALLOC_CAP_8BIT);
        if (!g_vp.frame_buf[i]) { ESP_LOGE(TAG, "OOM fb"); ret = ESP_ERR_NO_MEM; goto fail; }

        g_vp.jpeg_buf[i] = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_vp.jpeg_buf[i]) g_vp.jpeg_buf[i] = malloc(MAX_JPEG_SIZE);
        if (!g_vp.jpeg_buf[i]) { ESP_LOGE(TAG, "OOM jb"); ret = ESP_ERR_NO_MEM; goto fail; }
    }

    /* 打开文件, 解析 AVI 头 */
    char fp[272];
    snprintf(fp, sizeof(fp), "0:%s", filename + (strncmp(filename, "/0:/", 4) == 0 ? 4 : 0));
    if (!strchr(fp, ':')) strncpy(fp, filename, sizeof(fp) - 1);
    if (f_open(&g_vp.file, fp, FA_READ)) { ret = ESP_ERR_NOT_FOUND; goto fail; }

    if (sr_open(&g_vp.sr, &g_vp.file, DMA_BUF_SIZE) || sr_refill(&g_vp.sr) || g_vp.sr.valid < 1024) {
        ESP_LOGE(TAG, "header"); ret = ESP_FAIL; goto fail;
    }
    if (avi_init(g_vp.sr.buf, g_vp.sr.valid, &g_vp.avi) != AVI_OK) {
        ret = ESP_FAIL; goto fail;
    }
    if (g_vp.avi.Width > 320 || g_vp.avi.Height > 240) {
        ret = ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    mjpeg_decoder_init(g_vp.avi.Width, g_vp.avi.Height);

    /* 启动 Core1 解码任务 */
    s_decode_q    = xQueueCreate(2, sizeof(decode_job_t));
    s_decode_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(decode_task, "jpeg", 4096, NULL, 5, &s_decode_task, 1);

    g_vp.movi_pos = g_vp.avi.MoviOffset + 4;
    g_vp.offx = (320 - g_vp.avi.Width) / 2;
    g_vp.offy = (240 - g_vp.avi.Height) / 2;

    /* 定位到 movi 数据 */
    f_lseek(&g_vp.file, g_vp.movi_pos);
    g_vp.sr.pos = g_vp.sr.valid = 0;

    /* 预读前两帧 */
    if (!read_one_chunk(&g_vp.sr, &g_vp.avi, g_vp.jpeg_buf[0], &g_vp.jpeg_size[0], NULL) ||
        !read_one_chunk(&g_vp.sr, &g_vp.avi, g_vp.jpeg_buf[1], &g_vp.jpeg_size[1], NULL)) {
        ESP_LOGE(TAG, "prefetch fail"); ret = ESP_FAIL; goto fail;
    }

    /* 提交 F0 到 Core1 解码 */
    decode_job_t job0 = { g_vp.jpeg_buf[0], g_vp.jpeg_size[0], g_vp.frame_buf[0], ESP_OK };
    xQueueSend(s_decode_q, &job0, 0);

    g_vp.first_vf   = true;
    g_vp.have_next  = true;
    g_vp.pending    = 1;
    g_vp.cur        = 0;
    g_vp.next       = 1;
    g_vp.start_ts   = esp_timer_get_time();
    g_vp.next_ts    = g_vp.start_ts + g_vp.avi.SecPerFrame;
    g_vp.initialized = true;

    ESP_LOGI(TAG, "Init OK: %lux%lu, %lu us/frame",
             g_vp.avi.Width, g_vp.avi.Height, g_vp.avi.SecPerFrame);
    return ESP_OK;

fail:
    video_player_stop();
    return ret;
}

player_ret_t video_player_tick(void)
{
    if (!g_vp.initialized) return PLAYER_ERROR;

    /* Step A: 非阻塞检查 Core1 解码完成 */
    if (xSemaphoreTake(s_decode_done, 0) != pdTRUE)
        return PLAYER_BUSY;
    g_vp.pending--;

    int64_t t0 = esp_timer_get_time();
    (void)t0; /* unused for individual profiling in tick mode */

    /* Step B: 提交 frame[next] 解码 */
    if (g_vp.have_next) {
        decode_job_t job = {
            g_vp.jpeg_buf[g_vp.next], g_vp.jpeg_size[g_vp.next],
            g_vp.frame_buf[g_vp.next], ESP_OK
        };
        xQueueSend(s_decode_q, &job, 0);
        g_vp.pending++;
    }

    /* Step C: 预读下一帧 JPEG → jpeg_buf[cur] */
    int aud = 0;
    g_vp.have_next = read_one_chunk(&g_vp.sr, &g_vp.avi,
                                     g_vp.jpeg_buf[g_vp.cur],
                                     &g_vp.jpeg_size[g_vp.cur], &aud);
    g_vp.audio_frames += aud;
    if (!g_vp.have_next) {
        /* 无缝绕回 movi 头 */
        f_lseek(&g_vp.file, g_vp.movi_pos);
        g_vp.sr.pos = g_vp.sr.valid = 0;
        g_vp.have_next = read_one_chunk(&g_vp.sr, &g_vp.avi,
                                         g_vp.jpeg_buf[g_vp.cur],
                                         &g_vp.jpeg_size[g_vp.cur], &aud);
        g_vp.loop_wraps++;
    }
    if (g_vp.have_next) g_vp.video_chunks++;

    /* Step D: 非阻塞帧率控制 */
    if (!g_vp.first_vf) {
        if (esp_timer_get_time() < g_vp.next_ts)
            return PLAYER_BUSY;
        g_vp.next_ts += g_vp.avi.SecPerFrame;
    } else {
        g_vp.first_vf = false;
        g_vp.next_ts = esp_timer_get_time() + g_vp.avi.SecPerFrame;
    }

    /* Step E: 非阻塞 LCD TE 同步 */
    if (!lcd_is_ready())
        return PLAYER_BUSY;

    /* Step F: 发送帧到 LCD (DMA 异步) */
    esp_lcd_panel_draw_bitmap(panel_handle,
                               g_vp.offx, g_vp.offy,
                               g_vp.offx + g_vp.avi.Width,
                               g_vp.offy + g_vp.avi.Height,
                               g_vp.frame_buf[g_vp.cur]);
    g_vp.frame_count++;

    /* 定时分析 */
    if (g_vp.frame_count % PROF_EVERY == 0) {
        int64_t elapsed = esp_timer_get_time() - g_vp.start_ts;
        ESP_LOGI(TAG, "--- profile %lu frames (wrap %d) ---",
                 g_vp.frame_count, g_vp.loop_wraps);
        ESP_LOGI(TAG, "  avg fps: %.1f", g_vp.frame_count * 1e6 / elapsed);
    }

    /* Step G: 交换 buffer */
    g_vp.cur  = g_vp.next;
    g_vp.next = 1 - g_vp.next;

    return PLAYER_OK;
}

void video_player_stop(void)
{
    if (!g_vp.initialized) return;

    /* 通知解码任务退出 */
    if (s_decode_q && s_decode_task) {
        decode_job_t shutdown = { NULL, 0, NULL, ESP_OK };
        xQueueSend(s_decode_q, &shutdown, portMAX_DELAY);
        int wait = 0;
        while (eTaskGetState(s_decode_task) != eDeleted && wait < 100) {
            vTaskDelay(pdMS_TO_TICKS(1)); wait++;
        }
        s_decode_task = NULL;
    }
    if (s_decode_q)   { vQueueDelete(s_decode_q);   s_decode_q   = NULL; }
    if (s_decode_done){ vSemaphoreDelete(s_decode_done); s_decode_done = NULL; }

    mjpeg_decoder_deinit();

    if (g_vp.file.obj.fs) f_close(&g_vp.file);
    sr_close(&g_vp.sr);

    for (int i = 0; i < 2; i++) {
        if (g_vp.frame_buf[i]) { heap_caps_free(g_vp.frame_buf[i]); g_vp.frame_buf[i] = NULL; }
        if (g_vp.jpeg_buf[i])  { free(g_vp.jpeg_buf[i]);           g_vp.jpeg_buf[i]  = NULL; }
    }

    g_vp.initialized = false;
    ESP_LOGI(TAG, "Stopped");
}

/* ---- 兼容旧 API (阻塞版) ---- */
esp_err_t video_player_play(const char *filename)
{
    esp_err_t ret = video_player_init(filename);
    if (ret != ESP_OK) return ret;

    while (1) {
        player_ret_t r = video_player_tick();
        if (r == PLAYER_ERROR) {
            video_player_stop();
            return ESP_FAIL;
        }
        /* PLAYER_BUSY → continue looping (non-blocking in tick mode,
         * but in blocking wrapper we just retry) */
        if (r == PLAYER_BUSY) {
            vTaskDelay(1);
        }
    }
}
