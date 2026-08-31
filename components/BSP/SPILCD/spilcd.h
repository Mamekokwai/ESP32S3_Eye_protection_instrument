#ifndef __SPILCD_H
#define __SPILCD_H

#include <unistd.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_err.h"

/* YT06 V1.4: LCD 8080 8-bit 并口引脚 */
#define LCD_DB0    GPIO_NUM_6
#define LCD_DB1    GPIO_NUM_7
#define LCD_DB2    GPIO_NUM_8
#define LCD_DB3    GPIO_NUM_9
#define LCD_DB4    GPIO_NUM_10
#define LCD_DB5    GPIO_NUM_11
#define LCD_DB6    GPIO_NUM_12
#define LCD_DB7    GPIO_NUM_13
#define LCD_WR     GPIO_NUM_46
#define LCD_DC     GPIO_NUM_38
#define LCD_CS     GPIO_NUM_17    /* CS1 */
#define LCD_RST    GPIO_NUM_3

/* V1.4 硬件: GPIO1 为背光 PWM (PWM_LED -> R17 -> Q3 -> LEDK), 无 TE 连接。
 * 旧 V1.1 曾把 GPIO1 用作 TE; V1.4 起不再有 TE 帧同步。 */
#define LCD_BACKLIGHT GPIO_NUM_1  /* 背光 PWM 输出, 驱动 Q3 */

/* TE 帧同步: V1.4 无 TE 引脚, 保持关闭 (仅兼容旧代码) */
#define LCD_TE     GPIO_NUM_1
#define LCD_TE_ENABLE 0

/* 颜色 */
#define WHITE  0xFFFF
#define BLACK  0x0000
#define BLUE   0x001F
#define RED    0xF800
#define GREEN  0x07E0

typedef struct {
    uint16_t width, height, dir, pwidth, pheight;
} _spilcd_dev;

extern _spilcd_dev spilcddev;
extern esp_lcd_panel_handle_t panel_handle;
extern volatile uint8_t refresh_done_flag;
extern volatile uint32_t refresh_done_count;

esp_err_t spilcd_init(void);
void spilcd_display_dir(uint8_t dir);
void spilcd_clear(uint16_t color);
void spilcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);
void spilcd_fill_raw(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);  /* 不修正颜色 */
void spilcd_draw_point(uint16_t x, uint16_t y, uint16_t color);
void spilcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void spilcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color);
void spilcd_draw_rectangle(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void spilcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color);
void spilcd_show_char(uint16_t x, uint16_t y, uint8_t chr, uint8_t size, uint8_t mode, uint16_t color);
void spilcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color);
void spilcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color);
esp_err_t spilcd_show_text16(uint16_t x, uint16_t y, const char *text,
                             uint16_t foreground, uint16_t background);
void spilcd_wait_te(void);  /* V1.4 无 TE, 空实现 (兼容) */

/* V1.4 背光: GPIO1 PWM 调光 (0-100, 默认 100) */
void spilcd_backlight_set(uint8_t percent);

#endif
