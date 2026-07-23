#include "media_catalog.h"

#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "sd_card.h"

#define TAG "media_catalog"

static bool has_suffix(const char *name, const char *suffix)
{
    size_t name_length = strlen(name);
    size_t suffix_length = strlen(suffix);
    return name_length > suffix_length &&
           strcasecmp(name + name_length - suffix_length, suffix) == 0;
}

static bool media_matches(media_kind_t kind, const char *name)
{
    switch (kind)
    {
    case MEDIA_AUDIO:
        return has_suffix(name, ".pcm") || has_suffix(name, ".mp3");
    case MEDIA_IMAGE:
        return has_suffix(name, ".jpg") || has_suffix(name, ".jpeg");
    case MEDIA_VIDEO:
        return has_suffix(name, ".avi");
    default:
        return false;
    }
}

static DIR *open_root(void)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (!sd_card_is_mounted() || attempt > 0)
        {
            if (sd_card_mount() != ESP_OK)
                continue;
        }

        DIR *directory = opendir(SD_CARD_MOUNT_POINT);
        if (directory)
            return directory;
        ESP_LOGW(TAG, "Cannot open root; remounting TF card");
    }
    return NULL;
}

static esp_err_t ensure_mounted(void)
{
    if (sd_card_is_mounted())
        return ESP_OK;
    return sd_card_mount() == ESP_OK ? ESP_OK : ESP_FAIL;
}

static const char *plain_name(const char *selection)
{
    if (strncmp(selection, "/0:/", 4) == 0)
        return selection + 4;
    if (strncmp(selection, "0:/", 3) == 0 ||
        strncmp(selection, "/0:", 3) == 0)
        return selection + 3;
    if (selection[0] == '/')
        return selection + 1;
    return selection;
}

static esp_err_t copy_valid_name(media_kind_t kind, const char *source,
                                 char *name, size_t name_size)
{
    if (!source[0] || strchr(source, '/') || strchr(source, '\\') ||
        strcmp(source, ".") == 0 || strcmp(source, "..") == 0 ||
        !media_matches(kind, source))
        return ESP_ERR_INVALID_ARG;

    int written = snprintf(name, name_size, "%s", source);
    return written > 0 && written < (int)name_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

esp_err_t media_catalog_resolve(media_kind_t kind, const char *selection,
                                char *name, size_t name_size)
{
    if (!selection || !selection[0] || !name || name_size == 0)
        return ESP_ERR_INVALID_ARG;

    char *end = NULL;
    long requested = strtol(selection, &end, 10);
    if (selection != end && *end == '\0')
    {
        if (requested < 1 || requested > INT_MAX)
            return ESP_ERR_INVALID_ARG;

        DIR *directory = open_root();
        if (!directory)
            return ESP_FAIL;

        int index = 0;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL)
        {
            if (!media_matches(kind, entry->d_name))
                continue;
            if (++index != requested)
                continue;

            esp_err_t result = copy_valid_name(
                kind, entry->d_name, name, name_size);
            closedir(directory);
            return result;
        }
        closedir(directory);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = copy_valid_name(
        kind, plain_name(selection), name, name_size);
    if (result != ESP_OK)
        return result;
    return ensure_mounted();
}

int media_catalog_list(media_kind_t kind, char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return -1;
    output[0] = '\0';

    DIR *directory = open_root();
    if (!directory)
        return -1;

    int count = 0;
    size_t used = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (!media_matches(kind, entry->d_name))
            continue;

        count++;
        if (used + 1 >= output_size)
            continue;
        int written = snprintf(output + used, output_size - used,
                               "%d=%s\n", count, entry->d_name);
        if (written < 0)
            continue;
        used += (size_t)written < output_size - used
                    ? (size_t)written
                    : output_size - used - 1;
    }
    closedir(directory);
    return count;
}
