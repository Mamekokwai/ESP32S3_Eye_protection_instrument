#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief  初始化 MJPEG 解码器 (分配工作缓冲)
 * @param  max_width  最大图像宽度
 * @param  max_height 最大图像高度
 * @return ESP_OK 成功
 */
esp_err_t mjpeg_decoder_init(uint32_t max_width, uint32_t max_height);

/**
 * @brief  解码一帧 JPEG → RGB565 像素
 * @param  jpeg_data    JPEG 帧数据
 * @param  jpeg_size    JPEG 数据大小
 * @param  out_pixels   输出 RGB565 像素缓冲 (调用者分配, width*height*2)
 * @param  out_buf_size 缓冲大小
 * @param  out_width    输出图像宽度
 * @param  out_height   输出图像高度
 * @return ESP_OK 成功
 */
esp_err_t mjpeg_decoder_decode(const uint8_t *jpeg_data, size_t jpeg_size,
                                uint16_t *out_pixels, size_t out_buf_size,
                                uint32_t *out_width, uint32_t *out_height);

/**
 * @brief  释放 MJPEG 解码器资源
 */
void mjpeg_decoder_deinit(void);
