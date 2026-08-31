#include "media_catalog.h"

#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "sd_card.h"

#define TAG "media_catalog"

typedef struct directory_node
{
    struct directory_node *next;
    char path[];
} directory_node_t;

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

static bool is_dot_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
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

static bool invalid_component(const char *component, size_t length)
{
    return (length == 1 && component[0] == '.') ||
           (length == 2 && component[0] == '.' && component[1] == '.');
}

static esp_err_t normalize_path(media_kind_t kind, const char *selection,
                                char *path, size_t path_size)
{
    const char *source = selection;
    if (strncmp(source, "/0:/", 4) == 0)
        source += 4;
    else if (strncmp(source, "0:/", 3) == 0)
        source += 3;
    else if (strncmp(source, "/0:", 3) == 0)
        source += 3;
    else if (strncmp(source, "0:", 2) == 0)
        source += 2;
    else if (*source == '/' || *source == '\\')
        source++;

    if (*source == '/' || *source == '\\')
        source++;
    if (!source[0])
        return ESP_ERR_INVALID_ARG;

    size_t used = 0;
    size_t component_start = 0;
    for (; *source; source++)
    {
        unsigned char value = (unsigned char)*source;
        if (*source == '/' || *source == '\\')
        {
            size_t component_length = used - component_start;
            if (component_length == 0 ||
                invalid_component(path + component_start, component_length))
                return ESP_ERR_INVALID_ARG;
            if (used + 1 >= path_size)
                return ESP_ERR_INVALID_SIZE;
            path[used++] = '/';
            component_start = used;
            continue;
        }
        if (value < 32 || *source == ':')
            return ESP_ERR_INVALID_ARG;
        if (used + 1 >= path_size)
            return ESP_ERR_INVALID_SIZE;
        path[used++] = *source;
    }

    size_t component_length = used - component_start;
    if (component_length == 0 ||
        invalid_component(path + component_start, component_length))
        return ESP_ERR_INVALID_ARG;
    path[used] = '\0';
    return media_matches(kind, path) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t enqueue_directory(directory_node_t **head,
                                   directory_node_t **tail,
                                   const char *path)
{
    size_t path_length = strlen(path);
    directory_node_t *node = malloc(sizeof(*node) + path_length + 1);
    if (!node)
        return ESP_ERR_NO_MEM;

    node->next = NULL;
    memcpy(node->path, path, path_length + 1);
    if (*tail)
        (*tail)->next = node;
    else
        *head = node;
    *tail = node;
    return ESP_OK;
}

static void free_directories(directory_node_t *head)
{
    while (head)
    {
        directory_node_t *next = head->next;
        free(head);
        head = next;
    }
}

static bool entry_is_directory(const struct dirent *entry,
                               const char *full_path)
{
    if (entry->d_type == DT_DIR)
        return true;
    if (entry->d_type != DT_UNKNOWN)
        return false;

    struct stat info;
    return stat(full_path, &info) == 0 && S_ISDIR(info.st_mode);
}

static esp_err_t join_path(const char *parent, const char *name,
                           char *path, size_t path_size)
{
    int written = parent[0]
                      ? snprintf(path, path_size, "%s/%s", parent, name)
                      : snprintf(path, path_size, "%s", name);
    return written >= 0 && written < (int)path_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

static esp_err_t make_full_path(const char *relative_path,
                                char *full_path, size_t full_path_size)
{
    int written = relative_path[0]
                      ? snprintf(full_path, full_path_size, "%s/%s",
                                 SD_CARD_MOUNT_POINT, relative_path)
                      : snprintf(full_path, full_path_size, "%s",
                                 SD_CARD_MOUNT_POINT);
    return written >= 0 && written < (int)full_path_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

typedef struct
{
    media_kind_t kind;
    int requested_index;
    int count;
    char *resolved_path;
    size_t resolved_path_size;
    char *list_output;
    size_t list_output_size;
    size_t list_used;
} catalog_walk_t;

typedef struct
{
    char current_directory[MEDIA_CATALOG_PATH_MAX];
    char relative_path[MEDIA_CATALOG_PATH_MAX];
    char full_path[MEDIA_CATALOG_PATH_MAX + sizeof(SD_CARD_MOUNT_POINT) + 1];
} catalog_buffers_t;

static esp_err_t visit_media(catalog_walk_t *walk, const char *relative_path,
                             bool *finished)
{
    walk->count++;
    if (walk->list_output && walk->list_used + 1 < walk->list_output_size)
    {
        size_t available = walk->list_output_size - walk->list_used;
        int written = snprintf(walk->list_output + walk->list_used, available,
                               "%d=%s\n", walk->count, relative_path);
        if (written > 0)
        {
            walk->list_used += (size_t)written < available
                                   ? (size_t)written
                                   : available - 1;
        }
    }

    if (walk->requested_index != walk->count)
        return ESP_OK;

    int written = snprintf(walk->resolved_path, walk->resolved_path_size,
                           "%s", relative_path);
    if (written < 0 || written >= (int)walk->resolved_path_size)
        return ESP_ERR_INVALID_SIZE;
    *finished = true;
    return ESP_OK;
}

static esp_err_t walk_catalog(catalog_walk_t *walk)
{
    DIR *directory = open_root();
    if (!directory)
        return ESP_FAIL;

    catalog_buffers_t *buffers = calloc(1, sizeof(*buffers));
    if (!buffers)
    {
        closedir(directory);
        return ESP_ERR_NO_MEM;
    }

    directory_node_t *head = NULL;
    directory_node_t *tail = NULL;
    esp_err_t result = ESP_OK;
    bool finished = false;

    while (directory)
    {
        struct dirent *entry;
        while (!finished && (entry = readdir(directory)) != NULL)
        {
            if (is_dot_entry(entry->d_name))
                continue;

            if (join_path(buffers->current_directory, entry->d_name,
                          buffers->relative_path,
                          sizeof(buffers->relative_path)) != ESP_OK)
            {
                ESP_LOGW(TAG, "Skipping overlong path in %s",
                         buffers->current_directory[0]
                             ? buffers->current_directory
                             : "/");
                continue;
            }

            if (make_full_path(buffers->relative_path, buffers->full_path,
                               sizeof(buffers->full_path)) != ESP_OK)
                continue;

            if (entry_is_directory(entry, buffers->full_path))
            {
                result = enqueue_directory(&head, &tail,
                                           buffers->relative_path);
                if (result != ESP_OK)
                    break;
                continue;
            }
            if (!media_matches(walk->kind, entry->d_name))
                continue;

            result = visit_media(walk, buffers->relative_path, &finished);
            if (result != ESP_OK)
                break;
        }
        closedir(directory);
        directory = NULL;

        if (result != ESP_OK || finished)
            break;

        while (head && !directory)
        {
            directory_node_t *node = head;
            head = node->next;
            if (!head)
                tail = NULL;
            snprintf(buffers->current_directory,
                     sizeof(buffers->current_directory), "%s", node->path);
            free(node);

            if (make_full_path(buffers->current_directory, buffers->full_path,
                               sizeof(buffers->full_path)) != ESP_OK)
                continue;
            directory = opendir(buffers->full_path);
            if (!directory)
                ESP_LOGW(TAG, "Cannot open directory: %s",
                         buffers->full_path);
        }
    }

    free_directories(head);
    free(buffers);
    if (result != ESP_OK)
        return result;
    if (walk->requested_index > 0 && !finished)
        return ESP_ERR_NOT_FOUND;
    return ESP_OK;
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

        catalog_walk_t walk = {
            .kind = kind,
            .requested_index = (int)requested,
            .resolved_path = name,
            .resolved_path_size = name_size,
        };
        return walk_catalog(&walk);
    }

    esp_err_t result = normalize_path(kind, selection, name, name_size);
    if (result != ESP_OK)
        return result;
    return ensure_mounted();
}

int media_catalog_list(media_kind_t kind, char *output, size_t output_size)
{
    if (!output || output_size == 0)
        return -1;
    output[0] = '\0';

    catalog_walk_t walk = {
        .kind = kind,
        .list_output = output,
        .list_output_size = output_size,
    };
    return walk_catalog(&walk) == ESP_OK ? walk.count : -1;
}
