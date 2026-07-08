#pragma once
#include "esp_err.h"

/**
 * @brief  播放 Flash storage 分区中的 AVI 视频 (内存映射, 零拷贝)
 *
 * 不占用 SPI2 总线, SD 卡和 LCD 可独立运行。
 * 先用 tools/flash_video.sh 把 .avi 写入 storage 分区。
 */
esp_err_t flash_video_play(void);
