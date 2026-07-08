/**
 * @brief  AVI 视频播放器 (双核流水线版)
 *
 * Core0: 全部 SPI (SD读取 + LCD发送)
 * Core1: JPEG 解码 (CPU, 与 SPI DMA 并行)
 *
 * 流水线:
 *   Core0 等待 Core1 解码完 frame[cur] →
 *       提交 frame[next] 解码 (Core1开始工作) →
 *       预读 frame[next+1] 的JPEG数据到刚释放的 jpeg_buf[cur] →
 *       等待 LCD 完成, 发送 frame[cur] →
 *       cur/next 交换, 重复
 *
 *   overlap: Core1 解码 frame[next] || Core0 SPI (预读 + LCD)
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
#define ENABLE_AUDIO 0 /* 1=播放音频, 0=跳过 (48000Hz需重采样到16000Hz) */
#define PROF_EVERY 10  /* 每 N 帧输出一次耗时分析 */
/* ===================== */

#define TAG "video_player"

#define DMA_BUF_SIZE (32 * 1024)
#define FRAME_BUF_SIZE (320 * 240 * sizeof(uint16_t))
#define MAX_JPEG_SIZE (48 * 1024)

extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;

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

static void decode_task(void *arg)
{
    decode_job_t job;
    while (1)
    {
        if (xQueueReceive(s_decode_q, &job, portMAX_DELAY) == pdTRUE)
        {
            if (job.jpeg_data == NULL) break;  /* shutdown signal */
            uint32_t w, h;
            job.result = mjpeg_decoder_decode(job.jpeg_data, job.jpeg_size,
                                               job.out_buf, FRAME_BUF_SIZE, &w, &h);
            xSemaphoreGive(s_decode_done);
        }
    }
    vTaskDelete(NULL);  /* self-delete */
}

/* ---- 流式读取器 (Core0, SPI) ---- */
typedef struct
{
    FIL *file;
    uint8_t *buf;
    size_t buf_size, pos, valid;
} sr_t;

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
        free(sr->buf);
}

/* 对齐 DMA refill buffer: f_read 的目标地址必须 32B 对齐, 否则 CMD17 */
static uint8_t *s_refill_buf = NULL;

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
        uint8_t *ab = s_refill_buf ? s_refill_buf : sr->buf;
        UINT br;
        FRESULT fr = f_read(sr->file, ab, sr->buf_size - sr->valid, &br);
        if (fr)
            return ESP_FAIL;
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
static void sr_adv(sr_t *sr, size_t n) { sr->pos += n; }
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
static inline void lcd_wait(void)
{
    while (!refresh_done_flag)
        vTaskDelay(1);
    refresh_done_flag = 0;
}

/* 从流中读取一个 AVI chunk: 返回 true=视频帧, false=音频/其他 */
static bool read_one_chunk(sr_t *sr, AVI_INFO *avi,
                           uint8_t *jpeg_out, size_t *jpeg_sz_out,
                           int *audio_cnt)
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
            if (sr_ensure(sr, sz))
                return false;
            if (sz <= MAX_JPEG_SIZE)
            {
                memcpy(jpeg_out, sr->buf + sr->pos, sz);
                *jpeg_sz_out = sz;
            }
            sr_adv(sr, sz);
            if (sz & 1)
                sr_skip_pad(sr, 1);
            return true;
        }
        else if (memcmp(fhdr, avi->AudioFLAG, 4) == 0)
        {
#if ENABLE_AUDIO
            if (sr_ensure(sr, sz) == ESP_OK)
            {
                if (avi->AudioType == 1)
                    audio_write_pcm((const int16_t *)(sr->buf + sr->pos),
                                    sz / 2, avi->SampleRate, avi->Channels);
                sr_adv(sr, sz);
            }
#else
            sr_skip_pad(sr, sz);
#endif
            if (sz & 1)
                sr_skip_pad(sr, 1);
            if (audio_cnt)
                (*audio_cnt)++;
        }
        else
        {
            sr_skip_pad(sr, sz);
            if (sz & 1)
                sr_skip_pad(sr, 1);
        }
    }
}

/* ---- 主函数 ---- */
esp_err_t video_player_play(const char *filename)
{
    esp_err_t ret = ESP_OK;
    FIL file;
    uint16_t *frame_buf[2] = {NULL, NULL};
    uint8_t *jpeg_buf[2] = {NULL, NULL};
    size_t jpeg_size[2] = {0};
    sr_t sr = {0};
    AVI_INFO avi;

    /* 分配内存 (16-byte 对齐: esp_new_jpeg 要求) */
    for (int i = 0; i < 2; i++)
    {
        frame_buf[i] = heap_caps_aligned_alloc(16, FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!frame_buf[i])
            frame_buf[i] = heap_caps_aligned_alloc(16, FRAME_BUF_SIZE, MALLOC_CAP_8BIT);
        if (!frame_buf[i])
        {
            ESP_LOGE(TAG, "OOM fb");
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        jpeg_buf[i] = heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        if (!jpeg_buf[i])
            jpeg_buf[i] = malloc(MAX_JPEG_SIZE);
        if (!jpeg_buf[i])
        {
            ESP_LOGE(TAG, "OOM jb");
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    }

    /* 打开文件, 解析 AVI 头 */
    char fp[272];
    snprintf(fp, sizeof(fp), "0:%s", filename + (strncmp(filename, "/0:/", 4) == 0 ? 4 : 0));
    if (!strchr(fp, ':'))
        strncpy(fp, filename, sizeof(fp) - 1);
    if (f_open(&file, fp, FA_READ))
    {
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    if (sr_open(&sr, &file, DMA_BUF_SIZE) || sr_refill(&sr) || sr.valid < 1024)
    {
        ESP_LOGE(TAG, "header");
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (avi_init(sr.buf, sr.valid, &avi) != AVI_OK)
    {
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (avi.Width > 320 || avi.Height > 240)
    {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    mjpeg_decoder_init(avi.Width, avi.Height);

    /* 启动 Core1 解码任务 */
    s_decode_q = xQueueCreate(2, sizeof(decode_job_t));
    s_decode_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(decode_task, "jpeg", 4096, NULL, 5, &s_decode_task, 1);

    spilcd_show_string(0, 0, 320, 16, 16, (char *)filename, BLACK);

    uint32_t movi_pos = avi.MoviOffset + 4;  /* 记住, 无缝循环用 */

    uint16_t offx = (320 - avi.Width) / 2;
    uint16_t offy = (240 - avi.Height) / 2;

    /* ---- 外层: 无缝循环 ---- */
    static bool s_prefetched = false; /* 跨循环保持: 上一轮结束已预取 F0 */
    int loop_cnt = 0;

    for (;; loop_cnt++)
    {
        f_lseek(&file, movi_pos);
        sr.pos = sr.valid = 0;
        refresh_done_flag = 1;

        uint32_t frame_count = 0;
        int64_t start_ts = esp_timer_get_time();
        int64_t next_ts = start_ts + avi.SecPerFrame;
        bool first_vf = true;
        int64_t t_wait_dec = 0, t_read = 0, t_wait_lcd = 0;
        int audio_frames = 0, video_chunks = 0;
        int cur = 0, next = 1;
        bool have_next = true;
        int pending;

        if (!s_prefetched) {
            /* 第一轮: 完整预读 F0 + F1, 提交 F0 */
            if (!read_one_chunk(&sr, &avi, jpeg_buf[0], &jpeg_size[0], NULL) ||
                !read_one_chunk(&sr, &avi, jpeg_buf[1], &jpeg_size[1], NULL))
                { ESP_LOGE(TAG, "prefetch fail"); goto cleanup; }
            decode_job_t job0 = {jpeg_buf[0], jpeg_size[0], frame_buf[0]};
            xQueueSend(s_decode_q, &job0, 0);
            pending = 1;
        } else {
            /* 后续轮: F0 在上一轮结束时已提交 Core1, 只需预读 F1 */
            s_prefetched = false;
            if (!read_one_chunk(&sr, &avi, jpeg_buf[1], &jpeg_size[1], NULL))
                { ESP_LOGE(TAG, "prefetch F1 fail"); goto cleanup; }
            pending = 1; /* F0 正在 Core1 解码中 */
        }

        if (loop_cnt == 0)
            ESP_LOGI(TAG, "Start: audio=%s", ENABLE_AUDIO ? "ON" : "OFF");

        while (1)
        {
        /* (a) 等待 Core1 解码完 frame[cur] */
        int64_t t0 = esp_timer_get_time();
        xSemaphoreTake(s_decode_done, portMAX_DELAY);
        t_wait_dec += esp_timer_get_time() - t0;
        pending--;

        /* (b) 提交 frame[next] 解码 */
        if (have_next)
        {
            decode_job_t job = {jpeg_buf[next], jpeg_size[next], frame_buf[next]};
            xQueueSend(s_decode_q, &job, 0);
            pending++;
        }

        /* (c) 预读下一帧 JPEG → jpeg_buf[cur] */
        t0 = esp_timer_get_time();
        int aud = 0;
        have_next = read_one_chunk(&sr, &avi, jpeg_buf[cur], &jpeg_size[cur], &aud);
        audio_frames += aud;
        if (have_next)
            video_chunks++;
        t_read += esp_timer_get_time() - t0;

        /* (d) 帧率控制 */
        if (!first_vf)
        {
            while (esp_timer_get_time() < next_ts)
                vTaskDelay(1);
            next_ts += avi.SecPerFrame;
        }
        else
        {
            first_vf = false;
            next_ts = esp_timer_get_time() + avi.SecPerFrame;
        }

        /* (e) 等待 LCD 完成, 发送 frame[cur] */
        t0 = esp_timer_get_time();
        lcd_wait();
        t_wait_lcd += esp_timer_get_time() - t0;
        esp_lcd_panel_draw_bitmap(panel_handle, offx, offy,
                                  offx + avi.Width, offy + avi.Height,
                                  frame_buf[cur]);
        frame_count++;

        /* 定时分析 */
        if (frame_count % PROF_EVERY == 0)
        {
            int64_t elapsed = esp_timer_get_time() - start_ts;
            ESP_LOGI(TAG, "--- profile %lu frames (loop %d) ---", frame_count, loop_cnt);
            ESP_LOGI(TAG, "  avg fps       : %.1f", frame_count * 1e6 / elapsed);
            ESP_LOGI(TAG, "  wait decode   : %lld ms (%.0f%%)",
                     t_wait_dec / 1000, 100.0 * t_wait_dec / elapsed);
            ESP_LOGI(TAG, "  read chunk    : %lld ms (%.0f%%)",
                     t_read / 1000, 100.0 * t_read / elapsed);
        }

        /* (f) 退出检查 */
        if (pending == 0) {
            /* 提前预取下一轮 F0: 与当前帧 LCD DMA 重叠 */
            f_lseek(&file, movi_pos);
            sr.pos = sr.valid = 0;
            refresh_done_flag = 1;
            if (read_one_chunk(&sr, &avi, jpeg_buf[0], &jpeg_size[0], NULL)) {
                decode_job_t job = { jpeg_buf[0], jpeg_size[0], frame_buf[0] };
                xQueueSend(s_decode_q, &job, 0);
                s_prefetched = true;
            }
            break;
        }

        /* (g) 交换 */
        cur = next;
        next = 1 - next;
    }

    lcd_wait();
    int64_t elapsed = esp_timer_get_time() - start_ts;
    ESP_LOGI(TAG, "Loop %d: %lu frames %.1fs (%.1f fps)",
             loop_cnt, frame_count, elapsed / 1e6, frame_count / (elapsed / 1e6));
}

cleanup:
    /* 通知解码任务退出 */
    if (s_decode_q && s_decode_task) {
        decode_job_t shutdown = { NULL, 0, NULL, ESP_OK };
        xQueueSend(s_decode_q, &shutdown, portMAX_DELAY);
        /* 等待任务自行删除 (最多 100ms) */
        int wait = 0;
        while (eTaskGetState(s_decode_task) != eDeleted && wait < 100) {
            vTaskDelay(pdMS_TO_TICKS(1)); wait++;
        }
        s_decode_task = NULL;
    }
    if (s_decode_q)  { vQueueDelete(s_decode_q);  s_decode_q  = NULL; }
    if (s_decode_done){ vSemaphoreDelete(s_decode_done); s_decode_done = NULL; }
    mjpeg_decoder_deinit();
    if (file.obj.fs)
        f_close(&file);
    sr_close(&sr);
    for (int i = 0; i < 2; i++)
    {
        if (frame_buf[i])
            heap_caps_free(frame_buf[i]);
        if (jpeg_buf[i])
            free(jpeg_buf[i]);
    }
    return ret;
}
