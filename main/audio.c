#include "audio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
/* YT06: XL9555 removed */
#include "myiic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "audio"

// ---- I2S 引脚 (同参考项目) ----
#define I2S_MCLK GPIO_NUM_45
#define I2S_BCLK GPIO_NUM_39
#define I2S_WS GPIO_NUM_41
#define I2S_DOUT GPIO_NUM_42
#define I2S_DIN GPIO_NUM_40

// ---- I2C1 用于 ES8311 ----
#define I2C_ES8311_PORT I2C_NUM_1
#define I2C_ES8311_SDA GPIO_NUM_4
#define I2C_ES8311_SCL GPIO_NUM_5
#define ES8311_ADDR_7BIT 0x18
#define ES8311_ADDR (ES8311_ADDR_7BIT << 1)

#define AUDIO_OUTPUT_SAMPLE_RATE 44100
#define RESAMPLE_BUFFER_SAMPLES 512
#define AUDIO_AMP_MUTE_GPIO GPIO_NUM_2

#if AUDIO_VOLUME_MODE != AUDIO_VOLUME_MODE_SOFTWARE && \
    AUDIO_VOLUME_MODE != AUDIO_VOLUME_MODE_ES8311
#error "Invalid AUDIO_VOLUME_MODE"
#endif

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static esp_codec_dev_handle_t codec_dev = NULL;
static const audio_codec_data_if_t *data_if = NULL;
static const audio_codec_ctrl_if_t *ctrl_if = NULL;
static const audio_codec_gpio_if_t *gpio_if = NULL;
static const audio_codec_if_t *codec_if = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static int current_sample_rate = AUDIO_OUTPUT_SAMPLE_RATE;
static int current_volume = 70;
/* codec_dev 对象创建成功不代表 ES8311 已经应答并打开。 */
static bool s_audio_ready = false;
static bool s_amp_ready = false;
static bool s_amp_enabled = false;

// ---- 内部函数 ----

/*
 * esp_codec_dev 默认音量曲线：
 *   0  -> -96 dB
 *   1  -> -49.5 dB
 *   100 -> 0 dB
 *
 * 直接调用 codec_if->set_vol()，确保 ES8311 I2C 写入错误能够传回，
 * 避免 esp_codec_dev_set_out_vol() 丢弃底层 set_vol 返回值。
 */
static float volume_percent_to_db(int volume)
{
    if (volume == 0)
        return -96.0f;
    return -50.0f + volume * 0.5f;
}

static esp_err_t set_es8311_hardware_volume(int volume)
{
    if (codec_if == NULL || codec_if->set_vol == NULL)
        return ESP_ERR_NOT_SUPPORTED;

    int ret = codec_if->set_vol(codec_if, volume_percent_to_db(volume));
    if (ret != ESP_CODEC_DEV_OK)
    {
        ESP_LOGE(TAG, "ES8311 volume I2C write failed: volume=%d ret=%d",
                 volume, ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t init_audio_amp(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(AUDIO_AMP_MUTE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK)
        return ret;

    ret = gpio_set_level(AUDIO_AMP_MUTE_GPIO, 0);
    if (ret != ESP_OK)
        return ret;
    s_amp_ready = true;
    s_amp_enabled = false;
    ESP_LOGI(TAG, "Amplifier MUTE: GPIO2=0 (speaker off)");
    return ESP_OK;
}

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
    if (ret != ESP_OK)
    {
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
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S channel create failed: %d", ret);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_OUTPUT_SAMPLE_RATE,
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
        .gpio_cfg = {.mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_DIN, .invert_flags = {
                                                                                                             .mclk_inv = false,
                                                                                                             .bclk_inv = false,
                                                                                                             .ws_inv = false,
                                                                                                         }}};

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "TX init failed: %d", ret);
        return ret;
    }

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "RX init failed: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "I2S channels initialized");
    return ESP_OK;
}

static esp_err_t init_es8311_codec(void)
{
    /* 等待功放和 codec 电源稳定；GPIO2 此时保持低电平。 */
    vTaskDelay(pdMS_TO_TICKS(100));

    // I2C 总线（I2C1 → ES8311）
    esp_err_t ret = init_i2c_bus();
    if (ret != ESP_OK)
        return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    uint32_t probe_attempt = 0;
    while (1)
    {
        probe_attempt++;
        ret = i2c_master_probe(i2c_bus_handle, ES8311_ADDR_7BIT, 100);
        if (ret == ESP_OK)
            break;

        ESP_LOGW(TAG,
                 "ES8311 probe #%lu at 0x%02X failed: %s; retry in 1 s",
                 (unsigned long)probe_attempt, ES8311_ADDR_7BIT,
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "ES8311 detected at I2C address 0x%02X after %lu probe(s)",
             ES8311_ADDR_7BIT, (unsigned long)probe_attempt);

    // I2S 通道
    ret = init_i2s_channels();
    if (ret != ESP_OK)
        return ret;

    // I2S 数据接口
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
    };
    data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL)
    {
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
    if (ctrl_if == NULL)
    {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        return ESP_FAIL;
    }

    // GPIO 接口
    gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL)
    {
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
    if (codec_if == NULL)
    {
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
    if (codec_dev == NULL)
    {
        ESP_LOGE(TAG, "Failed to create codec device");
        return ESP_FAIL;
    }

    // 打开设备
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = AUDIO_OUTPUT_SAMPLE_RATE,
        .mclk_multiple = 0,
    };

    ret = esp_codec_dev_open(codec_dev, &fs);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open codec device: %d", ret);
        return ret;
    }

    /*
     * 硬件模式直接设置默认音量；软件模式把 ES8311 固定为满幅，
     * 后续仅缩放 PCM，保留原软件音量行为。
     */
#if AUDIO_VOLUME_MODE == AUDIO_VOLUME_MODE_ES8311
    ret = set_es8311_hardware_volume(current_volume);
#else
    ret = set_es8311_hardware_volume(100);
#endif
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set initial volume: %d", ret);
        return ret;
    }
    current_sample_rate = AUDIO_OUTPUT_SAMPLE_RATE;
    ESP_LOGI(TAG, "ES8311 codec ready: %d Hz, 16-bit mono, volume=%d mode=%s",
             current_sample_rate, current_volume,
#if AUDIO_VOLUME_MODE == AUDIO_VOLUME_MODE_ES8311
             "ES8311-I2C"
#else
             "PCM-software"
#endif
    );
    return ESP_OK;
}

// ---- 公开接口 ----

esp_err_t audio_init(void)
{
    s_audio_ready = false;

    esp_err_t ret = init_audio_amp();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Amplifier GPIO2 init failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = init_es8311_codec();
    if (ret == ESP_OK)
        s_audio_ready = true;
    else
        ESP_LOGE(TAG, "Audio unavailable, continuing without audio: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t audio_amp_set_enabled(bool enabled)
{
    if (!s_amp_ready)
        return ESP_ERR_INVALID_STATE;

    esp_err_t ret = gpio_set_level(
        AUDIO_AMP_MUTE_GPIO, enabled ? 1 : 0);
    if (ret != ESP_OK)
        return ret;

    if (s_amp_enabled != enabled)
    {
        ESP_LOGI(TAG, "Amplifier MUTE: GPIO2=%d (speaker %s)",
                 enabled ? 1 : 0, enabled ? "on" : "off");
    }
    s_amp_enabled = enabled;
    return ESP_OK;
}

bool audio_is_ready(void)
{
    return s_audio_ready;
}

esp_err_t audio_play(const uint8_t *data, size_t len)
{
    if (!s_audio_ready || codec_dev == NULL)
        return ESP_ERR_INVALID_STATE;
    if (data == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int total_written = 0;
    const int chunk_size = 512;

    int retries = 0;
    while (total_written < (int)len)
    {
        int remaining = (int)len - total_written;
        int to_write = (remaining > chunk_size) ? chunk_size : remaining;

        int ret = esp_codec_dev_write(codec_dev, (void *)(data + total_written), to_write);
        if (ret == ESP_CODEC_DEV_OK)
        {
            total_written += to_write;
            retries = 0;
        }
        else
        {
            if (++retries >= 3)
            {
                ESP_LOGE(TAG, "Audio write failed after %d retries: %d",
                         retries, ret);
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "Audio write retry %d/3, ret=%d", retries, ret);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    ESP_LOGD(TAG, "Playback chunk done (%d bytes)", total_written);
    return ESP_OK;
}

esp_err_t audio_set_sample_rate(int sample_rate)
{
    if (!s_audio_ready || codec_dev == NULL)
        return ESP_ERR_INVALID_STATE;
    if (sample_rate < 8000 || sample_rate > 48000)
        return ESP_ERR_INVALID_ARG;

    /*
     * ES8311 固定在 44.1kHz，不在播放期间 close/open codec。
     * 输入采样率由 audio_write_pcm() 软件转换。
     */
    return ESP_OK;
}

static int16_t read_mono_sample(const int16_t *samples, size_t index,
                                int channels)
{
    if (channels == 1)
        return samples[index];
    int32_t mixed = (int32_t)samples[index * 2] +
                    samples[index * 2 + 1];
    return (int16_t)(mixed / 2);
}

esp_err_t audio_write_pcm(const int16_t *samples, size_t num_samples,
                          int sample_rate, int channels)
{
    if (!s_audio_ready || codec_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == NULL || num_samples == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (channels != 1 && channels != 2)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate < 8000 || sample_rate > 48000)
        return ESP_ERR_INVALID_ARG;

#if AUDIO_VOLUME_MODE == AUDIO_VOLUME_MODE_SOFTWARE
    const int volume = current_volume;
#else
    const int volume = 100;
#endif
    if (sample_rate == AUDIO_OUTPUT_SAMPLE_RATE && channels == 1 &&
        volume == 100)
    {
        int bytes = (int)(num_samples * sizeof(int16_t));
        return esp_codec_dev_write(codec_dev, (void *)samples, bytes) ==
                       ESP_CODEC_DEV_OK
                   ? ESP_OK
                   : ESP_FAIL;
    }

    static int16_t output[RESAMPLE_BUFFER_SAMPLES];
    const uint64_t step = ((uint64_t)sample_rate << 32) /
                          AUDIO_OUTPUT_SAMPLE_RATE;
    const size_t output_count =
        ((uint64_t)num_samples * AUDIO_OUTPUT_SAMPLE_RATE +
         sample_rate - 1) /
        sample_rate;
    uint64_t position = 0;
    size_t produced = 0;

    while (produced < output_count)
    {
        size_t chunk = output_count - produced;
        if (chunk > RESAMPLE_BUFFER_SAMPLES)
            chunk = RESAMPLE_BUFFER_SAMPLES;

        for (size_t i = 0; i < chunk; i++)
        {
            size_t index = (size_t)(position >> 32);
            uint32_t fraction = (uint32_t)position;
            if (index >= num_samples)
                index = num_samples - 1;
            size_t next = index + 1 < num_samples ? index + 1 : index;
            int32_t first = read_mono_sample(samples, index, channels);
            int32_t second = read_mono_sample(samples, next, channels);
            int64_t delta = (int64_t)(second - first) * fraction;
            int32_t value = first + (int32_t)(delta >> 32);
#if AUDIO_VOLUME_MODE == AUDIO_VOLUME_MODE_SOFTWARE
            output[i] = (int16_t)(value * volume / 100);
#else
            output[i] = (int16_t)value;
#endif
            position += step;
        }

        if (esp_codec_dev_write(codec_dev, output,
                                (int)(chunk * sizeof(int16_t))) !=
            ESP_CODEC_DEV_OK)
            return ESP_FAIL;
        produced += chunk;
    }
    return ESP_OK;
}

esp_err_t audio_set_volume(int vol)
{
    if (!s_audio_ready || codec_dev == NULL)
        return ESP_ERR_INVALID_STATE;
    if (vol < 0 || vol > 100)
        return ESP_ERR_INVALID_ARG;

#if AUDIO_VOLUME_MODE == AUDIO_VOLUME_MODE_ES8311
    esp_err_t ret = set_es8311_hardware_volume(vol);
    if (ret != ESP_OK)
        return ret;
    current_volume = vol;
    ESP_LOGI(TAG, "ES8311 I2C volume=%d", vol);
#else
    current_volume = vol;
    ESP_LOGI(TAG, "Software volume=%d", vol);
#endif
    return ESP_OK;
}

int audio_get_volume(void)
{
    return current_volume;
}

/* ================================================================
 * I2C 总线测试 — 持续读写 ES8311 寄存器，用于示波器观察 SCL/SDA
 * 指令: I2CTEST  →  启停
 * ================================================================ */
static volatile bool s_i2c_test_run = false;
static TaskHandle_t s_i2c_test_task = NULL;

/* ---- I2C 总线恢复：手动发 9 个 SCL 脉冲，释放被锁死的 SDA ---- */
void audio_i2c_bus_recover(void)
{
    int sda_before = gpio_get_level(I2C_ES8311_SDA);
    int scl_before = gpio_get_level(I2C_ES8311_SCL);
    ESP_LOGI(TAG, "I2C recover: SDA=%d SCL=%d (before)", sda_before, scl_before);

    /* 先试推挽输出强驱高，测试脚是否被 ES8311 硬拉到地 */
    gpio_config_t pushpull = {
        .pin_bit_mask = BIT64(I2C_ES8311_SDA) | BIT64(I2C_ES8311_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pushpull);
    gpio_set_level(I2C_ES8311_SDA, 1);
    gpio_set_level(I2C_ES8311_SCL, 1);
    esp_rom_delay_us(5);
    int sda_pp = gpio_get_level(I2C_ES8311_SDA);
    int scl_pp = gpio_get_level(I2C_ES8311_SCL);
    ESP_LOGI(TAG, "I2C recover: push-pull HIGH → SDA=%d SCL=%d %s",
             sda_pp, scl_pp,
             (sda_pp == 1 && scl_pp == 1) ? "(free)" : "(ES8311 pulling down!)");

    /* 再用开漏 + 上拉恢复 */
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(I2C_ES8311_SDA) | BIT64(I2C_ES8311_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD, /* 开漏且保留输入采样 */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(I2C_ES8311_SDA, 1);
    gpio_set_level(I2C_ES8311_SCL, 1);
    esp_rom_delay_us(10);

    /* 标准 I2C 恢复: SCL 发 9 个脉冲，每 2 个周期检查 SDA 是否释放 */
    for (int i = 0; i < 9; i++)
    {
        gpio_set_level(I2C_ES8311_SCL, 0);
        esp_rom_delay_us(10);
        gpio_set_level(I2C_ES8311_SCL, 1);
        esp_rom_delay_us(10);
        if (gpio_get_level(I2C_ES8311_SDA))
        {
            ESP_LOGI(TAG, "I2C recover: SDA released after %d clocks", i + 1);
            break;
        }
    }

    int sda_after = gpio_get_level(I2C_ES8311_SDA);
    ESP_LOGI(TAG, "I2C recover: SDA=%d SCL=%d (after%s)",
             sda_after, gpio_get_level(I2C_ES8311_SCL),
             sda_after ? " - OK" : " - STILL STUCK!");
}

static void i2c_test_thread(void *arg)
{
    int count = 0;
    while (s_i2c_test_run)
    {
        if (ctrl_if && ctrl_if->read_reg)
        {
            int val = 0;
            int ret = ctrl_if->read_reg(ctrl_if, 0x00, 1, &val, 1);
            if (++count % 100 == 0)
            {
                ESP_LOGI(TAG, "I2CTEST %d reads, reg0x00=0x%02X, ret=%d",
                         count, val, ret);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1)); /* ~1kHz 持续 I2C 活动 */
    }
    ESP_LOGI(TAG, "I2CTEST stopped (%d reads)", count);
    __atomic_store_n(&s_i2c_test_task, NULL, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

void audio_i2c_test_start(void)
{
    if (s_i2c_test_run)
    {
        ESP_LOGI(TAG, "I2CTEST already running");
        return;
    }
    if (__atomic_load_n(&s_i2c_test_task, __ATOMIC_ACQUIRE) != NULL)
    {
        ESP_LOGW(TAG, "I2CTEST previous task is still stopping");
        return;
    }
    if (ctrl_if == NULL)
    {
        ESP_LOGE(TAG, "I2CTEST ERR no I2C ctrl interface");
        return;
    }
    s_i2c_test_run = true;
    BaseType_t created = xTaskCreatePinnedToCore(
        i2c_test_thread, "i2ctest", 3072, NULL, 1,
        &s_i2c_test_task, 0);
    if (created != pdPASS)
    {
        s_i2c_test_run = false;
        s_i2c_test_task = NULL;
        ESP_LOGE(TAG, "I2CTEST task creation failed");
        return;
    }
    ESP_LOGI(TAG, "I2CTEST started (1kHz read reg0x00)");
}

void audio_i2c_test_stop(void)
{
    if (!s_i2c_test_run)
    {
        ESP_LOGI(TAG, "I2CTEST not running");
        return;
    }
    s_i2c_test_run = false;
    ESP_LOGI(TAG, "I2CTEST stopping...");

    /* 等待线程自行退出，避免 stop/start 连续操作产生两个测试任务。 */
    for (int wait_ms = 0; wait_ms < 100 &&
         __atomic_load_n(&s_i2c_test_task, __ATOMIC_ACQUIRE) != NULL; wait_ms++)
        vTaskDelay(pdMS_TO_TICKS(1));
    if (__atomic_load_n(&s_i2c_test_task, __ATOMIC_ACQUIRE) != NULL)
        ESP_LOGW(TAG, "I2CTEST task did not stop within 100 ms");
}

bool audio_i2c_test_is_running(void)
{
    return s_i2c_test_run;
}
