/**
 * @brief  MJPEG 解码器
 *
 * 使用 esp_jpeg 组件（封装 ROM TJPGD）将 JPEG 帧解码为 RGB565 像素。
 */
#include "mjpeg.h"
#include "esp_log.h"
#include "jpeg_decoder.h"
#include <string.h>

#define TAG "mjpeg"

esp_err_t mjpeg_decoder_init(uint32_t max_width, uint32_t max_height)
{
    /* esp_jpeg 在每次 decode 调用时动态分配，无需全局 init */
    ESP_LOGI(TAG, "Decoder ready (max %lux%lu, software TJPGD)", max_width, max_height);
    return ESP_OK;
}

esp_err_t mjpeg_decoder_decode(const uint8_t *jpeg_data, size_t jpeg_size,
                                uint16_t *out_pixels, size_t out_buf_size,
                                uint32_t *out_width, uint32_t *out_height)
{
    if (jpeg_data == NULL || jpeg_size == 0 || out_pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata      = (uint8_t *)jpeg_data,
        .indata_size = (uint32_t)jpeg_size,
        .outbuf      = (uint8_t *)out_pixels,
        .outbuf_size = (uint32_t)out_buf_size,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
        .flags = {
            .swap_color_bytes = 1, /* 字节交换以匹配 ST7789 SPI 格式 */
        }
    };

    esp_jpeg_image_output_t outimg;
    esp_err_t ret = esp_jpeg_decode(&jpeg_cfg, &outimg);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Decode failed: %d", ret);
        return ret;
    }

    *out_width  = outimg.width;
    *out_height = outimg.height;

    return ESP_OK;
}

void mjpeg_decoder_deinit(void)
{
    /* esp_jpeg 无全局资源需释放 */
}
