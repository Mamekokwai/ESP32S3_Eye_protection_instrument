/**
 * @brief  LCD 驱动 (YT06 V1.1: 8080 8-bit 并口, 320x320 圆屏, JD9855 IC)
 *
 * 使用 esp_lcd_panel_io_i80 + esp_lcd_new_panel_jd9855
 */
#include "spilcd.h"
#include "spilcdfont.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_ops.h"
#include <stdio.h>
#include <math.h>
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_jd9855.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define LCD_CS2 GPIO_NUM_18 /* 右眼屏片选 (和 CS1=IO17 配合) */
#define LCD_DUAL 1          /* 0=单屏测试, 1=双屏同画面 */

#define TAG "spilcd"

volatile uint8_t refresh_done_flag = 0;
volatile uint32_t refresh_done_count = 0;
esp_lcd_panel_handle_t panel_handle = NULL;
_spilcd_dev spilcddev;

static bool notify_lcd_flush_ready(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *edata,
                                   void *user_ctx)
{
    refresh_done_flag = 1;
    refresh_done_count++;
    return false;
}

/* ---- 8080 并口 LCD 初始化 (双屏同画面, CS1+CS2) ---- */
esp_err_t spilcd_init(void)
{
    /* 0. CS2 (右眼屏片选, CS1 由 i80 驱动管理) */
    gpio_config_t cs2_cfg = {
        .pin_bit_mask = BIT64(LCD_CS2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs2_cfg);
#if LCD_DUAL
    gpio_set_level(LCD_CS2, 0); /* CS2 常低, 和 CS1 同步接收数据 */
    // gpio_set_level(LCD_CS2, 1); /* CS2 常高, 不接受数据 */
    ESP_LOGI(TAG, "Dual LCD: CS1=IO%d (i80), CS2=IO%d (manual LOW)", LCD_CS, LCD_CS2);
#else
    gpio_set_level(LCD_CS2, 1); /* CS2 高=禁用, 仅测 CS1 单屏 */
    ESP_LOGI(TAG, "Single LCD: CS1=IO%d (i80), CS2=IO%d (disabled)", LCD_CS, LCD_CS2);
#endif

    /* TE 引脚配置 (IO1, LCD tearing effect 输入) */
    gpio_config_t te_cfg = {
        .pin_bit_mask = BIT64(LCD_TE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&te_cfg);

    /* 1. 创建 i80 总线 */
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = LCD_DC,
        .wr_gpio_num = LCD_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            LCD_DB0,
            LCD_DB1,
            LCD_DB2,
            LCD_DB3,
            LCD_DB4,
            LCD_DB5,
            LCD_DB6,
            LCD_DB7,
        },
        .bus_width = 8,
        .max_transfer_bytes = 320 * 320 * 2,
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    /* 2. 创建 panel IO */
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40 * 1000 * 1000, /* 40MHz 写时钟 <50MHZ */
        .trans_queue_depth = 2, /* 与双 SRAM 条带缓冲匹配 */
        .on_color_trans_done = notify_lcd_flush_ready,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .swap_color_bytes = 1,
        },
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_cfg, &io_handle));

    /* 3. LCD 面板配置 */
    spilcddev.pwidth = 320;
    spilcddev.pheight = 320;

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };

    esp_err_t ret = esp_lcd_new_panel_jd9855(io_handle, &panel_cfg, &panel_handle);
    if (ret != ESP_OK)
    {
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
    if (dir == 0)
    {
        spilcddev.width = spilcddev.pheight;
        spilcddev.height = spilcddev.pwidth;
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, false);
    }
    else
    {
        spilcddev.width = spilcddev.pwidth;
        spilcddev.height = spilcddev.pheight;
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, false);
    }
}

void spilcd_clear(uint16_t color)
{
    spilcd_fill(0, 0, spilcddev.width, spilcddev.height, color);
}

void spilcd_fill_raw(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color); /* 不做颜色修正 */

void spilcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    if (sx >= ex || sy >= ey || ex > spilcddev.width || ey > spilcddev.height)
        return;
    uint16_t w = ex - sx, h = ey - sy;
    const uint16_t max_rows = 40;
    uint16_t rows = h < max_rows ? h : max_rows;
    size_t pixels = (size_t)w * rows;
    uint16_t *buf = heap_caps_malloc(pixels * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf)
        return;
    for (size_t i = 0; i < pixels; i++)
        buf[i] = color;
    for (uint16_t y = 0; y < h; y += rows)
    {
        uint16_t chunk_rows = (y + rows > h) ? h - y : rows;
        refresh_done_flag = 0;
        if (esp_lcd_panel_draw_bitmap(panel_handle, sx, sy + y, ex,
                                      sy + y + chunk_rows, buf) != ESP_OK)
            break;
        while (!refresh_done_flag)
            vTaskDelay(1);
    }
    heap_caps_free(buf);
}

void spilcd_fill_raw(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t color)
{
    spilcd_fill(sx, sy, ex, ey, color);
}

void spilcd_draw_point(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= spilcddev.width || y >= spilcddev.height)
        return;
    refresh_done_flag = 0;
    if (esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + 1, &color) != ESP_OK)
        return;
    while (!refresh_done_flag)
        vTaskDelay(1);
}

/* 以下函数和旧版相同, 只保留接口兼容性 */
void spilcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    while (1)
    {
        spilcd_draw_point(x1, y1, color);
        if (x1 == x2 && y1 == y2)
            break;
        int e2 = err * 2;
        if (e2 > -dy)
        {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx)
        {
            err += dx;
            y1 += sy;
        }
    }
}

void spilcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color)
{
    if (len == 0 || x >= spilcddev.width || y >= spilcddev.height)
        return;
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
    while (a <= b)
    {
        spilcd_draw_point(x0 + a, y0 + b, color);
        spilcd_draw_point(x0 - a, y0 + b, color);
        spilcd_draw_point(x0 + a, y0 - b, color);
        spilcd_draw_point(x0 - a, y0 - b, color);
        spilcd_draw_point(x0 + b, y0 + a, color);
        spilcd_draw_point(x0 - b, y0 + a, color);
        spilcd_draw_point(x0 + b, y0 - a, color);
        spilcd_draw_point(x0 - b, y0 - a, color);
        a++;
        if (di < 0)
            di += 4 * a + 6;
        else
        {
            di += 10 + 4 * (a - b);
            b--;
        }
    }
}

void spilcd_show_char(uint16_t x, uint16_t y, uint8_t chr, uint8_t size, uint8_t mode, uint16_t color)
{
    /* 逐行 buffer → draw_bitmap, 和 spilcd_fill 方式一致 */
    if (chr < 32 || chr > 126)
        chr = 32;
    uint8_t idx = chr - 32;
    uint8_t ch_width = size / 2;
    uint8_t ch_height = size;
    uint8_t byte_width = (ch_width + 7) / 8;
    const uint8_t *font = NULL;

    switch (size)
    {
    case 12:
        font = (uint8_t *)asc2_1206[idx];
        break;
    case 16:
        font = (uint8_t *)asc2_1608[idx];
        break;
    case 24:
        font = (uint8_t *)asc2_2412[idx];
        break;
    case 32:
        font = (uint8_t *)asc2_3216[idx];
        break;
    default:
        return;
    }

    uint16_t bg = 0xFFFF; /* 白色背景 */
    uint16_t *row_buf = heap_caps_malloc(ch_width * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!row_buf)
        return;

    for (int row = 0; row < ch_height; row++)
    {
        for (int col = 0; col < ch_width; col++)
        {
            uint8_t byte_pos = row * byte_width + col / 8;
            if (size == 24 && (byte_pos % 2) && col % 8 >= 4)
            {
                row_buf[col] = bg;
                continue;
            }
            uint8_t bit_pos = 7 - (col % 8);
            if (font[byte_pos] & (1 << bit_pos))
                row_buf[col] = color;
            else if (mode == 0)
                row_buf[col] = bg;
        }
        refresh_done_flag = 0;
        esp_lcd_panel_draw_bitmap(panel_handle, x, y + row, x + ch_width, y + row + 1, row_buf);
        while (!refresh_done_flag)
            vTaskDelay(1); /* 等每行传完 */
    }
    heap_caps_free(row_buf);
}

static uint32_t lcd_pow(uint8_t m, uint8_t n)
{
    uint32_t result = 1;
    while (n--)
        result *= m;
    return result;
}

void spilcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color)
{
    uint8_t t, temp;
    uint8_t enshow = 0;
    for (t = 0; t < len; t++)
    {
        temp = (num / lcd_pow(10, len - t - 1)) % 10;
        if (enshow == 0 && t < (len - 1))
        {
            if (temp == 0)
            {
                spilcd_show_char(x + (size / 2) * t, y, ' ', size, 0, color);
                continue;
            }
            else
                enshow = 1;
        }
        spilcd_show_char(x + (size / 2) * t, y, temp + '0', size, 0, color);
    }
}

void spilcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color)
{
    uint8_t x0 = x;
    width += x;
    height += y;
    while ((*p <= '~') && (*p >= ' '))
    {
        if (x >= width)
        {
            x = x0;
            y += size;
        }
        if (y >= height)
            break;
        spilcd_show_char(x, y, *p, size, 0, color);
        x += size / 2;
        p++;
    }
}

/* ---- TE 帧同步: 等待 LCD 垂直消隐, 抢先写入 ---- */
void spilcd_wait_te(void)
{
#if !LCD_TE_ENABLE
    return;
#else
    static bool first = true;
    int64_t t0 = esp_timer_get_time();
    if (first)
    {
        /* 诊断: 用最快速度采样 50ms，检测 GPIO1 是否有任何翻转 */
        int hi = 0, lo = 0, edges = 0;
        int prev = gpio_get_level(LCD_TE);
        int64_t t1 = esp_timer_get_time();
        while (esp_timer_get_time() - t1 < 50000)
        {
            int cur = gpio_get_level(LCD_TE);
            if (cur)
                hi++;
            else
                lo++;
            if (cur != prev)
                edges++;
            prev = cur;
        }
        ESP_LOGI(TAG, "TE diag: hi=%d lo=%d edges=%d level=%d (pull-up %s)",
                 hi, lo, edges, gpio_get_level(LCD_TE),
                 (gpio_get_level(LCD_TE) == 1) ? "OK=HIGH" : "LOW=maybe open-drain w/o ext pull-up");
        first = false;
    }
    /* TE=1 = V-blank (可写入), TE=0 = 扫描中.
     * 等 TE 拉高 → V-blank 期间抢先写顶部, 扫描线在后面追.
     * 先等 TE=0 (确保不在 blank 内), 再等 TE=1 (blank 开始). */
    while (gpio_get_level(LCD_TE) == 1) {
        if (esp_timer_get_time() - t0 > 50000) return;
    }
    while (gpio_get_level(LCD_TE) == 0) {
        if (esp_timer_get_time() - t0 > 50000) return;
    }
#endif
}
