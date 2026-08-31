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
#include "audio.h"
#include "spilcd.h"
#include "sd_card.h"
#include "flash_player.h"
#include "video_player.h"
#include "audio_player.h"
#include "image_viewer.h"
#include "app_uart.h"
#include "production_unlock.h"
#include "gbk_font.h"
#include "esp_efuse.h"
#include "esp_efuse_custom_table.h"

#define TAG "app"

/* ====== 1ms Tick (Semaphore, 不忙等, 不丢 tick) ====== */
#include "freertos/semphr.h"
static SemaphoreHandle_t s_tick_sem = NULL;
static void IRAM_ATTR tick_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tick_sem, &woken);
    if (woken)
        portYIELD_FROM_ISR();
}

/* ====== Workspace ====== */
typedef enum
{
    WS_UART_CMD = 0,
    WS_APP_STATE = 1,
    WS_DISPLAY_TICK = 2,
    WS_SYSTEM_MON = 3,
    WS_RESERVED = 4,
    WS_NUM
} workspace_t;

/* 显示状态与音频播放器状态相互独立。 */
display_mode_t g_display_mode = DISPLAY_IDLE;

/* ---- 状态机 (占位, 状态切换由 UART 指令驱动) ---- */
static void state_tick(void) {}

/* ---- 显示调度：视频和图片共用 LCD，因此两者互斥 ---- */
static void display_tick(void)
{
    player_ret_t ret;
    switch (g_display_mode)
    {
    case DISPLAY_VIDEO_PLAYING:
        ret = flash_player_tick();
        if (ret == PLAYER_ERROR)
        {
            ESP_LOGE(TAG, "Flash player error");
            flash_player_stop();
            g_display_mode = DISPLAY_IDLE;
        }
        break;
    case DISPLAY_SD_VIDEO_PLAYING:
        ret = video_player_tick();
        if (ret == PLAYER_ERROR)
        {
            ESP_LOGE(TAG, "SD video player error");
            video_player_stop();
            app_uart_send("ERR VID playback");
            g_display_mode = DISPLAY_IDLE;
        }
        break;
    case DISPLAY_IMAGE_LOADING:
    {
        image_viewer_state_t image_state = image_viewer_tick();
        if (image_state == IMAGE_VIEWER_DONE)
        {
            char response[192];
            snprintf(response, sizeof(response), "OK %s %s %lux%lu",
                     image_viewer_command(),
                     image_viewer_name(),
                     (unsigned long)image_viewer_width(),
                     (unsigned long)image_viewer_height());
            app_uart_send(response);
            g_display_mode = DISPLAY_IDLE;
        }
        else if (image_state == IMAGE_VIEWER_ERROR)
        {
            char response[64];
            snprintf(response, sizeof(response), "ERR %s %s",
                     image_viewer_command(),
                     esp_err_to_name(image_viewer_last_error()));
            app_uart_send(response);
            g_display_mode = DISPLAY_IDLE;
        }
        break;
    }
    case DISPLAY_VIDEO_PAUSED:
    case DISPLAY_SD_VIDEO_PAUSED:
    case DISPLAY_SLEEP:
    default:
        break;
    }
}

/* ---- 系统监控（堆日志；GPIO2 已专用于音频功放）---- */
static uint16_t g_mon = 0;
static void monitor_tick(void)
{
    g_mon++;
    if (g_mon % 400 == 0) /* 2s   */
        ESP_LOGI(TAG, "heap=%lu display=%d audio=%d",
                 esp_get_free_heap_size(), g_display_mode,
                 audio_player_is_active());
}

/* ====== 启动门: SD 卡检测 + 解密检测 ======
 * 链路: 检查SD卡 → 未插入则提示并重试
 *              → 已插入 → 检查解密 → 未解密则提示并重试
 *                                    → 已解密 → 进入正常启动
 * 提示用内嵌 GBK 字库 (无卡也显示): gbk_embedded_font.h */
static void boot_gate(void)
{
    while (1)
    {
        /* 1. 检查 SD 卡 */
        esp_err_t sd_ret = sd_card_mount();
        if (sd_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "SD card absent; waiting for card");
            gbk_show_boot_text(88, 150, BLACK); /* 白底黑字: 请插入SD卡 */
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* 2. 检查解密 (仅加密开启时) */
#if EYECARE_ENABLE_ENCRYPTION
        if (!esp_efuse_read_field_bit(ESP_EFUSE_USER_DATA_EYECARE_UNLOCKED))
        {
            ESP_LOGW(TAG, "Device locked; waiting for unlock token");
            gbk_show_unlock_text(120, 150, BLACK); /* 白底黑字: 请解密 */
            if (production_unlock_ensure())
            {
                spilcd_clear(WHITE);
                return; /* 已解锁, 进入系统 */
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!production_unlock_ensure())
        {
            /* 安全 eFuse 未启用时生产锁失败, 直接放行开发板 */
            ESP_LOGE(TAG, "Production unlock unavailable");
        }
#endif

        return; /* 卡已插入 + 已解锁 → 正常启动 */
    }
}

/* ====== 主入口 ====== */
void app_main(void)
{
    /* 硬件初始化: 先 LCD (背光/显示), 再进入启动门 */
    ESP_LOGI(TAG, "LCD init");
    spilcd_init();

    /* 启动门: 无卡提示 / 未解锁提示, 直到条件满足 */
    boot_gate();

    /* 中文字库: 从 TF 卡 /SYSTEM/FONT/GBK16.FON 加载 (SD 已就绪) */
    gbk_font_init();

    /* 音频初始化 */
    ESP_LOGI(TAG, "audio init");
    esp_err_t audio_ret = audio_init();
    if (audio_ret != ESP_OK)
        ESP_LOGE(TAG, "Audio init failed (%s); video/display will continue",
                 esp_err_to_name(audio_ret));
    ESP_ERROR_CHECK(audio_player_start_service());

    app_uart_init();

    /* 上电自动播放 */
    esp_err_t auto_play_ret = flash_player_init();
    if (auto_play_ret == ESP_OK)
    {
        g_display_mode = DISPLAY_VIDEO_PLAYING;
        ESP_LOGI(TAG, "Auto VPLAY OK");
    }
    else
    {
        ESP_LOGW(TAG, "Auto VPLAY unavailable: %s",
                 esp_err_to_name(auto_play_ret));
        spilcd_clear(WHITE);
        spilcd_show_text16(112, 136, "READY", BLUE, WHITE);
        spilcd_show_text16(88, 168, "NO FLASH VIDEO", BLACK, WHITE);
    }

    s_tick_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_tick_sem ? ESP_OK : ESP_ERR_NO_MEM);

    const esp_timer_create_args_t timer_config = {
        .callback = tick_isr,
        .name = "tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_config, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 1000));

    /* 主循环 */
    static uint8_t ws = WS_NUM - 1;
    ESP_LOGI(TAG, "loop start");

    while (1)
    {
        xSemaphoreTake(s_tick_sem, portMAX_DELAY); /* 阻塞等 1ms tick, 让出 CPU 给 idle */
        if (++ws >= WS_NUM)
            ws = 0;

        /* 视频每 1ms 检查一次以衔接异步解码和条带 DMA；
         * 音频仍保留独立 CPU1 服务任务。 */
        if (g_display_mode == DISPLAY_VIDEO_PLAYING ||
            g_display_mode == DISPLAY_SD_VIDEO_PLAYING)
            display_tick();

        switch (ws)
        {
        case WS_UART_CMD:
            app_uart_tick();
            break;
        case WS_APP_STATE:
            state_tick();
            break;
        case WS_DISPLAY_TICK:
            if (g_display_mode != DISPLAY_VIDEO_PLAYING &&
                g_display_mode != DISPLAY_SD_VIDEO_PLAYING)
                display_tick();
            break;
        case WS_SYSTEM_MON:
            monitor_tick();
            break;
        case WS_RESERVED:
            break;
        }
    }
}
