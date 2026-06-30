#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief 初始化音频系统（I2C, I2S, ES8311, XL9555 MUTE）
 * @return ESP_OK 成功
 */
esp_err_t audio_init(void);

/**
 * @brief 播放 PCM 数据
 * @param data  PCM 音频数据缓冲区 (16bit, 单声道, 16kHz)
 * @param len   数据长度（字节）
 * @return ESP_OK 成功
 */
esp_err_t audio_play(const uint8_t *data, size_t len);

/**
 * @brief 设置输出音量
 * @param vol 音量 0~100
 * @return ESP_OK 成功
 */
esp_err_t audio_set_volume(int vol);
