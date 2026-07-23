#include "sd_browser.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "spi_sd.h"
#include "spilcd.h"

#define SD_BROWSER_ROWS_PER_PAGE 12
#define SD_BROWSER_LINE_CHARS    30

static const char *TAG = "sd_browser";

static bool is_dot_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static bool entry_is_directory(const struct dirent *entry)
{
    if (entry->d_type == DT_DIR)
        return true;
    if (entry->d_type != DT_UNKNOWN)
        return false;

    char path[320];
    struct stat info;
    int length = snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, entry->d_name);
    return length > 0 && length < (int)sizeof(path) &&
           stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static void make_display_line(char *output, size_t output_size,
                              const struct dirent *entry, bool is_directory)
{
    const char *prefix = is_directory ? "[D] " : "[F] ";
    size_t position = 0;

    while (*prefix && position + 1 < output_size)
        output[position++] = *prefix++;

    const unsigned char *name = (const unsigned char *)entry->d_name;
    while (*name && position + 1 < output_size && position < SD_BROWSER_LINE_CHARS)
    {
        output[position++] = (*name >= 32 && *name <= 126) ? (char)*name : '?';
        name++;
    }
    output[position] = '\0';
}

static DIR *open_root_with_remount(esp_err_t *result)
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

static void show_error(const char *detail)
{
    spilcd_clear(WHITE);
    spilcd_show_text16(96, 120, "TF CARD ERROR", RED, WHITE);
    spilcd_show_text16(104, 152, detail, BLACK, WHITE);
}

esp_err_t sd_browser_show_page(int requested_page, int *shown_page,
                               int *page_count, int *entry_count)
{
    esp_err_t ret = ESP_FAIL;
    DIR *directory = open_root_with_remount(&ret);
    if (!directory)
    {
        ESP_LOGE(TAG, "TF card unavailable: %s", esp_err_to_name(ret));
        show_error(sd_spi_is_mounted() ? "OPEN FAILED" : "INSERT CARD");
        return ret;
    }

    int total_entries = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (!is_dot_entry(entry->d_name))
            total_entries++;
    }
    closedir(directory);

    int total_pages = (total_entries + SD_BROWSER_ROWS_PER_PAGE - 1) /
                      SD_BROWSER_ROWS_PER_PAGE;
    if (total_pages == 0)
        total_pages = 1;
    int page = requested_page < 1 ? 1 : requested_page;
    if (page > total_pages)
        page = total_pages;

    spilcd_clear(WHITE);
    char text[40];
    snprintf(text, sizeof(text), "TF CARD  PAGE %d/%d", page, total_pages);
    spilcd_show_text16(80, 32, text, BLUE, WHITE);

    directory = open_root_with_remount(&ret);
    if (!directory)
    {
        show_error("OPEN FAILED");
        return ret;
    }

    int skip = (page - 1) * SD_BROWSER_ROWS_PER_PAGE;
    int valid_index = 0;
    int row = 0;
    while ((entry = readdir(directory)) != NULL && row < SD_BROWSER_ROWS_PER_PAGE)
    {
        if (is_dot_entry(entry->d_name))
            continue;
        if (valid_index++ < skip)
            continue;

        char line[SD_BROWSER_LINE_CHARS + 1];
        bool is_directory = entry_is_directory(entry);
        make_display_line(line, sizeof(line), entry, is_directory);
        uint16_t color = is_directory ? GREEN : BLACK;
        spilcd_show_text16(40, 64 + row * 16, line, color, WHITE);
        row++;
    }
    closedir(directory);

    if (total_entries == 0)
        spilcd_show_text16(112, 144, "EMPTY", BLACK, WHITE);

    snprintf(text, sizeof(text), "%d ITEMS   SDLIST N", total_entries);
    spilcd_show_text16(80, 272, text, BLACK, WHITE);

    if (shown_page)
        *shown_page = page;
    if (page_count)
        *page_count = total_pages;
    if (entry_count)
        *entry_count = total_entries;

    ESP_LOGI(TAG, "Displayed TF root page %d/%d (%d entries)",
             page, total_pages, total_entries);
    return ESP_OK;
}
