#include "app_settings.h"

#include <stdbool.h>

#include "nvs.h"
#include "nvs_flash.h"

#define SETTINGS_NAMESPACE "eyecare"
#define SETTINGS_KEY_VOLUME "volume"
#define SETTINGS_KEY_BACKLIGHT "backlight"
#define DEFAULT_VOLUME 70
#define DEFAULT_BACKLIGHT 100

static bool valid_volume(uint8_t value)
{
    return value >= APP_VOLUME_MIN && value <= APP_VOLUME_MAX;
}

static bool valid_backlight(uint8_t value)
{
    return value >= APP_BACKLIGHT_MIN && value <= APP_BACKLIGHT_MAX;
}

esp_err_t app_settings_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ret = nvs_flash_erase();
        if (ret == ESP_OK)
            ret = nvs_flash_init();
    }
    return ret;
}

void app_settings_load(app_settings_t *settings)
{
    if (!settings)
        return;

    settings->volume = DEFAULT_VOLUME;
    settings->backlight = DEFAULT_BACKLIGHT;

    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;

    uint8_t value;
    if (nvs_get_u8(handle, SETTINGS_KEY_VOLUME, &value) == ESP_OK &&
        valid_volume(value))
        settings->volume = value;
    if (nvs_get_u8(handle, SETTINGS_KEY_BACKLIGHT, &value) == ESP_OK &&
        valid_backlight(value))
        settings->backlight = value;
    nvs_close(handle);
}

static esp_err_t save_percent(const char *key, uint8_t value, uint8_t min_value)
{
    if (value < min_value || value > 100)
        return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
        return ret;

    ret = nvs_set_u8(handle, key, value);
    if (ret == ESP_OK)
        ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

esp_err_t app_settings_save_volume(uint8_t volume)
{
    return save_percent(SETTINGS_KEY_VOLUME, volume, APP_VOLUME_MIN);
}

esp_err_t app_settings_save_backlight(uint8_t backlight)
{
    return save_percent(SETTINGS_KEY_BACKLIGHT, backlight, APP_BACKLIGHT_MIN);
}
