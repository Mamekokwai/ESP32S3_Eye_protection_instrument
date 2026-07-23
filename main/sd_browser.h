#pragma once

#include "esp_err.h"

esp_err_t sd_browser_show_page(int requested_page, int *shown_page,
                               int *page_count, int *entry_count);
