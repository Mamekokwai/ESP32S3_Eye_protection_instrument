#include "flash_media.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"

#define TAG "flash_media"
#define FLASH_MEDIA_MAGIC   0x31444D46U /* "FMD1" */
#define FLASH_MEDIA_VERSION_V1 1U
#define FLASH_MEDIA_VERSION_V2 2U

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t entry_count;
    uint16_t entry_size;
    uint16_t reserved;
    uint32_t index_size;
} flash_media_header_t;

_Static_assert(sizeof(flash_media_header_t) == 16,
               "flash media header layout changed");
_Static_assert(sizeof(flash_media_entry_t) == 64,
               "flash media entry layout changed");

static bool s_init_attempted;
static esp_err_t s_init_result = ESP_ERR_INVALID_STATE;
static const esp_partition_t *s_partition;
static const uint8_t *s_partition_data;
static spi_flash_mmap_handle_t s_mmap_handle;
static const flash_media_entry_t *s_entries;
static uint16_t s_entry_count;
static bool s_legacy_mode;
static flash_media_entry_t s_legacy_entry;

static bool media_type_valid(uint8_t type)
{
    return type == FLASH_MEDIA_VIDEO || type == FLASH_MEDIA_IMAGE;
}

static bool entry_valid(const flash_media_entry_t *entry)
{
    if (!media_type_valid(entry->type) || entry->size == 0 ||
        entry->offset < FLASH_MEDIA_INDEX_SIZE ||
        (entry->offset & (FLASH_MEDIA_INDEX_SIZE - 1U)) != 0)
        return false;
    if (entry->offset > s_partition->size ||
        entry->size > s_partition->size - entry->offset)
        return false;
    return memchr(entry->name, '\0', sizeof(entry->name)) != NULL &&
           entry->name[0] != '\0';
}

static esp_err_t use_legacy_single_avi(void)
{
    if (s_partition->size < 12 ||
        memcmp(s_partition_data, "RIFF", 4) != 0 ||
        memcmp(s_partition_data + 8, "AVI ", 4) != 0)
        return ESP_ERR_NOT_FOUND;

    uint32_t riff_payload_size;
    memcpy(&riff_payload_size, s_partition_data + 4,
           sizeof(riff_payload_size));
    uint64_t file_size = (uint64_t)riff_payload_size + 8U;
    if (file_size < 12 || file_size > s_partition->size)
        return ESP_ERR_INVALID_SIZE;

    memset(&s_legacy_entry, 0, sizeof(s_legacy_entry));
    s_legacy_entry.type = FLASH_MEDIA_VIDEO;
    s_legacy_entry.offset = 0;
    s_legacy_entry.size = (uint32_t)file_size;
    snprintf(s_legacy_entry.name, sizeof(s_legacy_entry.name),
             "legacy.avi");
    s_entries = &s_legacy_entry;
    s_entry_count = 1;
    s_legacy_mode = true;
    ESP_LOGW(TAG, "Legacy single AVI detected (%lu bytes)",
             (unsigned long)s_legacy_entry.size);
    return ESP_OK;
}

esp_err_t flash_media_init(void)
{
    if (s_init_attempted)
        return s_init_result;
    s_init_attempted = true;

    s_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        "storage");
    if (!s_partition)
    {
        s_init_result = ESP_ERR_NOT_FOUND;
        return s_init_result;
    }

    const void *mapped = NULL;
    s_init_result = esp_partition_mmap(
        s_partition, 0, s_partition->size, SPI_FLASH_MMAP_DATA,
        &mapped, &s_mmap_handle);
    if (s_init_result != ESP_OK)
        return s_init_result;
    s_partition_data = mapped;

    const flash_media_header_t *header =
        (const flash_media_header_t *)s_partition_data;
    if (header->magic != FLASH_MEDIA_MAGIC)
    {
        s_init_result = use_legacy_single_avi();
        return s_init_result;
    }

    /* v2 keeps the v1 index/entry layout and only reserves an optional
     * GBK16 font area between the index and the first media payload. */
    if ((header->version != FLASH_MEDIA_VERSION_V1 &&
         header->version != FLASH_MEDIA_VERSION_V2) ||
        header->entry_size != sizeof(flash_media_entry_t) ||
        header->index_size != FLASH_MEDIA_INDEX_SIZE ||
        header->entry_count == 0 ||
        header->entry_count > FLASH_MEDIA_MAX_ENTRIES)
    {
        ESP_LOGE(TAG,
                 "Invalid index: version=%u count=%u entry=%u index=%lu",
                 header->version, header->entry_count, header->entry_size,
                 (unsigned long)header->index_size);
        s_init_result = ESP_ERR_INVALID_RESPONSE;
        return s_init_result;
    }

    s_entries = (const flash_media_entry_t *)(header + 1);
    s_entry_count = header->entry_count;
    for (uint16_t i = 0; i < s_entry_count; i++)
    {
        if (!entry_valid(&s_entries[i]))
        {
            ESP_LOGE(TAG, "Invalid media entry %u", i + 1);
            s_init_result = ESP_ERR_INVALID_RESPONSE;
            return s_init_result;
        }
    }

    int videos = 0;
    int images = 0;
    for (uint16_t i = 0; i < s_entry_count; i++)
    {
        if (s_entries[i].type == FLASH_MEDIA_VIDEO)
            videos++;
        else if (s_entries[i].type == FLASH_MEDIA_IMAGE)
            images++;
    }
    s_init_result = ESP_OK;
    ESP_LOGI(TAG, "Index v%u ready: %u files, %d AVI, %d JPEG",
             header->version, s_entry_count, videos, images);
    return ESP_OK;
}

static const char *plain_name(const char *selection)
{
    const char *slash = strrchr(selection, '/');
    const char *backslash = strrchr(selection, '\\');
    if (backslash && (!slash || backslash > slash))
        slash = backslash;
    return slash ? slash + 1 : selection;
}

esp_err_t flash_media_resolve(flash_media_type_t type,
                              const char *selection,
                              const flash_media_entry_t **entry)
{
    if (!media_type_valid(type) || !selection || !selection[0] || !entry)
        return ESP_ERR_INVALID_ARG;

    esp_err_t ret = flash_media_init();
    if (ret != ESP_OK)
        return ret;

    char *end = NULL;
    long requested = strtol(selection, &end, 10);
    if (selection != end && *end == '\0')
    {
        if (requested < 1 || requested > INT_MAX)
            return ESP_ERR_INVALID_ARG;
        int index = 0;
        for (uint16_t i = 0; i < s_entry_count; i++)
        {
            if (s_entries[i].type != type)
                continue;
            if (++index == requested)
            {
                *entry = &s_entries[i];
                return ESP_OK;
            }
        }
        return ESP_ERR_NOT_FOUND;
    }

    const char *name = plain_name(selection);
    for (uint16_t i = 0; i < s_entry_count; i++)
    {
        if (s_entries[i].type == type &&
            strcasecmp(s_entries[i].name, name) == 0)
        {
            *entry = &s_entries[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

int flash_media_list(flash_media_type_t type, char *output,
                     size_t output_size)
{
    if (!media_type_valid(type) || !output || output_size == 0)
        return -1;
    output[0] = '\0';
    if (flash_media_init() != ESP_OK)
        return -1;

    int count = 0;
    size_t used = 0;
    for (uint16_t i = 0; i < s_entry_count; i++)
    {
        if (s_entries[i].type != type)
            continue;
        count++;
        if (used + 1 >= output_size)
            continue;
        int written = snprintf(output + used, output_size - used,
                               "%d=%s\n", count, s_entries[i].name);
        if (written < 0)
            continue;
        used += (size_t)written < output_size - used
                    ? (size_t)written
                    : output_size - used - 1;
    }
    return count;
}

const uint8_t *flash_media_data(const flash_media_entry_t *entry)
{
    if (!entry || flash_media_init() != ESP_OK)
        return NULL;
    if (s_legacy_mode && entry == &s_legacy_entry)
        return s_partition_data;
    if (!entry_valid(entry))
        return NULL;
    return s_partition_data + entry->offset;
}

const uint8_t *flash_media_font_data(void)
{
    if (flash_media_init() != ESP_OK)
        return NULL;
    if (s_legacy_mode)
        return NULL; /* 旧镜像无字库区 */

    /* 字库区在索引区后: "GBK16F\0\0" + 766,080 字节点阵 */
    const uint8_t *font_area = s_partition_data + FLASH_MEDIA_INDEX_SIZE;
    if (s_partition->size <
        FLASH_MEDIA_INDEX_SIZE + FLASH_MEDIA_FONT_MAGIC_SIZE)
        return NULL;
    if (memcmp(font_area, FLASH_MEDIA_FONT_MAGIC,
               sizeof(FLASH_MEDIA_FONT_MAGIC) - 1U) != 0)
        return NULL;
    if (s_partition->size <
        FLASH_MEDIA_INDEX_SIZE + FLASH_MEDIA_FONT_MAGIC_SIZE +
            FLASH_MEDIA_FONT_SIZE)
        return NULL;
    return font_area + FLASH_MEDIA_FONT_MAGIC_SIZE;
}

