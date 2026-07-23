#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "video_player.h"  /* player_ret_t */

/**
 * @brief  SD 卡 PCM/MP3 音频播放器 (tick 化)
 *
 * PCM 使用 16-bit signed/mono/16kHz；MP3 按源采样率流式解码，
 * 立体声下混为单声道后写入 ES8311。
 *
 * 支持格式: 原始 PCM (无 header)、MP3 Layer III
 */

/**
 * @brief  启动 CPU1 音频服务任务
 */
esp_err_t audio_player_start_service(void);

/**
 * @brief  初始化音频播放器
 * @param  filename  VFS 路径, 如 "/0:/music.pcm"
 * @return ESP_OK 成功
 */
esp_err_t audio_player_init(const char *filename);

/**
 * @brief  按序号或根目录文件名启动音频
 * @param  selection "1"、"music.mp3" 或 "music.pcm"
 */
esp_err_t audio_player_start(const char *selection);

/**
 * @brief  推进播放 (每次 tick 读一块数据 → I2S)
 */
player_ret_t audio_player_tick(void);

/**
 * @brief  停止播放, 关闭文件
 */
void audio_player_stop(void);

/**
 * @brief  查询音频播放器是否正在运行
 */
bool audio_player_is_active(void);

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
 * @brief  列出 SD 卡根目录 PCM/MP3 文件, 返回文件数
 */
int audio_player_list_files(char *out_buf, size_t out_len);
