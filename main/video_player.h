#pragma once

#include <stddef.h>

#include "esp_err.h"

/* 播放器 tick 返回值 */
typedef enum {
    PLAYER_BUSY = 0,   /* 本次 tick 无事可做 (等待解码/等待LCD/未到帧时间) */
    PLAYER_OK   = 1,   /* 本次 tick 完成了一帧 */
    PLAYER_ERROR = -1, /* 不可恢复错误, 需 stop */
} player_ret_t;

/**
 * @brief  初始化 SD 卡 AVI 播放器
 *
 * 打开文件, 解析 AVI 头, 分配缓冲, 启动 Core1 解码任务, 预读前两帧。
 * 调用后即可用 video_player_tick() 逐帧推进。
 *
 * @param filename  VFS 路径, 如 "/0:/video.avi"
 * @return ESP_OK 成功
 */
esp_err_t video_player_init(const char *filename);

/**
 * @brief 递归列出 TF 卡目录树中的 AVI 文件
 *
 * 输出格式与 IMGLIST 一致：每行 "序号=文件名"。
 */
int video_player_list_files(char *output, size_t output_size);

/**
 * @brief 按序号或文件名启动 TF 卡 AVI
 *
 * @param selection "1" 或 "demo.avi"
 */
esp_err_t video_player_start(const char *selection);

/** @brief 当前 TF 卡视频文件名 */
const char *video_player_name(void);

/**
 * @brief  推进播放器一帧 (非阻塞, 每 ~5ms 调用一次)
 *
 * @return PLAYER_OK    本次完成了一帧
 *         PLAYER_BUSY  无事可做 (解码中/等LCD/等帧率)
 *         PLAYER_ERROR 错误, 需调用 video_player_stop()
 */
player_ret_t video_player_tick(void);

/**
 * @brief  停止播放, 释放所有资源
 */
void video_player_stop(void);

/** Reclaim strip buffers after any pending LCD DMA has completed. */
void video_player_reclaim_buffers(void);

/* ---- 兼容旧 API (阻塞版, 仅供过渡) ---- */
esp_err_t video_player_play(const char *filename);
