#pragma once

#include <stdint.h>

#include "esp_err.h"

/* 用户可调参数的掉电保存：取值均为 0~100。 */
typedef struct
{
    uint8_t volume;
    uint8_t backlight;
} app_settings_t;

/** 初始化 NVS；无效/版本不兼容时自动修复 NVS 分区。 */
esp_err_t app_settings_init(void);

/** 读取已保存参数；NVS 无记录或数值越界时返回默认值。 */
void app_settings_load(app_settings_t *settings);

esp_err_t app_settings_save_volume(uint8_t volume);
esp_err_t app_settings_save_backlight(uint8_t backlight);
