/**
 * @brief  原始 RGB565 视频播放器 (DMA 优化版)
 *
 * 从 SD 卡读取预处理的 .raw 文件, 逐帧推送到 ST7789 LCD。
 *
 * 关键优化:
 *   1. 使用内部 DMA buffer + f_read 分块读取, 在 diskio 层触发
 *      sdmmc_read_sectors 的 CMD18 多块读 (而非 PSRAM 触发的 CMD17 逐扇区读)
 *   2. 双缓冲流水线: SD 读和 LCD 传并行, 隐藏 I/O 延迟
 */
#include "raw_player.h"
#include "spilcd.h"
#include "spi_sd.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "raw_player"

extern esp_lcd_panel_handle_t panel_handle;
extern sdmmc_card_t *card;   /* from spi_sd.c */

#define RAW_MAGIC  0x56574152  /* "RAWV" */
#define DMA_CHUNK  (16 * 1024) /* 内部 DMA bounce buffer 大小 (32 扇区) */

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t total_frames;
} __attribute__((packed)) raw_header_t;

/*
 * 从文件读取指定字节到 PSRAM, 通过内部 DMA buffer 中转
 * 确保 diskio 层拿到 MALLOC_CAP_DMA 的 buffer → CMD18 多块读
 */
static esp_err_t f_read_dma(FIL *file, uint8_t *psram_dst, size_t total_bytes)
{
    static uint8_t *dma_buf = NULL;
    if (dma_buf == NULL) {
        dma_buf = heap_caps_malloc(DMA_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (dma_buf == NULL) {
            ESP_LOGE(TAG, "DMA bounce buffer OOM");
            return ESP_ERR_NO_MEM;
        }
    }

    size_t remaining = total_bytes;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk = (remaining > DMA_CHUNK) ? DMA_CHUNK : remaining;
        UINT bytes_read;
        FRESULT fr = f_read(file, dma_buf, chunk, &bytes_read);
        if (fr != FR_OK || bytes_read < chunk) {
            ESP_LOGE(TAG, "f_read err: fr=%d, got=%u/%u", fr, bytes_read, chunk);
            return ESP_FAIL;
        }
        memcpy(psram_dst + offset, dma_buf, chunk);
        offset += chunk;
        remaining -= chunk;
    }
    return ESP_OK;
}

esp_err_t raw_player_play(const char *filename)
{
    FIL file;
    uint16_t *frame_buf[2] = {NULL, NULL};  /* 双缓冲 (PSRAM) */
    esp_err_t ret = ESP_OK;

    char fatfs_path[272];
    if (strncmp(filename, "/0:/", 4) == 0) {
        snprintf(fatfs_path, sizeof(fatfs_path), "0:%s", filename + 4);
    } else {
        strncpy(fatfs_path, filename, sizeof(fatfs_path) - 1);
    }

    FRESULT fr = f_open(&file, fatfs_path, FA_READ);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "Failed to open: %s (err=%d)", fatfs_path, fr);
        return ESP_ERR_NOT_FOUND;
    }

    raw_header_t hdr;
    UINT bytes_read;
    fr = f_read(&file, &hdr, sizeof(hdr), &bytes_read);
    if (fr != FR_OK || bytes_read < sizeof(hdr) || hdr.magic != RAW_MAGIC) {
        ESP_LOGE(TAG, "Invalid header");
        ret = ESP_FAIL;
        goto cleanup;
    }
    if (hdr.width > 320 || hdr.height > 240) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    uint32_t frame_bytes = (uint32_t)hdr.width * hdr.height * 2;
    int64_t frame_interval = (hdr.fps > 0) ? (1000000LL / hdr.fps) : 0;

    /* 分配双缓冲 (PSRAM) */
    for (int i = 0; i < 2; i++) {
        frame_buf[i] = (uint16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM);
        if (frame_buf[i] == NULL) {
            frame_buf[i] = (uint16_t *)malloc(frame_bytes);
        }
        if (frame_buf[i] == NULL) {
            ESP_LOGE(TAG, "OOM for frame buf %d", i);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    }

    /* ---- 基准测试: 裸 sdmmc_read_sectors (排除 FATFS 影响) ---- */
    DWORD data_sec = file.sect;  /* FATFS: 当前数据起始扇区 */
    int nsectors = (int)((frame_bytes + 511) / 512);
    uint8_t *dma_buf = heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *psram_buf = (uint8_t *)frame_buf[0];

    /* 测试 A: DMA buffer (内部) → 应走 CMD18 多块读 */
    if (dma_buf) {
        ESP_LOGI(TAG, "Bench A: sdmmc_read_sectors(DMA buf) 5x%d sec ...", nsectors);
        int64_t bench_start = esp_timer_get_time();
        for (int b = 0; b < 5; b++) {
            sdmmc_read_sectors(card, dma_buf, data_sec, nsectors);
        }
        int64_t bench_elapsed = esp_timer_get_time() - bench_start;
        ESP_LOGI(TAG, "DMA buf : 5x%luB = %lld ms (%.1f KB/s)",
                 frame_bytes, bench_elapsed / 1000,
                 (frame_bytes * 5.0 / 1024) / (bench_elapsed / 1000000.0));
    } else {
        ESP_LOGE(TAG, "DMA buf alloc failed!");
    }

    /* 测试 B: PSRAM buffer → 应走 CMD17 逐扇区回退 */
    ESP_LOGI(TAG, "Bench B: sdmmc_read_sectors(PSRAM)   5x%d sec ...", nsectors);
    int64_t bench_start = esp_timer_get_time();
    for (int b = 0; b < 5; b++) {
        sdmmc_read_sectors(card, psram_buf, data_sec, nsectors);
    }
    int64_t bench_elapsed = esp_timer_get_time() - bench_start;
    ESP_LOGI(TAG, "PSRAM   : 5x%luB = %lld ms (%.1f KB/s)",
             frame_bytes, bench_elapsed / 1000,
             (frame_bytes * 5.0 / 1024) / (bench_elapsed / 1000000.0));

    /* 测试 C: DMA buffer, 但每次只读 1 个扇区 (模拟 CMD17 回退) */
    ESP_LOGI(TAG, "Bench C: sdmmc_read_sectors(DMA,1sec) 5x%d calls ...", nsectors);
    bench_start = esp_timer_get_time();
    for (int b = 0; b < 5; b++) {
        for (int s = 0; s < nsectors; s++) {
            sdmmc_read_sectors(card, dma_buf + s * 512, data_sec + s, 1);
        }
    }
    bench_elapsed = esp_timer_get_time() - bench_start;
    ESP_LOGI(TAG, "DMA 1sec: 5x%luB = %lld ms (%.1f KB/s)",
             frame_bytes, bench_elapsed / 1000,
             (frame_bytes * 5.0 / 1024) / (bench_elapsed / 1000000.0));

    /* 测试 D: f_read 到 PSRAM (原始方式, 对照) */
    ESP_LOGI(TAG, "Bench D: f_read(PSRAM)              5x%lu B ...", frame_bytes);
    bench_start = esp_timer_get_time();
    for (int b = 0; b < 5; b++) {
        f_lseek(&file, sizeof(hdr));
        f_read(&file, psram_buf, frame_bytes, &bytes_read);
    }
    bench_elapsed = esp_timer_get_time() - bench_start;
    ESP_LOGI(TAG, "f_read   : 5x%luB = %lld ms (%.1f KB/s)",
             frame_bytes, bench_elapsed / 1000,
             (frame_bytes * 5.0 / 1024) / (bench_elapsed / 1000000.0));

    if (dma_buf) free(dma_buf);

    /* ---- 开始播放 (直接 sdmmc_read_sectors + 内部 DMA buffer) ---- */
    uint16_t offx = (320 - hdr.width) / 2;
    uint16_t offy = (240 - hdr.height) / 2;

    /*
     * 每帧 150 扇区, 但文件数据从 header 之后开始 (byte 14),
     * 所以每帧的第一扇区只有 498 字节有效数据, 最后一扇区只有 14 字节。
     * 读 151 扇区 (77312 B) 覆盖完整一帧, 然后从 +14 偏移处取 76800 B。
     */
    int sec_per_frame = (frame_bytes + 511) / 512;  /* 150 */
    int sec_to_read   = sec_per_frame + 1;          /* 151, 覆盖首尾不对齐 */
    size_t dma_size   = sec_to_read * 512;          /* 77312 B */
    bool use_chunks = false;

    uint8_t *rdma = heap_caps_malloc(dma_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (rdma == NULL) {
        /* 内部 RAM 不够, 降级到 16KB chunk 模式 */
        ESP_LOGW(TAG, "Large DMA buf OOM, using 16KB chunks");
        use_chunks = true;
        rdma = heap_caps_malloc(DMA_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (rdma == NULL) {
            ESP_LOGE(TAG, "DMA chunk OOM");
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
    }

    ESP_LOGI(TAG, "Playing: %ux%u, %lu frames, %u fps (CMD18 DMA)",
             hdr.width, hdr.height, hdr.total_frames, hdr.fps);

    spilcd_clear(BLACK);

    uint32_t frame_count = 0;
    int64_t start_time = esp_timer_get_time();
    int64_t next_frame_time = start_time;

    for (uint32_t i = 0; i < hdr.total_frames; i++) {
        DWORD frame_sec = data_sec + i * sec_per_frame;

        if (!use_chunks) {
            /* 快速路径: 一次 CMD18 读 151 扇区 → 覆盖首尾不对齐 */
            ret = sdmmc_read_sectors(card, rdma, frame_sec, sec_to_read);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "read err frm %lu", i); break; }
            memcpy((uint8_t *)frame_buf[0], rdma + 14, frame_bytes);
        } else {
            /* 降级路径: 16KB chunk, 首 chunk 跳过 14 字节 */
            uint8_t *dst = (uint8_t *)frame_buf[0];
            size_t remain = frame_bytes;
            size_t dst_off = 0;
            DWORD cur_sec = frame_sec;
            int head_skip = 14;

            while (remain > 0) {
                size_t chunk = (remain + head_skip > DMA_CHUNK) ? DMA_CHUNK : remain + head_skip;
                size_t cs = (chunk + 511) / 512;
                ret = sdmmc_read_sectors(card, rdma, cur_sec, cs);
                if (ret != ESP_OK) break;
                memcpy(dst + dst_off, rdma + head_skip, chunk - head_skip);
                dst_off += chunk - head_skip;
                remain -= chunk - head_skip;
                cur_sec += cs;
                head_skip = 0;
            }
            if (ret != ESP_OK) { ESP_LOGE(TAG, "chunk read err frm %lu", i); break; }
        }

        /* 帧率控制 */
        if (frame_interval > 0 && i > 0) {
            int64_t now;
            while ((now = esp_timer_get_time()) < next_frame_time) {
                vTaskDelay(1);
            }
            next_frame_time += frame_interval;
            if (now > next_frame_time + frame_interval * 2) {
                next_frame_time = now + frame_interval;
            }
        }

        /* 发送当前帧到 LCD (DMA 异步) */
        esp_lcd_panel_draw_bitmap(panel_handle,
                                   offx, offy,
                                   offx + hdr.width, offy + hdr.height,
                                   frame_buf[0]);
        frame_count++;

        if (frame_count % 50 == 0) {
            int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "frm %lu: avg %.1f fps",
                     frame_count,
                     frame_count * 1000000.0 / (now - start_time));
        }
    }

    free(rdma);

    int64_t elapsed = esp_timer_get_time() - start_time;
    ESP_LOGI(TAG, "Done: %lu frames in %.1fs (%.1f fps)",
             frame_count, elapsed / 1000000.0,
             frame_count / (elapsed / 1000000.0));

    spilcd_show_string(10, 220, 300, 240, 12, "Playback done", GREEN);

cleanup:
    if (file.obj.fs != NULL) f_close(&file);
    for (int i = 0; i < 2; i++) free(frame_buf[i]);
    return ret;
}
