/**
 * @brief  LCD 驱动 (YT06 V1.1: 8080 8-bit 并口, 320x320 圆屏, JD9855 IC)
 *
 * 使用 esp_lcd_panel_io_i80 + esp_lcd_new_panel_jd9855
 */
#include "spilcd.h"
#include "spilcdfont.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_ops.h"
#include <math.h>
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_jd9855.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define LCD_CS2   GPIO_NUM_18   /* 右眼屏片选 (和 CS1=IO17 配合) */

#define TAG "spilcd"

volatile uint8_t refresh_done_flag = 0;
esp_lcd_panel_handle_t panel_handle = NULL;
_spilcd_dev spilcddev;

static bool notify_lcd_flush_ready(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    refresh_done_flag = 1;
    return false;
}

/* ---- 8080 并口 LCD 初始化 (双屏同画面, CS1+CS2) ---- */
esp_err_t spilcd_init(void)
{
    /* 0. CS2 拉低使能右眼屏 (CS1 由 i80 驱动管理, CS2 常低) */
    gpio_config_t cs2_cfg = {
        .pin_bit_mask = BIT64(LCD_CS2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs2_cfg);
    gpio_set_level(LCD_CS2, 0);  /* CS2 常低, 和 CS1 同步接收数据 */
    ESP_LOGI(TAG, "Dual LCD: CS1=IO%d (i80), CS2=IO%d (manual LOW)", LCD_CS, LCD_CS2);

    /* 1. 创建 i80 总线 */
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = LCD_DC,
        .wr_gpio_num = LCD_WR,
        .clk_src     = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            LCD_DB0, LCD_DB1, LCD_DB2, LCD_DB3,
            LCD_DB4, LCD_DB5, LCD_DB6, LCD_DB7,
        },
        .bus_width   = 8,
        .max_transfer_bytes = 320 * 320 * 2,
        .psram_trans_align  = 64,
        .sram_trans_align   = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    /* 2. 创建 panel IO */
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .pclk_hz     = 10 * 1000 * 1000,  /* 10MHz 写时钟 */
        .trans_queue_depth = 7,
        .on_color_trans_done = notify_lcd_flush_ready,
        .dc_levels = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
        .lcd_cmd_bits   = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle));

    /* 3. LCD 面板配置 */
    spilcddev.pwidth  = 320;
    spilcddev.pheight = 320;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,
    };

    esp_err_t ret = esp_lcd_new_panel_jd9855(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JD9855 init failed: %d", ret);
        return ret;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    spilcd_display_dir(1);
    spilcd_clear(BLACK);

    ESP_LOGI(TAG, "LCD JD9855 i80 init OK (320x320)");
    return ESP_OK;
}

void spilcd_display_dir(uint8_t dir)
{
    spilcddev.dir = dir;
    if (dir == 0) {
        spilcddev.width  = spilcddev.pheight;
        spilcddev.height = spilcddev.pwidth;
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, false);
    } else {
        spilcddev.width  = spilcddev.pwidth;
        spilcddev.height = spilcddev.pheight;
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, false);
    }
}

void spilcd_clear(uint16_t color)
{
    uint16_t *buf = heap_caps_malloc(spilcddev.width * 40 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) return;
    uint16_t ct = ((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8);
    for (int i = 0; i < spilcddev.width * 40; i++) buf[i] = ct;
    for (int y = 0; y < spilcddev.height; y += 40)
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, spilcddev.width, y + 40, buf);
    refresh_done_flag = 0;
    while (!refresh_done_flag) vTaskDelay(1);
    heap_caps_free(buf);
}

void spilcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    uint16_t w = ex - sx, h = ey - sy;
    uint16_t *buf = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf) return;
    uint16_t ct = ((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8);
    for (int i = 0; i < w; i++) buf[i] = ct;
    for (int y = 0; y < h; y++)
        esp_lcd_panel_draw_bitmap(panel_handle, sx, sy + y, ex, sy + y + 1, buf);
    heap_caps_free(buf);
}

void spilcd_draw_point(uint16_t x, uint16_t y, uint16_t color)
{
    uint16_t ct = ((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8);
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + 1, &ct);
}

/* 以下函数和旧版相同, 只保留接口兼容性 */
void spilcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        spilcd_draw_point(x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 < dx)  { err += dx; y1 += sy; }
    }
}

void spilcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color)
{
    if (len == 0 || x >= spilcddev.width || y >= spilcddev.height) return;
    uint16_t ex = fmin(spilcddev.width - 1, x + len - 1);
    spilcd_fill(x, y, ex + 1, y + 1, color);
}

void spilcd_draw_rectangle(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    spilcd_draw_line(x0, y0, x1, y0, color);
    spilcd_draw_line(x0, y0, x0, y1, color);
    spilcd_draw_line(x0, y1, x1, y1, color);
    spilcd_draw_line(x1, y0, x1, y1, color);
}

void spilcd_draw_circle(uint16_t x0, uint16_t y0, uint16_t r, uint16_t color)
{
    int a = 0, b = r, di = 3 - (r << 1);
    while (a <= b) {
        spilcd_draw_point(x0 + a, y0 + b, color); spilcd_draw_point(x0 - a, y0 + b, color);
        spilcd_draw_point(x0 + a, y0 - b, color); spilcd_draw_point(x0 - a, y0 - b, color);
        spilcd_draw_point(x0 + b, y0 + a, color); spilcd_draw_point(x0 - b, y0 + a, color);
        spilcd_draw_point(x0 + b, y0 - a, color); spilcd_draw_point(x0 - b, y0 - a, color);
        a++;
        if (di < 0) di += 4 * a + 6;
        else { di += 10 + 4 * (a - b); b--; }
    }
}

void spilcd_show_char(uint16_t x, uint16_t y, uint8_t chr, uint8_t size, uint8_t mode, uint16_t color)
{
    /* 简化: 用 fill 画占位块 */
    spilcd_fill(x, y, x + size / 2, y + size, color);
}

void spilcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color) {}
void spilcd_show_string(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t size, char *p, uint16_t color) {}
