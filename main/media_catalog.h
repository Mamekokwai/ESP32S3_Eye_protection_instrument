#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef enum
{
    MEDIA_AUDIO,
    MEDIA_IMAGE,
    MEDIA_VIDEO,
} media_kind_t;

/**
 * @brief Resolve a one-based index or a root-level filename.
 *
 * @param kind       Media extension group.
 * @param selection  "1", "demo.avi", or an equivalent root path.
 * @param name       Receives a plain root-level filename.
 */
esp_err_t media_catalog_resolve(media_kind_t kind, const char *selection,
                                char *name, size_t name_size);

/**
 * @brief List matching root-level files as "index=name\n".
 *
 * @return Matching file count, or -1 when the card/root is unavailable.
 */
int media_catalog_list(media_kind_t kind, char *output, size_t output_size);
