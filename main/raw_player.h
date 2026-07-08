#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief  播放 .raw 原始视频文件 (RGB565)
 *
 * 文件格式:
 *   Header (14B): magic "RAWV"(4) + width(2) + height(2) + fps(2) + frames(4)
 *   Data: width*height*2 字节/帧, 连续存放
 *
 * @param filename  VFS 路径, 如 "/0:/video.raw"
 * @return ESP_OK 成功
 */
esp_err_t raw_player_play(const char *filename);
