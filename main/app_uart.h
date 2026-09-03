#pragma once

#include <stdbool.h>

typedef enum
{
    DISPLAY_IDLE = 0,
    DISPLAY_VIDEO_PLAYING,
    DISPLAY_VIDEO_PAUSED,
    DISPLAY_SD_VIDEO_PLAYING,
    DISPLAY_SD_VIDEO_PAUSED,
    DISPLAY_IMAGE_LOADING,
    DISPLAY_SLEEP,
} display_mode_t;

extern display_mode_t g_display_mode;

/**
 * @brief  UART 指令系统 (CA51F352P4 → ESP32)
 *
 * UART1: RX=IO44、TX=IO43, 115200-8N1, 文本协议 (\n 终止)
 * CA51 的 DBG 行转发到 USB JTAG 后不进入业务解析；IO38 是 LCD D/C。
 */

void app_uart_init(void);
void app_uart_tick(void);       /* 每 5ms: 接收 + 解析指令 */
void app_uart_send(const char *msg);  /* 发送响应 */
void app_uart_send_gbk(const char *msg); /* 发送含FATFS GBK路径的响应 */
void app_uart_inject(const char *cmd); /* 注入指令, 模拟 CA51 发送 */

/* SD 卡异常时切换到 Flash 中的 SDCard.jpg；返回是否已开始异步显示。 */
bool app_uart_start_sd_error_image(void);
