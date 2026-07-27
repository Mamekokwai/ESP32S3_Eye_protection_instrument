#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    IMAGE_VIEWER_BUSY,
    IMAGE_VIEWER_DONE,
    IMAGE_VIEWER_ERROR,
} image_viewer_state_t;

int image_viewer_list_files(char *output, size_t output_size);
int image_viewer_list_flash_files(char *output, size_t output_size);
esp_err_t image_viewer_start(const char *selection);
esp_err_t image_viewer_start_flash(const char *selection);
image_viewer_state_t image_viewer_tick(void);
void image_viewer_cancel(void);
const char *image_viewer_command(void);
const char *image_viewer_name(void);
uint32_t image_viewer_width(void);
uint32_t image_viewer_height(void);
esp_err_t image_viewer_last_error(void);
