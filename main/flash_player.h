#pragma once
#include <stddef.h>

#include "esp_err.h"
#include "video_player.h"  /* player_ret_t */

/**
 * @brief 启动Flash中的第一个AVI（兼容旧API）
 */
esp_err_t flash_player_init(void);

/** 列出Flash索引中的AVI，每行“序号=文件名”。 */
int flash_player_list_files(char *output, size_t output_size);

/** 按从1开始的序号或文件名启动Flash AVI。 */
esp_err_t flash_player_start(const char *selection);

/** 当前Flash视频文件名。 */
const char *flash_player_name(void);

/**
 * @brief  推进播放器一帧 (非阻塞, 每 ~5ms 调用一次)
 */
player_ret_t flash_player_tick(void);

/**
 * @brief  停止播放并释放解码、帧缓冲资源
 */
void flash_player_stop(void);

/* ---- 兼容旧 API ---- */
esp_err_t flash_video_play(void);
