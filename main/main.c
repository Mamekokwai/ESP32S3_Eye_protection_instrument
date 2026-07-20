/**
 * @brief  ESP32-S3 眼保仪 — 主循环
 *
 * switch(workspace) 协作多任务, 1ms tick 驱动, 5 槽位轮转.
 * UART 指令 → app_uart.c, 播放器 → *_player.c
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
// #include "esp_adc/adc_oneshot.h"   /* DEBUG: GPIO0 ADC 电压检测, 暂时不用 */
#include "audio.h"
#include "my_spi.h"
#include "spilcd.h"
#include "spi_sd.h"
#include "key.h"
#include "flash_player.h"
#include "audio_player.h"
#include "app_uart.h"

#define TAG "app"

/* ====== 1ms Tick ====== */
static volatile bool g_tick_flag = false;
static void tick_isr(void *arg) { g_tick_flag = true; }

/* ====== Workspace ====== */
typedef enum {
    WS_UART_CMD    = 0,
    WS_APP_STATE   = 1,
    WS_PLAYER_TICK = 2,
    WS_SYSTEM_MON  = 3,
    WS_KEY_SCAN    = 4,
    WS_NUM
} workspace_t;

/* ====== 应用状态 (app_uart.c 通过 extern 访问) ====== */
typedef enum {
    MODE_IDLE          = 0,
    MODE_VIDEO_PLAYING,
    MODE_VIDEO_PAUSED,
    MODE_AUDIO_PLAYING,
    MODE_SLEEP,
} app_mode_t;

app_mode_t g_mode = MODE_IDLE;

/* ---- BOOT 按键 (仅长按 2s → DL) ---- */
static uint16_t g_key_hold = 0;
#define LONG_PRESS_TICKS ((2000 * 1000) / 5000)

// static adc_oneshot_unit_handle_t s_adc1 = NULL;  /* DEBUG: GPIO0 ADC */

static void key_tick(void)
{
#if 0  /* ---- DEBUG: GPIO0 ADC 电压检测, 暂时关闭 ---- */
    static bool adc_inited = false;
    if (!adc_inited) {
        adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
        adc_oneshot_new_unit(&init_cfg, &s_adc1);
        adc_oneshot_chan_cfg_t ch_cfg = {
            .atten = ADC_ATTEN_DB_11,
            .bitwidth = ADC_BITWIDTH_12,
        };
        adc_oneshot_config_channel(s_adc1, ADC_CHANNEL_0, &ch_cfg);
        adc_inited = true;
    }
    static int last_mv = -1;
    int raw, mv = 0;
    adc_oneshot_read(s_adc1, ADC_CHANNEL_0, &raw);
    mv = raw * 3100 / 4095;
    if (abs(mv - last_mv) > 50) {
        ESP_LOGI(TAG, "DEBUG GPIO0=%dmV", mv);
        last_mv = mv;
    }
#endif /* ---- DEBUG END ---- */

    if (key_scan(0) == BOOT_PRES) {
        if (g_key_hold == 0) app_uart_send("KEY BOOT");
        if (++g_key_hold > LONG_PRESS_TICKS) {
            extern void reboot_to_download(void);
            ESP_LOGI(TAG, "BOOT long press -> DL");
            reboot_to_download();
        }
    } else {
        if (g_key_hold > 0) app_uart_send("KEY BOOT UP");
        g_key_hold = 0;
    }
}

/* ---- 状态机 (占位, 状态切换由 UART 指令驱动) ---- */
static void state_tick(void) {}

/* ---- 播放器调度 ---- */
static void player_tick(void)
{
    player_ret_t ret;
    switch (g_mode) {
    case MODE_VIDEO_PLAYING:
        ret = flash_player_tick();
        if (ret == PLAYER_ERROR) {
            ESP_LOGE(TAG, "Flash player error");
            flash_player_stop();
            g_mode = MODE_IDLE;
        }
        break;
    case MODE_AUDIO_PLAYING:
        ret = audio_player_tick();
        if (ret == PLAYER_ERROR) {
            ESP_LOGE(TAG, "Audio player error");
            audio_player_stop();
            g_mode = MODE_IDLE;
        }
        break;
    case MODE_VIDEO_PAUSED:
    case MODE_SLEEP:
    default:
        break;
    }
}

/* ---- 系统监控 (LED 心跳 / 堆日志) ---- */
static uint16_t g_mon = 0;
static void monitor_tick(void)
{
    g_mon++;
    if (g_mon % 50 == 0)   /* 250ms */  gpio_set_level(GPIO_NUM_1, (g_mon / 50) % 2);
    if (g_mon % 400 == 0)  /* 2s   */   ESP_LOGI(TAG, "heap=%lu mode=%d", esp_get_free_heap_size(), g_mode);
}

/* ====== 主入口 ====== */
void app_main(void)
{
    /* 硬件初始化 */
    ESP_LOGI(TAG, "audio init");    audio_init();
    ESP_LOGI(TAG, "SPI init");      my_spi_init();
    ESP_LOGI(TAG, "LCD init");      spilcd_init();
    ESP_LOGI(TAG, "SD mount...");   sd_spi_init();  /* 音频用, 失败不阻塞 */

    spilcd_show_string(40, 140, 280, 170, 16, "ESP32-S3 Eye", BLUE);

    // /* SD 初始化后 GPIO0 恢复上拉 */  /* DEBUG: GPIO0 检测暂时关闭 */
    // gpio_reset_pin(GPIO_NUM_0);
    // gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    // gpio_set_pull_mode(GPIO_NUM_0, GPIO_PULLUP_ONLY);

    key_init();
    gpio_config_t lc = { .pin_bit_mask = BIT64(1), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&lc);

    app_uart_init();

    esp_timer_create_args_t ta = { .callback = tick_isr, .name = "tick" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, 1000);

    /* 主循环 */
    static uint8_t ws = WS_NUM - 1;
    ESP_LOGI(TAG, "loop start");

    while (1) {
        while (!g_tick_flag) { vTaskDelay(1); }
        g_tick_flag = false;
        if (++ws >= WS_NUM) ws = 0;

        switch (ws) {
        case WS_UART_CMD:    app_uart_tick();   break;
        case WS_APP_STATE:   state_tick();      break;
        case WS_PLAYER_TICK: player_tick();      break;
        case WS_SYSTEM_MON:  monitor_tick();     break;
        case WS_KEY_SCAN:    key_tick();          break;
        }
    }
}
