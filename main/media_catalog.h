#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef enum
{
    MEDIA_AUDIO,
    MEDIA_IMAGE,
    MEDIA_VIDEO,
} media_kind_t;

/** Maximum UTF-8 byte length of a catalog path, including the terminator. */
#define MEDIA_CATALOG_PATH_MAX 512

/**
 * @brief Resolve a one-based index or an SD-card-relative media path.
 *
 * @param kind       Media extension group.
 * @param selection  "1", "demo.avi", "/video/demo.avi", or a deeper path.
 * @param name       Receives a normalized path relative to the SD-card root.
 */
esp_err_t media_catalog_resolve(media_kind_t kind, const char *selection,
                                char *name, size_t name_size);

/**
 * @brief Recursively list matching files as "index=relative/path\n".
 *
 * @return Matching file count, or -1 when the card/root is unavailable.
 */
int media_catalog_list(media_kind_t kind, char *output, size_t output_size);
