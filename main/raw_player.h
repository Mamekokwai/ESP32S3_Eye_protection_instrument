#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "video_player.h"  /* player_ret_t */

/**
 * @brief  初始化 SD 卡 RAW/RGB565 播放器
 *
 * 打开文件, 解析 header, 分配缓冲。
 *
 * @param filename  VFS 路径, 如 "/0:/video.raw"
 * @return ESP_OK 成功
 */
esp_err_t raw_player_init(const char *filename);

/**
 * @brief  推进播放器一帧 (非阻塞, 每 ~5ms 调用一次)
 */
player_ret_t raw_player_tick(void);

/**
 * @brief  停止播放, 释放资源
 */
void raw_player_stop(void);

/* ---- 兼容旧 API ---- */
esp_err_t raw_player_play(const char *filename);
