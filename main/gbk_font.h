#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ff.h"

/*
 * GBK16 中文字库 (16x16 点阵)
 *
 * 字库来源: TF 卡 /SYSTEM/FONT/GBK16.FON (与 sample 13_spi_sdcard 一致)
 *   布局 (与 sample text.c text_get_hz_mat() 一致):
 *     区号 qh = 0x81..0xFE (126 区), 位号 ql = 0x40..0xFE 跳过 0x7F (190 位)
 *     字形偏移 = ((qh - 0x81) * 190 + ql_index) * 32, 每字 32 字节
 *
 * 显示函数要求字符串为 GBK 编码 (FATFS CODEPAGE_936, d_name 为 GBK 双字节)。
 *
 * 启动提示 (无卡时也用) 由内嵌小字库提供, 不依赖 TF 卡:
 *   请 / 插 / 入 / 卡 / 解 / 密 / 请 (boot gate 提示)
 */

/* ---- 内嵌小字库 (固定提示字, 无卡也显示) ---- */
/** "请插入SD卡" 里的汉字: 请插[入SD]卡 → 请,插,入,卡 */
void gbk_show_boot_text(uint16_t x, uint16_t y, uint16_t color);

/** "请解密" 提示 */
void gbk_show_unlock_text(uint16_t x, uint16_t y, uint16_t color);

/* ---- TF 卡字库 (任意中文文件名) ---- */

/** 字库文件路径 */
#define GBK_FONT_PATH "/SYSTEM/FONT/GBK16.FON"

/** 初始化: 打开 TF 卡上的 GBK16.FON (失败时只能显示内嵌提示字) */
void gbk_font_init(void);

/** 字库是否就绪 (TF 卡 GBK16.FON 已打开) */
bool gbk_font_loaded(void);

/** 取一个 GBK 汉字点阵 (TF 卡字库优先, 内嵌字库兜底) */
esp_err_t gbk_font_get_glyph(const uint8_t code[2], uint8_t mat[32]);

/* 字库文件句柄 (gbk_font.c 内部使用) */
extern FIL s_gbk_file;

/** 显示字符串 (混合 ASCII + GBK, 用 TF 卡字库; 未加载时中文显示为方块) */
void gbk_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                     const char *str, uint16_t color);

/** 显示一个 GBK 汉字 (用 TF 卡字库) */
void gbk_show_font(uint16_t x, uint16_t y, const uint8_t *font, uint16_t color);
