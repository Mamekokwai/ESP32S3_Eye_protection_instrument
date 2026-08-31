#include "production_unlock.h"

#include "sdkconfig.h"

#if CONFIG_EYECARE_PRODUCTION_LOCK

#if !CONFIG_SECURE_BOOT_V2_ENABLED || !CONFIG_SECURE_FLASH_ENC_ENABLED || \
    !CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
#error "Production unlock requires Secure Boot V2 and Release Flash Encryption"
#endif

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_efuse.h"
#include "esp_efuse_custom_table.h"
#include "esp_log.h"
#include "esp_flash_encrypt.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "esp_secure_boot.h"
#include "sd_card.h"
#include "unlock_public_key.h"

#define TAG "prod_unlock"
#define UNLOCK_TOKEN_PATH SD_CARD_MOUNT_POINT "/eyecare.unlock"
#define UNLOCK_MAGIC "EYEUNLK1"
#define UNLOCK_MAGIC_SIZE 8U
#define UNLOCK_VERSION 1U
#define UNLOCK_KEY_ID 1U
#define UNLOCK_NONCE_SIZE 16U
#define UNLOCK_PAYLOAD_SIZE                                                   \
    (UNLOCK_MAGIC_SIZE + 1U + 1U + 2U + UNLOCK_NONCE_SIZE)
#define UNLOCK_SIGNATURE_MAX 72U
#define UNLOCK_TOKEN_MAX (UNLOCK_PAYLOAD_SIZE + 1U + UNLOCK_SIGNATURE_MAX)

static esp_err_t read_token(uint8_t *token, size_t *token_size)
{
    FILE *file = fopen(UNLOCK_TOKEN_PATH, "rb");
    if (!file)
        return ESP_ERR_NOT_FOUND;

    size_t count = fread(token, 1, UNLOCK_TOKEN_MAX, file);
    int extra = fgetc(file);
    bool read_error = ferror(file) != 0;
    fclose(file);

    if (read_error)
        return ESP_FAIL;
    if (extra != EOF || count <= UNLOCK_PAYLOAD_SIZE + 1U)
        return ESP_ERR_INVALID_SIZE;
    *token_size = count;
    return ESP_OK;
}

static esp_err_t verify_token(const uint8_t *token, size_t token_size)
{
    if (EYECARE_UNLOCK_PUBLIC_KEY_DER_LEN == 0)
        return ESP_ERR_INVALID_STATE;
    if (memcmp(token, UNLOCK_MAGIC, UNLOCK_MAGIC_SIZE) != 0 ||
        token[8] != UNLOCK_VERSION || token[9] != UNLOCK_KEY_ID ||
        token[10] != 0 || token[11] != 0)
        return ESP_ERR_INVALID_VERSION;

    uint8_t signature_size = token[UNLOCK_PAYLOAD_SIZE];
    if (signature_size == 0 || signature_size > UNLOCK_SIGNATURE_MAX ||
        token_size != UNLOCK_PAYLOAD_SIZE + 1U + signature_size)
        return ESP_ERR_INVALID_SIZE;

    uint8_t digest[32];
    if (mbedtls_sha256(token, UNLOCK_PAYLOAD_SIZE, digest, 0) != 0)
        return ESP_FAIL;

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int result = mbedtls_pk_parse_public_key(
        &key, EYECARE_UNLOCK_PUBLIC_KEY_DER,
        EYECARE_UNLOCK_PUBLIC_KEY_DER_LEN);
    if (result == 0)
    {
        result = mbedtls_pk_verify(
            &key, MBEDTLS_MD_SHA256, digest, sizeof(digest),
            token + UNLOCK_PAYLOAD_SIZE + 1U, signature_size);
    }
    mbedtls_pk_free(&key);
    return result == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t try_unlock(void)
{
    esp_err_t err = sd_card_mount();
    if (err != ESP_OK)
        return err;

    uint8_t token[UNLOCK_TOKEN_MAX];
    size_t token_size = 0;
    err = read_token(token, &token_size);
    if (err != ESP_OK)
        return err;
    return verify_token(token, token_size);
}

bool production_unlock_ensure(void)
{
    if (!esp_secure_boot_enabled() || !esp_flash_encryption_enabled())
    {
        ESP_LOGE(TAG, "Security eFuses are not active; refusing unlock");
        return false;
    }

    if (esp_efuse_read_field_bit(ESP_EFUSE_USER_DATA_EYECARE_UNLOCKED))
    {
        ESP_LOGI(TAG, "Permanent unlock marker present");
        return true;
    }

    ESP_LOGW(TAG, "Device locked; waiting for %s", UNLOCK_TOKEN_PATH);
    esp_err_t previous = ESP_OK;
    while (true)
    {
        esp_err_t err = try_unlock();
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Token verified; burning permanent unlock eFuse");
            err = esp_efuse_write_field_bit(
                ESP_EFUSE_USER_DATA_EYECARE_UNLOCKED);
            if (err == ESP_OK &&
                esp_efuse_read_field_bit(
                    ESP_EFUSE_USER_DATA_EYECARE_UNLOCKED))
            {
                ESP_LOGI(TAG, "Permanent unlock complete");
                return true;
            }
            ESP_LOGE(TAG, "eFuse write failed: %s", esp_err_to_name(err));
        }
        else if (err != previous)
        {
            ESP_LOGW(TAG, "Unlock pending: %s", esp_err_to_name(err));
            previous = err;
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_EYECARE_UNLOCK_RETRY_MS));
    }
}

#else

bool production_unlock_ensure(void)
{
    return true;
}

#endif
