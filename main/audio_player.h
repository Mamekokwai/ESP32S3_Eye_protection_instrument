#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "video_player.h"  /* player_ret_t */

/**
 * @brief  SD 卡 PCM 音频播放器 (tick 化)
 *
 * 从 SD 卡读取 PCM 文件 (16-bit signed, mono, 16kHz),
 * 每次 tick 读取一块数据写入 I2S 输出。
 *
 * 支持格式: 原始 PCM (无 header)
 */

/**
 * @brief  初始化音频播放器
 * @param  filename  VFS 路径, 如 "/0:/music.pcm"
 * @return ESP_OK 成功
 */
esp_err_t audio_player_init(const char *filename);

/**
 * @brief  推进播放 (每次 tick 读一块数据 → I2S)
 */
player_ret_t audio_player_tick(void);

/**
 * @brief  停止播放, 关闭文件
 */
void audio_player_stop(void);

/**
 * @brief  设置音量 0-100
 */
void audio_player_set_volume(int vol);

/**
 * @brief  切换静音
 * @return true=当前静音
 */
bool audio_player_toggle_mute(void);

/**
 * @brief  获取当前播放文件名 (用于 STATUS)
 */
const char *audio_player_current_file(void);

/**
 * @brief  列出 SD 卡根目录 PCM 文件, 返回文件数
 */
int audio_player_list_files(char *out_buf, size_t out_len);
