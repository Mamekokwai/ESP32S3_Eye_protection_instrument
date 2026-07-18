#include "audio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
/* YT06: XL9555 removed */
#include "myiic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "audio"

// ---- I2S 引脚 (同参考项目) ----
#define I2S_MCLK    GPIO_NUM_45
#define I2S_BCLK    GPIO_NUM_39
#define I2S_WS      GPIO_NUM_41
#define I2S_DOUT    GPIO_NUM_42
#define I2S_DIN     GPIO_NUM_40

// ---- I2C1 用于 ES8311 ----
#define I2C_ES8311_PORT   I2C_NUM_1
#define I2C_ES8311_SDA    GPIO_NUM_4
#define I2C_ES8311_SCL    GPIO_NUM_5
#define ES8311_ADDR        0x30      // (0x18 << 1)

#define SAMPLE_RATE        16000
#define MUTE_PIN           0x8000    // XL9555 P1.7

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static esp_codec_dev_handle_t codec_dev = NULL;
static const audio_codec_data_if_t *data_if = NULL;
static const audio_codec_ctrl_if_t *ctrl_if = NULL;
static const audio_codec_gpio_if_t *gpio_if = NULL;
static const audio_codec_if_t *codec_if = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

// ---- 内部函数 ----

static esp_err_t init_i2c_bus(void)
{
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_ES8311_PORT,
        .scl_io_num = I2C_ES8311_SCL,
        .sda_io_num = I2C_ES8311_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "I2C bus initialized");
    return ESP_OK;
}

static esp_err_t init_i2s_channels(void)
{
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel create failed: %d", ret);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            }
        }
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX init failed: %d", ret);
        return ret;
    }

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RX init failed: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2S channels initialized");
    return ESP_OK;
}

static esp_err_t init_es8311_codec(void)
{
    // XL9555 初始化（通过 I2C0）并开启扬声器
    /* xl9555_init removed */
    /* mute via CA51F352P4 */
    ESP_LOGI(TAG, "MUTE pin set to 1 (speaker enabled)");

    vTaskDelay(pdMS_TO_TICKS(100));

    // I2C 总线（I2C1 → ES8311）
    ESP_ERROR_CHECK(init_i2c_bus());
    vTaskDelay(pdMS_TO_TICKS(50));

    // I2S 通道
    ESP_ERROR_CHECK(init_i2s_channels());

    // I2S 数据接口
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
    };
    data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }

    // I2C 控制接口
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_ES8311_PORT,
        .addr = ES8311_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        return ESP_FAIL;
    }

    // GPIO 接口
    gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGE(TAG, "Failed to create GPIO interface");
        return ESP_FAIL;
    }

    // ES8311 编解码器配置
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = GPIO_NUM_NC;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.pa_reverted = false;

    codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    // 编解码器设备
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    codec_dev = esp_codec_dev_new(&dev_cfg);
    if (codec_dev == NULL) {
        ESP_LOGE(TAG, "Failed to create codec device");
        return ESP_FAIL;
    }

    // 打开设备
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = 0,
    };

    esp_err_t ret = esp_codec_dev_open(codec_dev, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open codec device: %d", ret);
        return ret;
    }

    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(codec_dev, 70));
    ESP_LOGI(TAG, "ES8311 codec initialized");
    return ESP_OK;
}

// ---- 公开接口 ----

esp_err_t audio_init(void)
{
    return init_es8311_codec();
}

esp_err_t audio_play(const uint8_t *data, size_t len)
{
    if (codec_dev == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int total_written = 0;
    const int chunk_size = 512;

    while (total_written < (int)len) {
        int remaining = (int)len - total_written;
        int to_write = (remaining > chunk_size) ? chunk_size : remaining;

        int ret = esp_codec_dev_write(codec_dev, (void *)(data + total_written), to_write);
        if (ret == ESP_CODEC_DEV_OK) {
            total_written += to_write;
        } else {
            ESP_LOGW(TAG, "Write retry, ret=%d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGI(TAG, "Playback done (%d bytes)", total_written);
    return ESP_OK;
}

void audio_write_pcm(const int16_t *samples, size_t num_samples, int sample_rate, int channels)
{
    if (codec_dev == NULL || samples == NULL || num_samples == 0) {
        return;
    }

    // 立体声转单声道简单处理：取左声道
    // 单声道直接写入
    size_t bytes = num_samples * sizeof(int16_t);
    if (channels >= 2) {
        // 下混到单声道：(L+R)/2
        static int16_t mono_buf[1152 * 2];  // max samples per MP3 frame
        size_t mono_count = num_samples / 2;
        if (mono_count > sizeof(mono_buf) / sizeof(mono_buf[0])) {
            mono_count = sizeof(mono_buf) / sizeof(mono_buf[0]);
        }
        for (size_t i = 0; i < mono_count; i++) {
            int32_t mix = (int32_t)samples[i * 2] + samples[i * 2 + 1];
            mono_buf[i] = (int16_t)(mix / 2);
        }
        bytes = mono_count * sizeof(int16_t);
        esp_codec_dev_write(codec_dev, mono_buf, (int)bytes);
    } else {
        esp_codec_dev_write(codec_dev, (void *)samples, (int)bytes);
    }
}

esp_err_t audio_set_volume(int vol)
{
    if (codec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;

    return esp_codec_dev_set_out_vol(codec_dev, vol);
}
