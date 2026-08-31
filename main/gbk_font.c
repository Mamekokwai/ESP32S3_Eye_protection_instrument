/*
 * GBK16 中文字库显示
 *  - 内嵌小字库 (boot_gate 启动提示, 无 TF 卡也显示): gbk_embedded_font.h
 *  - TF 卡字库 (SDLIST 任意中文文件名): /SYSTEM/FONT/GBK16.FON
 */
#include "gbk_font.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "spilcd.h"
#include "gbk_embedded_font.h"

#define TAG "gbk_font"

FIL s_gbk_file; /* TF 卡 GBK16.FON 句柄 */
static bool s_font_loaded;

void gbk_font_init(void)
{
    s_font_loaded = (f_open(&s_gbk_file, GBK_FONT_PATH, FA_READ) == FR_OK);
    if (s_font_loaded)
        ESP_LOGI(TAG, "GBK16 font loaded from %s", GBK_FONT_PATH);
    else
        ESP_LOGW(TAG, "GBK16 font not found at %s (only embedded hint chars)",
                 GBK_FONT_PATH);
}

bool gbk_font_loaded(void)
{
    return s_font_loaded;
}

/* 从内嵌表取点阵 (GBK 码 => 点阵), 供启动提示使用 */
static const uint8_t *embedded_glyph(const uint8_t qh, const uint8_t ql,
                                     uint8_t *out)
{
    for (int i = 0; i < GBK_EMBEDDED_FONT_COUNT; i++)
    {
        if (gbk_embedded_font[i].gbk[0] == qh &&
            gbk_embedded_font[i].gbk[1] == ql)
        {
            memcpy(out, gbk_embedded_font[i].mat, 32);
            return out;
        }
    }
    return NULL;
}

/* 从 TF 卡 GBK16.FON 取点阵 */
static esp_err_t file_glyph(const uint8_t *code, uint8_t mat[32])
{
    if (!s_font_loaded)
        return ESP_ERR_NOT_FOUND;

    uint8_t qh = code[0];
    uint8_t ql = code[1];
    if (qh < 0x81 || ql < 0x40 || ql == 0xFF || qh == 0xFF)
        return ESP_ERR_INVALID_ARG;

    uint8_t ql_idx = (ql < 0x7F) ? (ql - 0x40) : (ql - 0x41);
    uint32_t offset = ((uint32_t)(qh - 0x81) * 190U + ql_idx) * 32U;

    if (f_lseek(&s_gbk_file, offset) != FR_OK)
        return ESP_FAIL;
    UINT br = 0;
    if (f_read(&s_gbk_file, mat, 32, &br) != FR_OK || br != 32)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t gbk_font_get_glyph(const uint8_t code[2], uint8_t mat[32])
{
    if (file_glyph(code, mat) == ESP_OK)
        return ESP_OK;
    return embedded_glyph(code[0], code[1], mat) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void gbk_show_font(uint16_t x, uint16_t y, const uint8_t *font, uint16_t color)
{
    uint8_t dzk[32];
    if (gbk_font_get_glyph(font, dzk) != ESP_OK)
        return;

    uint16_t y0 = y;
    for (int t = 0; t < 32; t++)
    {
        uint8_t temp = dzk[t];
        for (int t1 = 0; t1 < 8; t1++)
        {
            if (temp & 0x80)
                spilcd_draw_point(x, y, color);
            temp <<= 1;
            y++;
            if ((y - y0) == 16)
            {
                y = y0;
                x++;
                break;
            }
        }
    }
}

void gbk_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                     const char *str, uint16_t color)
{
    uint16_t x0 = x;
    uint16_t y0 = y;
    int bHz = 0;
    const uint8_t *pstr = (const uint8_t *)str;

    while (*pstr != 0)
    {
        if (!bHz)
        {
            if (*pstr > 0x80)
            {
                bHz = 1;
            }
            else
            {
                if (x > (x0 + width - 8))
                {
                    y += 16;
                    x = x0;
                }
                if (y > (y0 + height - 16))
                    break;
                if (*pstr == 13)
                {
                    y += 16;
                    x = x0;
                    pstr++;
                }
                else
                {
                    spilcd_show_char(x, y, *pstr, 16, 1, color);
                }
                pstr++;
                x += 8;
            }
        }
        else
        {
            bHz = 0;
            if (x > (x0 + width - 16))
            {
                y += 16;
                x = x0;
            }
            if (y > (y0 + height - 16))
                break;
            gbk_show_font(x, y, pstr, color);
            pstr += 2;
            x += 16;
        }
    }
}

/* ---- boot_gate 提示 (只用内嵌字库, 无卡也显示) ---- */

/* "请插入SD卡": 请(内嵌),插(内嵌),入(内嵌),S,D,卡(内嵌) */
void gbk_show_boot_text(uint16_t x, uint16_t y, uint16_t color)
{
    spilcd_clear(WHITE);
    /* 请 */
    uint8_t qing[2] = {0xC7, 0xEB};
    uint8_t dzk_q[32];
    if (embedded_glyph(qing[0], qing[1], dzk_q))
        gbk_show_font(x, y, qing, color);
    x += 16;
    uint8_t cha[2] = {0xB2, 0xE5};
    gbk_show_font(x, y, cha, color);
    x += 16;
    uint8_t ru[2] = {0xC8, 0xEB};
    gbk_show_font(x, y, ru, color);
    x += 16;
    spilcd_show_char(x, y, 'S', 16, 1, color);
    x += 8;
    spilcd_show_char(x, y, 'D', 16, 1, color);
    x += 8;
    uint8_t ka[2] = {0xBF, 0xA8};
    gbk_show_font(x, y, ka, color);
}

/* "请解密": 请,解,密 */
void gbk_show_unlock_text(uint16_t x, uint16_t y, uint16_t color)
{
    spilcd_clear(WHITE);
    uint8_t qing[2] = {0xC7, 0xEB};
    gbk_show_font(x, y, qing, color);
    x += 16;
    uint8_t jie[2] = {0xBD, 0xE2};
    gbk_show_font(x, y, jie, color);
    x += 16;
    uint8_t mi[2] = {0xC3, 0xDC};
    gbk_show_font(x, y, mi, color);
}
