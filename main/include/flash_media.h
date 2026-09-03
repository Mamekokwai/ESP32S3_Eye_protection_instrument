#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define FLASH_MEDIA_INDEX_SIZE 4096U
#define FLASH_MEDIA_NAME_SIZE  48U
#define FLASH_MEDIA_MAX_ENTRIES 63U

/* 可选 GBK16 字库区 (位于索引区后, 固定偏移 INDEX_SIZE) */
#define FLASH_MEDIA_FONT_MAGIC "GBK16F"
#define FLASH_MEDIA_FONT_MAGIC_SIZE 8U   /* 6字节magic + 2字节保留 */
#define FLASH_MEDIA_FONT_GLYPH 32U       /* 16x16 点阵 32 字节 */
#define FLASH_MEDIA_FONT_ROWS 126U
#define FLASH_MEDIA_FONT_COLS 190U
#define FLASH_MEDIA_FONT_SIZE \
    (FLASH_MEDIA_FONT_GLYPH * FLASH_MEDIA_FONT_ROWS * FLASH_MEDIA_FONT_COLS)

typedef enum
{
    FLASH_MEDIA_VIDEO = 1,
    FLASH_MEDIA_IMAGE = 2,
} flash_media_type_t;

/*
 * storage 分区中的固定64字节索引项。
 * tools/flash_media_pack.py 使用相同的小端格式生成。
 */
typedef struct
{
    uint8_t type;
    uint8_t flags;
    uint16_t reserved;
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    char name[FLASH_MEDIA_NAME_SIZE];
} flash_media_entry_t;

/** 初始化并常驻映射Flash媒体分区；重复调用安全。 */
esp_err_t flash_media_init(void);

/** 按类型列出媒体，输出格式为“序号=文件名\n”。 */
int flash_media_list(flash_media_type_t type, char *output,
                     size_t output_size);

/** 按从1开始的序号或文件名选择媒体。 */
esp_err_t flash_media_resolve(flash_media_type_t type,
                              const char *selection,
                              const flash_media_entry_t **entry);

/** 返回索引项对应的只读mmap地址。 */
const uint8_t *flash_media_data(const flash_media_entry_t *entry);

/**
 * @brief 返回 GBK16 字库 mmap 指针; 无字库返回 NULL。
 * 字库区位于 storage 索引区后固定偏移, 格式:
 *   "GBK16F\0\0" (8字节) + 766,080 字节点阵
 * 字形偏移 = ((qh-0x81)*190 + ql_index) * 32, 与 sample text.c 一致。
 */
const uint8_t *flash_media_font_data(void);

