#pragma once

#include "esp_err.h"

/**
 * @brief  播放 SD 卡上的 AVI 视频 (MJPEG+PCM)
 *
 * 初始化 SD 卡、LCD、音频后调用。
 * 播放 SD 卡根目录下第一个 .avi 文件。
 *
 * @return ESP_OK 成功，否则失败
 */
esp_err_t video_player_play(const char *filename);
