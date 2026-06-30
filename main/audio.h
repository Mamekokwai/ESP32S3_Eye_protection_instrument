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
 * @brief 写入 PCM 采样（由解码器逐帧调用，边解边播）
 * @param samples    int16_t PCM 采样数据
 * @param num_samples 采样数（非字节数）
 * @param sample_rate 采样率 (Hz)
 * @param channels    声道数 (1=单声道, 2=立体声)
 * @note  当前 I2S 固定 16kHz 单声道，不匹配时会有速度/声道差异
 */
void audio_write_pcm(const int16_t *samples, size_t num_samples, int sample_rate, int channels);

/**
 * @brief 设置输出音量
 * @param vol 音量 0~100
 * @return ESP_OK 成功
 */
esp_err_t audio_set_volume(int vol);
