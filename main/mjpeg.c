/**
 * @brief  MJPEG 解码器
 *
 *  宏切换:
 *    JPEG_DECODER_OLD  → esp_jpeg (ROM TJPGD) 稳定, ~11fps@320x240
 *    JPEG_DECODER_NEW  → esp_new_jpeg (SIMD)   加速, ~66fps@320x240
 */
#include "mjpeg.h"
#include "esp_log.h"
#include <string.h>

#define JPEG_DECODER_OLD 1
#define JPEG_DECODER_NEW 2

#ifndef JPEG_DECODER
#define JPEG_DECODER JPEG_DECODER_NEW
#endif

#define TAG "mjpeg"

#if JPEG_DECODER == JPEG_DECODER_OLD
/* ================================================================
 *  旧版: esp_jpeg — ROM TJPGD, 稳定, 省内存
 * ================================================================ */
#include "jpeg_decoder.h"

esp_err_t mjpeg_decoder_init(uint32_t max_width, uint32_t max_height)
{
    ESP_LOGI(TAG, "Decoder ready (ROM TJPGD, max %lux%lu)", max_width, max_height);
    return ESP_OK;
}

esp_err_t mjpeg_decoder_decode(const uint8_t *jpeg_data, size_t jpeg_size,
                               uint16_t *out_pixels, size_t out_buf_size,
                               uint32_t *out_width, uint32_t *out_height)
{
    if (!jpeg_data || !jpeg_size || !out_pixels)
        return ESP_ERR_INVALID_ARG;

    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)jpeg_data,
        .indata_size = (uint32_t)jpeg_size,
        .outbuf = (uint8_t *)out_pixels,
        .outbuf_size = (uint32_t)out_buf_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {.swap_color_bytes = 1},
    };

    esp_jpeg_image_output_t outimg;
    esp_err_t ret = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "decode: %d", ret);
        return ret;
    }
    *out_width = outimg.width;
    *out_height = outimg.height;
    return ESP_OK;
}

void mjpeg_decoder_deinit(void) {}

#elif JPEG_DECODER == JPEG_DECODER_NEW
/* ================================================================
 *  新版: esp_new_jpeg — SIMD 加速, 快 6x
 * ================================================================ */
#include "esp_jpeg_dec.h"

static jpeg_dec_handle_t s_dec = NULL;

esp_err_t mjpeg_decoder_init(uint32_t max_width, uint32_t max_height)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;  /* DMA swap=1 需要小端输入 */
    jpeg_error_t ret = jpeg_dec_open(&cfg, &s_dec);
    if (ret != JPEG_ERR_OK)
    {
        ESP_LOGE(TAG, "open: %d", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Decoder ready (esp_new_jpeg SIMD, max %lux%lu)", max_width, max_height);
    return ESP_OK;
}

esp_err_t mjpeg_decoder_decode(const uint8_t *jpeg_data, size_t jpeg_size,
                               uint16_t *out_pixels, size_t out_buf_size,
                               uint32_t *out_width, uint32_t *out_height)
{
    if (!s_dec || !jpeg_data || !jpeg_size || !out_pixels)
        return ESP_ERR_INVALID_ARG;

    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)jpeg_data,
        .inbuf_len = (int)jpeg_size,
        .inbuf_remain = 0,
        .outbuf = (uint8_t *)out_pixels,
    };

    jpeg_dec_header_info_t hdr;
    /* DBG: 打印输入 JPEG 前 16 字节 */
    {
        static int dbg_in_cnt = 0;
        if (dbg_in_cnt < 5) {
            ESP_LOGI(TAG, "IN[%d] sz=%u: %02x %02x %02x %02x %02x %02x %02x %02x "
                     "%02x %02x %02x %02x %02x %02x %02x %02x",
                     dbg_in_cnt, (unsigned)jpeg_size,
                     jpeg_data[0], jpeg_data[1], jpeg_data[2], jpeg_data[3],
                     jpeg_data[4], jpeg_data[5], jpeg_data[6], jpeg_data[7],
                     jpeg_data[8], jpeg_data[9], jpeg_data[10], jpeg_data[11],
                     jpeg_data[12], jpeg_data[13], jpeg_data[14], jpeg_data[15]);
            dbg_in_cnt++;
        }
    }
    jpeg_error_t ret = jpeg_dec_parse_header(s_dec, &io, &hdr);
    if (ret != JPEG_ERR_OK)
    {
        ESP_LOGE(TAG, "hdr: %d", ret);
        return ESP_FAIL;
    }
    *out_width = hdr.width;
    *out_height = hdr.height;

    int need = 0;
    jpeg_dec_get_outbuf_len(s_dec, &need);
    if (need > (int)out_buf_size)
    {
        ESP_LOGE(TAG, "buf small");
        return ESP_ERR_NO_MEM;
    }

    int cnt = 1;
    jpeg_dec_get_process_count(s_dec, &cnt);
    for (int i = 0; i < cnt; i++)
    {
        ret = jpeg_dec_process(s_dec, &io);
        if (ret != JPEG_ERR_OK)
        {
            ESP_LOGE(TAG, "decode: %d", ret);
            return ESP_FAIL;
        }
    }
    /* DBG: 打印解码后前 16 像素 */
    {
        static int dbg_dec_cnt = 0;
        if (dbg_dec_cnt < 5) {
            ESP_LOGI(TAG, "DEC[%d] %dx%d: "
                     "%04x %04x %04x %04x %04x %04x %04x %04x "
                     "%04x %04x %04x %04x %04x %04x %04x %04x",
                     dbg_dec_cnt, hdr.width, hdr.height,
                     out_pixels[0], out_pixels[1], out_pixels[2], out_pixels[3],
                     out_pixels[4], out_pixels[5], out_pixels[6], out_pixels[7],
                     out_pixels[8], out_pixels[9], out_pixels[10], out_pixels[11],
                     out_pixels[12], out_pixels[13], out_pixels[14], out_pixels[15]);
            dbg_dec_cnt++;
        }
    }
    return ESP_OK;
}

void mjpeg_decoder_deinit(void)
{
    if (s_dec)
    {
        jpeg_dec_close(s_dec);
        s_dec = NULL;
    }
}

#else
#error "JPEG_DECODER must be JPEG_DECODER_OLD or JPEG_DECODER_NEW"
#endif
