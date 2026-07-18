#pragma once
#include "esp_err.h"
#include "video_player.h"  /* player_ret_t */

/**
 * @brief  初始化 Flash storage 分区 AVI 播放器
 *
 * 查找 storage 分区, mmap, 解析 AVI 头, 启动 Core1 解码任务。
 *
 * @return ESP_OK 成功
 */
esp_err_t flash_player_init(void);

/**
 * @brief  推进播放器一帧 (非阻塞, 每 ~5ms 调用一次)
 */
player_ret_t flash_player_tick(void);

/**
 * @brief  停止播放, 释放所有资源 (munmap, 释放缓冲)
 */
void flash_player_stop(void);

/* ---- 兼容旧 API ---- */
esp_err_t flash_video_play(void);
