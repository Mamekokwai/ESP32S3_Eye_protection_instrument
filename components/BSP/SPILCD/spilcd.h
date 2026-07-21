#ifndef __SPILCD_H
#define __SPILCD_H

#include <unistd.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_err.h"

/* YT06 V1.1: LCD 8080 8-bit 并口引脚 */
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
#define LCD_TE     GPIO_NUM_1
#define LCD_RST    GPIO_NUM_3

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

esp_err_t spilcd_init(void);
uint16_t lcd_color_fix(uint16_t c);  /* 限制 R 分量避免地弹 */
void spilcd_display_dir(uint8_t dir);
void spilcd_clear(uint16_t color);
void spilcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color);
void spilcd_draw_point(uint16_t x, uint16_t y, uint16_t color);
void spilcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void spilcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color);
void spilcd_draw_rectangle(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
void spilcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color);
void spilcd_show_char(uint16_t x, uint16_t y, uint8_t chr, uint8_t size, uint8_t mode, uint16_t color);
void spilcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color);
void spilcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color);

#endif
