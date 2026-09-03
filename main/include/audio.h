#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * 音量控制方式（编译期切换）：
 *   AUDIO_VOLUME_MODE_ES8311  — VOL 通过 I2C 写 ES8311 DAC 音量寄存器
 *   AUDIO_VOLUME_MODE_SOFTWARE — VOL 在 PCM 输出前进行软件幅度缩放
 */
#define AUDIO_VOLUME_MODE_SOFTWARE 0
#define AUDIO_VOLUME_MODE_ES8311   1
#define AUDIO_VOLUME_MODE          AUDIO_VOLUME_MODE_ES8311

/**
 * @brief 初始化音频系统（I2C、I2S、ES8311 和 GPIO2 功放控制）
 * @return ESP_OK 成功
 */
esp_err_t audio_init(void);

/**
 * @brief 查询 ES8311 是否已完成初始化并可输出音频
 */
bool audio_is_ready(void);

/**
 * @brief 控制功放 MUTE 脚
 * @param enabled true=GPIO2 拉高并开启喇叭，false=拉低并静音
 */
esp_err_t audio_amp_set_enabled(bool enabled);

/**
 * @brief 播放 PCM 数据
 * @param data  PCM 音频数据缓冲区 (16bit, 单声道, 44.1kHz)
 * @param len   数据长度（字节）
 * @return ESP_OK 成功
 */
esp_err_t audio_play(const uint8_t *data, size_t len);

/**
 * @brief 校验输入采样率（兼容旧 API；硬件输出固定 44.1kHz）
 */
esp_err_t audio_set_sample_rate(int sample_rate);

/**
 * @brief 写入 PCM 采样（由解码器逐帧调用，边解边播）
 * @param samples    int16_t PCM 采样数据
 * @param num_samples 每声道采样数（非字节数）
 * @param sample_rate 采样率 (Hz)
 * @param channels    声道数 (1=单声道, 2=立体声)
 */
esp_err_t audio_write_pcm(const int16_t *samples, size_t num_samples,
                          int sample_rate, int channels);

/**
 * @brief 按 AUDIO_VOLUME_MODE 设置 ES8311 硬件音量或 PCM 软件音量
 * @param vol 音量 5~100
 * @return ESP_OK 成功；硬件模式下 I2C 写入失败返回 ESP_FAIL
 */
esp_err_t audio_set_volume(int vol);

/** @brief 获取当前音量百分比（0~100）。 */
int audio_get_volume(void);

/* ---- I2C 总线测试/恢复 (示波器诊断用) ---- */
void audio_i2c_test_start(void);
void audio_i2c_test_stop(void);
bool audio_i2c_test_is_running(void);
void audio_i2c_bus_recover(void);
