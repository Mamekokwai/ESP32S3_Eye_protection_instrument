#include "image_viewer.h"

#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mjpeg.h"
#include "spi_sd.h"
#include "spilcd.h"

#define IMAGE_MAX_FILE_SIZE   (1024U * 1024U)
#define IMAGE_MAX_WIDTH       320U
#define IMAGE_MAX_HEIGHT      320U
#define IMAGE_READ_CHUNK      (16U * 1024U)
#define IMAGE_STRIP_ROWS      40U
#define IMAGE_FRAME_BYTES     (IMAGE_MAX_WIDTH * IMAGE_MAX_HEIGHT * sizeof(uint16_t))
#define IMAGE_STRIP_BYTES     (IMAGE_MAX_WIDTH * IMAGE_STRIP_ROWS * sizeof(uint16_t))

typedef enum
{
    PHASE_IDLE,
    PHASE_READ,
    PHASE_VALIDATE,
    PHASE_DECODE,
    PHASE_CLEAR,
    PHASE_CLEAR_WAIT,
    PHASE_SUBMIT,
    PHASE_SUBMIT_WAIT,
    PHASE_DONE,
    PHASE_ERROR,
} image_phase_t;

typedef struct
{
    image_phase_t phase;
    FILE *file;
    uint8_t *jpeg;
    uint16_t *frame;
    uint16_t *strip;
    size_t file_size;
    size_t bytes_read;
    uint32_t width;
    uint32_t height;
    uint32_t row;
    uint16_t offset_x;
    uint16_t offset_y;
    char name[256];
    esp_err_t error;
    int64_t started_at;
} image_context_t;

static const char *TAG = "image_viewer";
static image_context_t s_image;

static bool has_jpeg_extension(const char *name)
{
    size_t length = strlen(name);
    return (length > 4 && strcasecmp(name + length - 4, ".jpg") == 0) ||
           (length > 5 && strcasecmp(name + length - 5, ".jpeg") == 0);
}

static bool copy_name(char *output, size_t output_size, const char *name)
{
    size_t length = strlen(name);
    if (output_size == 0 || length >= output_size)
        return false;
    memcpy(output, name, length + 1);
    return true;
}

static void release_resources(void)
{
    if (s_image.file)
        fclose(s_image.file);
    heap_caps_free(s_image.jpeg);
    heap_caps_free(s_image.frame);
    heap_caps_free(s_image.strip);
    s_image.file = NULL;
    s_image.jpeg = NULL;
    s_image.frame = NULL;
    s_image.strip = NULL;
}

static void show_error(const char *detail)
{
    spilcd_clear(WHITE);
    spilcd_show_text16(104, 120, "IMAGE ERROR", RED, WHITE);
    spilcd_show_text16(96, 152, detail, BLACK, WHITE);
}

static image_viewer_state_t fail_loading(esp_err_t error, const char *detail)
{
    s_image.error = error;
    release_resources();
    s_image.phase = PHASE_ERROR;
    show_error(detail);
    ESP_LOGE(TAG, "Cannot display %s: %s", s_image.name,
             esp_err_to_name(error));
    return IMAGE_VIEWER_ERROR;
}

static DIR *open_root(esp_err_t *result)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (!sd_spi_is_mounted() || attempt > 0)
        {
            *result = sd_spi_init();
            if (*result != ESP_OK || !sd_spi_is_mounted())
                continue;
        }

        DIR *directory = opendir(MOUNT_POINT);
        if (directory)
        {
            *result = ESP_OK;
            return directory;
        }
        *result = ESP_FAIL;
        ESP_LOGW(TAG, "Cannot open %s, remounting TF card", MOUNT_POINT);
    }
    return NULL;
}

static bool parse_index(const char *selection, int *index)
{
    char *end = NULL;
    long value = strtol(selection, &end, 10);
    if (selection == end || *end != '\0' || value < 1 || value > INT_MAX)
        return false;
    *index = (int)value;
    return true;
}

static esp_err_t resolve_selection(const char *selection, char *path,
                                   size_t path_size)
{
    if (!selection || !selection[0])
        return ESP_ERR_INVALID_ARG;

    int requested_index = 0;
    if (parse_index(selection, &requested_index))
    {
        esp_err_t ret = ESP_FAIL;
        DIR *directory = open_root(&ret);
        if (!directory)
            return ret;

        int current_index = 0;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL)
        {
            if (!has_jpeg_extension(entry->d_name))
                continue;
            if (++current_index == requested_index)
            {
                bool copied = copy_name(s_image.name, sizeof(s_image.name),
                                        entry->d_name);
                closedir(directory);
                if (!copied)
                    return ESP_ERR_INVALID_SIZE;
                int length = snprintf(path, path_size, "%s/%s",
                                      MOUNT_POINT, s_image.name);
                return length > 0 && length < (int)path_size
                           ? ESP_OK
                           : ESP_ERR_INVALID_SIZE;
            }
        }
        closedir(directory);
        return ESP_ERR_NOT_FOUND;
    }

    const char *name = selection;
    if (strncmp(name, "/0:/", 4) == 0)
        name += 4;
    else if (strncmp(name, "0:/", 3) == 0)
        name += 3;
    else if (strncmp(name, "/0:", 3) == 0)
        name += 3;
    else if (name[0] == '/')
        name++;

    if (!name[0] || strchr(name, '/') || strchr(name, '\\') ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        !has_jpeg_extension(name))
        return ESP_ERR_INVALID_ARG;
    if (!copy_name(s_image.name, sizeof(s_image.name), name))
        return ESP_ERR_INVALID_SIZE;

    int length = snprintf(path, path_size, "%s/%s",
                          MOUNT_POINT, s_image.name);
    return length > 0 && length < (int)path_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t read_jpeg_dimensions(const uint8_t *data, size_t size,
                                      uint32_t *width, uint32_t *height)
{
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return ESP_ERR_INVALID_ARG;

    size_t position = 2;
    while (position + 1 < size)
    {
        while (position < size && data[position] != 0xFF)
            position++;
        while (position < size && data[position] == 0xFF)
            position++;
        if (position >= size)
            break;

        uint8_t marker = data[position++];
        if (marker == 0x00 || marker == 0xD8 ||
            (marker >= 0xD0 && marker <= 0xD7))
            continue;
        if (marker == 0xD9 || marker == 0xDA || position + 1 >= size)
            break;

        size_t segment_length = ((size_t)data[position] << 8) |
                                data[position + 1];
        if (segment_length < 2 || segment_length > size - position)
            return ESP_ERR_INVALID_SIZE;

        bool is_sof = marker >= 0xC0 && marker <= 0xCF &&
                      marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof)
        {
            if (marker != 0xC0)
                return ESP_ERR_NOT_SUPPORTED;
            if (segment_length < 8)
                return ESP_ERR_INVALID_SIZE;
            *height = ((uint32_t)data[position + 3] << 8) |
                      data[position + 4];
            *width = ((uint32_t)data[position + 5] << 8) |
                     data[position + 6];
            if (*width == 0 || *height == 0)
                return ESP_ERR_INVALID_SIZE;
            return ESP_OK;
        }
        position += segment_length;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

int image_viewer_list_files(char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return -1;
    output[0] = '\0';

    esp_err_t ret = ESP_FAIL;
    DIR *directory = open_root(&ret);
    if (!directory)
        return -1;

    int count = 0;
    size_t used = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (!has_jpeg_extension(entry->d_name))
            continue;
        count++;
        if (used + 1 >= output_size)
            continue;

        int written = snprintf(output + used, output_size - used,
                               "%d=%s\n", count, entry->d_name);
        if (written < 0)
            continue;
        if ((size_t)written >= output_size - used)
            used = output_size - 1;
        else
            used += (size_t)written;
    }
    closedir(directory);
    return count;
}

esp_err_t image_viewer_start(const char *selection)
{
    image_viewer_cancel();

    if (!sd_spi_is_mounted() && sd_spi_init() != ESP_OK)
    {
        show_error("INSERT CARD");
        return ESP_FAIL;
    }

    char path[320];
    esp_err_t ret = resolve_selection(selection, path, sizeof(path));
    if (ret != ESP_OK)
    {
        show_error(ret == ESP_ERR_NOT_FOUND ? "FILE NOT FOUND" : "BAD FILE NAME");
        return ret;
    }

    s_image.file = fopen(path, "rb");
    if (!s_image.file)
    {
        show_error("OPEN FAILED");
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(s_image.file, 0, SEEK_END) != 0)
    {
        release_resources();
        show_error("READ FAILED");
        return ESP_FAIL;
    }
    long size = ftell(s_image.file);
    if (size <= 0 || (unsigned long)size > IMAGE_MAX_FILE_SIZE ||
        fseek(s_image.file, 0, SEEK_SET) != 0)
    {
        release_resources();
        show_error("MAX FILE 1MB");
        return ESP_ERR_INVALID_SIZE;
    }

    s_image.file_size = (size_t)size;
    s_image.jpeg = heap_caps_aligned_alloc(
        64, s_image.file_size, MALLOC_CAP_SPIRAM);
    s_image.frame = heap_caps_aligned_alloc(
        64, IMAGE_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_image.strip = heap_caps_aligned_alloc(
        64, IMAGE_STRIP_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_image.jpeg || !s_image.frame || !s_image.strip)
    {
        release_resources();
        show_error("OUT OF MEMORY");
        return ESP_ERR_NO_MEM;
    }

    s_image.started_at = esp_timer_get_time();
    s_image.phase = PHASE_READ;
    ESP_LOGI(TAG, "Loading %s (%lu bytes)", s_image.name,
             (unsigned long)s_image.file_size);
    return ESP_OK;
}

image_viewer_state_t image_viewer_tick(void)
{
    switch (s_image.phase)
    {
    case PHASE_DONE:
        return IMAGE_VIEWER_DONE;
    case PHASE_ERROR:
        return IMAGE_VIEWER_ERROR;

    case PHASE_READ:
    {
        size_t remaining = s_image.file_size - s_image.bytes_read;
        size_t chunk = remaining > IMAGE_READ_CHUNK
                           ? IMAGE_READ_CHUNK
                           : remaining;
        size_t read = fread(s_image.jpeg + s_image.bytes_read, 1,
                            chunk, s_image.file);
        s_image.bytes_read += read;
        if (read != chunk)
            return fail_loading(ESP_FAIL, "READ FAILED");
        if (s_image.bytes_read == s_image.file_size)
        {
            fclose(s_image.file);
            s_image.file = NULL;
            s_image.phase = PHASE_VALIDATE;
        }
        break;
    }

    case PHASE_VALIDATE:
        s_image.error = read_jpeg_dimensions(
            s_image.jpeg, s_image.file_size,
            &s_image.width, &s_image.height);
        if (s_image.error == ESP_ERR_NOT_SUPPORTED)
            return fail_loading(s_image.error, "BASELINE ONLY");
        if (s_image.error != ESP_OK)
            return fail_loading(s_image.error, "INVALID JPEG");
        if (s_image.width > IMAGE_MAX_WIDTH ||
            s_image.height > IMAGE_MAX_HEIGHT)
            return fail_loading(ESP_ERR_INVALID_SIZE, "MAX 320X320");
        s_image.phase = PHASE_DECODE;
        break;

    case PHASE_DECODE:
    {
        s_image.error = mjpeg_decoder_init(
            IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT);
        if (s_image.error != ESP_OK)
            return fail_loading(s_image.error, "DECODER ERROR");

        uint32_t decoded_width = 0;
        uint32_t decoded_height = 0;
        s_image.error = mjpeg_decoder_decode(
            s_image.jpeg, s_image.file_size, s_image.frame,
            IMAGE_FRAME_BYTES, &decoded_width, &decoded_height);
        mjpeg_decoder_deinit();
        if (s_image.error != ESP_OK)
            return fail_loading(s_image.error, "DECODE FAILED");
        if (decoded_width != s_image.width ||
            decoded_height != s_image.height)
            return fail_loading(ESP_ERR_INVALID_RESPONSE, "SIZE MISMATCH");

        heap_caps_free(s_image.jpeg);
        s_image.jpeg = NULL;
        s_image.offset_x = (IMAGE_MAX_WIDTH - s_image.width) / 2;
        s_image.offset_y = (IMAGE_MAX_HEIGHT - s_image.height) / 2;
        memset(s_image.strip, 0, IMAGE_STRIP_BYTES);
        s_image.row = 0;
        s_image.phase = PHASE_CLEAR;
        break;
    }

    case PHASE_CLEAR:
    {
        if (s_image.row >= IMAGE_MAX_HEIGHT)
        {
            s_image.row = 0;
            s_image.phase = PHASE_SUBMIT;
            break;
        }
        uint32_t rows = IMAGE_MAX_HEIGHT - s_image.row;
        if (rows > IMAGE_STRIP_ROWS)
            rows = IMAGE_STRIP_ROWS;

        refresh_done_flag = 0;
        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            panel_handle, 0, s_image.row, IMAGE_MAX_WIDTH,
            s_image.row + rows, s_image.strip);
        if (ret != ESP_OK)
            return fail_loading(ret, "LCD ERROR");
        s_image.row += rows;
        s_image.phase = PHASE_CLEAR_WAIT;
        break;
    }

    case PHASE_CLEAR_WAIT:
        if (refresh_done_flag)
            s_image.phase = PHASE_CLEAR;
        break;

    case PHASE_SUBMIT:
    {
        if (s_image.row >= s_image.height)
        {
            release_resources();
            s_image.phase = PHASE_DONE;
            ESP_LOGI(TAG, "Displayed %s (%lux%lu) in %lld ms",
                     s_image.name, (unsigned long)s_image.width,
                     (unsigned long)s_image.height,
                     (esp_timer_get_time() - s_image.started_at) / 1000);
            return IMAGE_VIEWER_DONE;
        }

        uint32_t rows = s_image.height - s_image.row;
        if (rows > IMAGE_STRIP_ROWS)
            rows = IMAGE_STRIP_ROWS;
        memcpy(s_image.strip,
               s_image.frame + s_image.row * s_image.width,
               (size_t)rows * s_image.width * sizeof(uint16_t));

        refresh_done_flag = 0;
        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            panel_handle, s_image.offset_x,
            s_image.offset_y + s_image.row,
            s_image.offset_x + s_image.width,
            s_image.offset_y + s_image.row + rows,
            s_image.strip);
        if (ret != ESP_OK)
            return fail_loading(ret, "LCD ERROR");
        s_image.row += rows;
        s_image.phase = PHASE_SUBMIT_WAIT;
        break;
    }

    case PHASE_SUBMIT_WAIT:
        if (refresh_done_flag)
            s_image.phase = PHASE_SUBMIT;
        break;

    case PHASE_IDLE:
    default:
        break;
    }
    return IMAGE_VIEWER_BUSY;
}

void image_viewer_cancel(void)
{
    if ((s_image.phase == PHASE_CLEAR_WAIT ||
         s_image.phase == PHASE_SUBMIT_WAIT) &&
        !refresh_done_flag)
    {
        while (!refresh_done_flag)
            vTaskDelay(pdMS_TO_TICKS(1));
    }
    release_resources();
    memset(&s_image, 0, sizeof(s_image));
    s_image.phase = PHASE_IDLE;
}

const char *image_viewer_name(void)
{
    return s_image.name;
}

uint32_t image_viewer_width(void)
{
    return s_image.width;
}

uint32_t image_viewer_height(void)
{
    return s_image.height;
}

esp_err_t image_viewer_last_error(void)
{
    return s_image.error;
}
