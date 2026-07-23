#include "image_viewer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_catalog.h"
#include "mjpeg.h"
#include "sd_card.h"
#include "spilcd.h"

#define IMAGE_MAX_FILE_SIZE   (1024U * 1024U)
#define IMAGE_MAX_WIDTH       320U
#define IMAGE_MAX_HEIGHT      320U
#define IMAGE_READ_CHUNK      (32U * 1024U)
#define IMAGE_STRIP_ROWS      80U
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
    return media_catalog_list(MEDIA_IMAGE, output, output_size);
}

esp_err_t image_viewer_start(const char *selection)
{
    image_viewer_cancel();

    char path[320];
    esp_err_t ret = media_catalog_resolve(
        MEDIA_IMAGE, selection, s_image.name, sizeof(s_image.name));
    if (ret != ESP_OK)
    {
        const char *detail = ret == ESP_ERR_NOT_FOUND
                                 ? "FILE NOT FOUND"
                             : ret == ESP_FAIL
                                 ? "INSERT CARD"
                                 : "BAD FILE NAME";
        show_error(detail);
        return ret;
    }
    int path_length = snprintf(path, sizeof(path), "%s/%s",
                               SD_CARD_MOUNT_POINT, s_image.name);
    if (path_length <= 0 || path_length >= (int)sizeof(path))
        return ESP_ERR_INVALID_SIZE;

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
        /* Full-screen images already cover every pixel; avoid a redundant
         * clear pass that makes the old frame visibly disappear first. */
        s_image.phase = (s_image.width == IMAGE_MAX_WIDTH &&
                         s_image.height == IMAGE_MAX_HEIGHT)
                            ? PHASE_SUBMIT
                            : PHASE_CLEAR;
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
