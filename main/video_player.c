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
#include "spilcd.h"
#include "spi_sd.h"
#include "esp_lcd_jd9855.h"
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/* ====== 用户配置 ====== */
#define PROF_EVERY   100
/* ===================== */

#define TAG "video_player"

#define DMA_BUF_SIZE   (32 * 1024)
#define FRAME_BUF_SIZE (320 * 320 * sizeof(uint16_t))
#define MAX_JPEG_SIZE  (96 * 1024)
#define VP_STRIP_H     40
#define VP_STRIP_BYTES (VP_STRIP_H * 320 * sizeof(uint16_t))
#define VP_STRIP_BUFS  2

extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;
extern volatile uint32_t refresh_done_count;

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
static esp_err_t         s_decode_result = ESP_OK;
static uint32_t          s_decode_width = 0;
static uint32_t          s_decode_height = 0;

static void decode_task(void *arg)
{
    decode_job_t job;
    while (1) {
        if (xQueueReceive(s_decode_q, &job, portMAX_DELAY) == pdTRUE) {
            if (job.jpeg_data == NULL) break;
            s_decode_result = mjpeg_decoder_decode(
                job.jpeg_data, job.jpeg_size, job.out_buf, FRAME_BUF_SIZE,
                &s_decode_width, &s_decode_height);
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
        uint8_t *ab = s_refill_buf
                          ? s_refill_buf
                          : sr->buf + sr->valid;
        UINT br;
        FRESULT fr = f_read(sr->file, ab, sr->buf_size - sr->valid, &br);
        if (fr) return ESP_FAIL;
        if (s_refill_buf)
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
                            uint8_t *jpeg_out, size_t *jpeg_sz_out)
{
    while (1) {
        uint8_t fhdr[8];
        if (sr_read(sr, fhdr, 8)) return false;
        if (avi_get_streaminfo(fhdr, avi) != AVI_OK) return false;

        uint32_t sz = avi->StreamSize;
        if (memcmp(fhdr, avi->VideoFLAG, 4) == 0) {
            if (sz <= MAX_JPEG_SIZE) {
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
            if (sz & 1) sr_skip_pad(sr, 1);
            return true;
        } else if (memcmp(fhdr, avi->AudioFLAG, 4) == 0) {
            /* 视频播放器只处理 MJPEG 图像；AVI 内的音频块始终跳过。 */
            sr_skip_pad(sr, sz);
            if (sz & 1) sr_skip_pad(sr, 1);
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
    uint16_t  *strip_buf[VP_STRIP_BUFS];
    uint8_t    strip_buf_count;

    int        cur, next;
    bool       have_next;
    bool       frame_decoded;
    bool       next_queued;

    uint32_t   movi_pos;
    uint16_t   offx, offy;

    /* 帧率控制 */
    int64_t    next_ts;
    bool       first_vf;

    /* 性能分析 */
    uint32_t   frame_count;
    int64_t    start_ts;
    int64_t    stats_ts;
    int        loop_wraps;

    int16_t    strip_ys;
    uint16_t   strip_submitted;
    uint32_t   lcd_done_base;
    int64_t    last_write_ts;
    char       name[256];
} vp_ctx_t;

static vp_ctx_t g_vp = {0};

/* ====== 公开 API ====== */

static bool has_avi_extension(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && strcasecmp(name + length - 4, ".avi") == 0;
}

static DIR *open_sd_root(void)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (!sd_spi_is_mounted() || attempt > 0)
        {
            if (sd_spi_init() != ESP_OK || !sd_spi_is_mounted())
                continue;
        }
        DIR *directory = opendir(MOUNT_POINT);
        if (directory)
            return directory;
    }
    return NULL;
}

static esp_err_t resolve_video_selection(const char *selection,
                                         char *name, size_t name_size)
{
    if (!selection || !selection[0] || !name || name_size == 0)
        return ESP_ERR_INVALID_ARG;

    char *end = NULL;
    long requested = strtol(selection, &end, 10);
    if (selection != end && *end == '\0')
    {
        if (requested < 1 || requested > INT_MAX)
            return ESP_ERR_INVALID_ARG;
        DIR *directory = open_sd_root();
        if (!directory)
            return ESP_FAIL;
        int index = 0;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL)
        {
            if (!has_avi_extension(entry->d_name))
                continue;
            if (++index == requested)
            {
                int written = snprintf(name, name_size, "%s", entry->d_name);
                closedir(directory);
                return written > 0 && written < (int)name_size
                           ? ESP_OK
                           : ESP_ERR_INVALID_SIZE;
            }
        }
        closedir(directory);
        return ESP_ERR_NOT_FOUND;
    }

    const char *plain = selection;
    if (strncmp(plain, "/0:/", 4) == 0)
        plain += 4;
    else if (strncmp(plain, "0:/", 3) == 0)
        plain += 3;
    else if (plain[0] == '/')
        plain++;

    if (!plain[0] || strchr(plain, '/') || strchr(plain, '\\') ||
        strcmp(plain, ".") == 0 || strcmp(plain, "..") == 0 ||
        !has_avi_extension(plain))
        return ESP_ERR_INVALID_ARG;

    int written = snprintf(name, name_size, "%s", plain);
    return written > 0 && written < (int)name_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

int video_player_list_files(char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return -1;
    output[0] = '\0';

    DIR *directory = open_sd_root();
    if (!directory)
        return -1;

    int count = 0;
    size_t used = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (!has_avi_extension(entry->d_name))
            continue;
        count++;
        if (used + 1 >= output_size)
            continue;
        int written = snprintf(output + used, output_size - used,
                               "%d=%s\n", count, entry->d_name);
        if (written < 0)
            continue;
        used += (size_t)written < output_size - used
                    ? (size_t)written
                    : output_size - used - 1;
    }
    closedir(directory);
    return count;
}

esp_err_t video_player_start(const char *selection)
{
    char name[256];
    esp_err_t ret = resolve_video_selection(selection, name, sizeof(name));
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
    for (int i = 0; i < 2; i++) {
        g_vp.frame_buf[i] = heap_caps_aligned_alloc(
            64, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_vp.frame_buf[i]) { ESP_LOGE(TAG, "OOM fb"); ret = ESP_ERR_NO_MEM; goto fail; }

        g_vp.jpeg_buf[i] = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_vp.jpeg_buf[i]) { ESP_LOGE(TAG, "OOM jb"); ret = ESP_ERR_NO_MEM; goto fail; }
    }
    for (int i = 0; i < VP_STRIP_BUFS; i++)
    {
        g_vp.strip_buf[i] = heap_caps_aligned_alloc(
            64, VP_STRIP_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!g_vp.strip_buf[i])
            break;
        g_vp.strip_buf_count++;
    }
    if (g_vp.strip_buf_count == 0)
    {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* 打开文件, 解析 AVI 头 */
    char fp[272];
    if (strncmp(filename, "/0:/", 4) == 0)
        snprintf(fp, sizeof(fp), "0:/%s", filename + 4);
    else if (strncmp(filename, "0:/", 3) == 0)
        snprintf(fp, sizeof(fp), "%s", filename);
    else
        snprintf(fp, sizeof(fp), "0:/%s", filename[0] == '/' ? filename + 1 : filename);
    if (f_open(&g_vp.file, fp, FA_READ)) { ret = ESP_ERR_NOT_FOUND; goto fail; }

    if (sr_open(&g_vp.sr, &g_vp.file, DMA_BUF_SIZE) || sr_refill(&g_vp.sr) || g_vp.sr.valid < 1024) {
        ESP_LOGE(TAG, "header"); ret = ESP_FAIL; goto fail;
    }
    if (avi_init(g_vp.sr.buf, g_vp.sr.valid, &g_vp.avi) != AVI_OK) {
        ret = ESP_FAIL; goto fail;
    }
    if (g_vp.avi.Width == 0 || g_vp.avi.Height == 0 ||
        g_vp.avi.Width > 320 || g_vp.avi.Height > 320 ||
        g_vp.avi.SecPerFrame == 0) {
        ret = ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    ret = mjpeg_decoder_init(g_vp.avi.Width, g_vp.avi.Height);
    if (ret != ESP_OK)
        goto fail;

    /* 启动 Core1 解码任务 */
    s_decode_q    = xQueueCreate(1, sizeof(decode_job_t));
    s_decode_done = xSemaphoreCreateBinary();
    if (!s_decode_q || !s_decode_done ||
        xTaskCreatePinnedToCore(decode_task, "jpeg_sd", 4096, NULL, 5,
                                &s_decode_task, 1) != pdPASS)
    {
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
        !read_one_chunk(&g_vp.sr, &g_vp.avi, g_vp.jpeg_buf[1], &g_vp.jpeg_size[1])) {
        ESP_LOGE(TAG, "prefetch fail"); ret = ESP_FAIL; goto fail;
    }

    /* 提交 F0 到 Core1 解码 */
    decode_job_t job0 = { g_vp.jpeg_buf[0], g_vp.jpeg_size[0], g_vp.frame_buf[0], ESP_OK };
    if (xQueueSend(s_decode_q, &job0, 0) != pdTRUE)
    {
        ret = ESP_FAIL;
        goto fail;
    }

    g_vp.first_vf   = true;
    g_vp.have_next  = true;
    g_vp.frame_decoded = false;
    g_vp.next_queued = false;
    g_vp.cur        = 0;
    g_vp.next       = 1;
    g_vp.strip_ys   = -1;
    g_vp.start_ts   = esp_timer_get_time();
    g_vp.stats_ts   = g_vp.start_ts;
    g_vp.next_ts    = g_vp.start_ts + g_vp.avi.SecPerFrame;
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
    if (!g_vp.initialized) return PLAYER_ERROR;

    if (g_vp.strip_ys < 0)
    {
        if (!g_vp.frame_decoded)
        {
            if (xSemaphoreTake(s_decode_done, 0) != pdTRUE)
                return PLAYER_BUSY;
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
            if (xQueueSend(s_decode_q, &job, 0) != pdTRUE)
                return PLAYER_BUSY;

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
            g_vp.next_queued = true;
        }

        if (!g_vp.first_vf)
        {
            if (esp_timer_get_time() < g_vp.next_ts)
                return PLAYER_BUSY;
            g_vp.next_ts += g_vp.avi.SecPerFrame;
        }
        else
        {
            g_vp.first_vf = false;
            g_vp.next_ts = esp_timer_get_time() + g_vp.avi.SecPerFrame;
        }

        if (!lcd_is_ready())
            return PLAYER_BUSY;
        if (g_vp.last_write_ts &&
            esp_timer_get_time() < g_vp.last_write_ts + 17000)
            return PLAYER_BUSY;

        spilcd_wait_te();
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

        g_vp.strip_ys = -1;
        g_vp.frame_count++;
        if (g_vp.frame_count % PROF_EVERY == 0)
        {
            int64_t now = esp_timer_get_time();
            float fps = PROF_EVERY * 1000000.0f /
                        (float)(now - g_vp.stats_ts);
            ESP_LOGI(TAG, "perf: %.1f fps, frames=%lu, wraps=%d", fps,
                     (unsigned long)g_vp.frame_count, g_vp.loop_wraps);
            g_vp.stats_ts = now;
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
    memcpy(strip, g_vp.frame_buf[g_vp.cur] + ys * g_vp.avi.Width, bytes);

    esp_err_t ret;
    if (g_vp.strip_submitted == 0)
    {
        ret = esp_lcd_jd9855_draw_bitmap_start(
            panel_handle, g_vp.offx, g_vp.offy,
            g_vp.offx + g_vp.avi.Width, g_vp.offy + g_vp.avi.Height,
            strip, bytes);
    }
    else
    {
        ret = esp_lcd_jd9855_draw_bitmap_continue(
            panel_handle, strip, bytes);
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LCD submit failed: %s", esp_err_to_name(ret));
        return PLAYER_ERROR;
    }
    g_vp.strip_submitted++;
    g_vp.strip_ys = ys + rows;
    return PLAYER_BUSY;
}

void video_player_stop(void)
{
    if (!g_vp.initialized) return;

    bool lcd_idle = true;
    if (g_vp.strip_submitted > 0)
    {
        int wait = 0;
        while (refresh_done_count - g_vp.lcd_done_base <
                   g_vp.strip_submitted &&
               wait++ < 100)
            vTaskDelay(pdMS_TO_TICKS(1));
        lcd_idle = refresh_done_count - g_vp.lcd_done_base >=
                   g_vp.strip_submitted;
    }

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
    for (int i = 0; i < VP_STRIP_BUFS; i++)
    {
        if (g_vp.strip_buf[i] && lcd_idle)
        {
            heap_caps_free(g_vp.strip_buf[i]);
            g_vp.strip_buf[i] = NULL;
        }
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
