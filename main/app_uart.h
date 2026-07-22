#pragma once

/**
 * @brief  UART 指令系统 (CA51F352P4 → ESP32)
 *
 * UART1: RX=IO44, TX=IO38, 115200-8N1, 文本协议 (\n 终止)
 */

void app_uart_init(void);
void app_uart_tick(void);       /* 每 5ms: 接收 + 解析指令 */
void app_uart_send(const char *msg);  /* 发送响应 */
void app_uart_inject(const char *cmd); /* 注入指令, 模拟 CA51 发送 */
