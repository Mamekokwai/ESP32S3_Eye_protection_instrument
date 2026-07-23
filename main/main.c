/**
 * @brief  ESP32-S3 眼保仪 — 主循环
 *
 * switch(workspace) 协作多任务, 1ms tick 驱动, 5 槽位轮转.
 * UART 指令 → app_uart.c, 播放器 → *_player.c
 */

#include <stdio.h>
#include <string.h>
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
#include "flash_player.h"
#include "audio_player.h"
#include "app_uart.h"
#include "esp_cache.h"

#define TAG "app"

/* ====== PSRAM vs Internal DMA 显示诊断 ====== */
#define DIAG_STRIP_H 40
#define DIAG_FRAMES 60

typedef enum
{
    BUF_INTERNAL_DMA,
    BUF_PSRAM,
    BUF_PSRAM_CACHED
} buf_type_t;

/* 用指定内存类型填充全屏纯色并发送, 测帧率和显示 */
static void __attribute__((unused)) diag_fill_test(const char *label, buf_type_t btype, uint16_t color, int fps_target)
{
    int w = 320, h = 320;
    size_t buf_sz = w * DIAG_STRIP_H * sizeof(uint16_t);
    uint16_t *buf;

    if (btype == BUF_INTERNAL_DMA)
    {
        buf = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    else
    {
        buf = heap_caps_aligned_alloc(64, buf_sz, MALLOC_CAP_SPIRAM);
    }
    if (!buf)
    {
        ESP_LOGE(TAG, "%s: alloc fail", label);
        return;
    }

    /* 填充颜色 */
    for (int i = 0; i < w * DIAG_STRIP_H; i++)
        buf[i] = color;

    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < DIAG_FRAMES; f++)
    {
        /* 每帧变色调, 确认帧在刷新 */
        {
            uint16_t c = color;
            if (f % 3 == 1)
                c = 0x07E0; /* GREEN */
            if (f % 3 == 2)
                c = 0x001F; /* BLUE */
            for (int i = 0; i < w * DIAG_STRIP_H; i++)
                buf[i] = c;
        }

        /* cache sync once (buffer 不变, 每帧只需一次) */
        if (btype == BUF_PSRAM_CACHED)
            esp_cache_msync(buf, buf_sz, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        for (int ys = 0; ys < h; ys += DIAG_STRIP_H)
        {
            int row_h = (ys + DIAG_STRIP_H > h) ? h - ys : DIAG_STRIP_H;
            refresh_done_flag = 0;
            esp_lcd_panel_draw_bitmap(panel_handle, 0, ys, w, ys + row_h, buf);
            while (!refresh_done_flag)
                vTaskDelay(1);
        }

        /* 帧率控制 */
        if (fps_target > 0)
        {
            int64_t target = t0 + (f + 1) * 1000000LL / fps_target;
            while (esp_timer_get_time() < target)
                vTaskDelay(1);
        }
    }
    int64_t elapsed = esp_timer_get_time() - t0;
    float fps = DIAG_FRAMES * 1e6f / elapsed;
    ESP_LOGI(TAG, "%s: %.1f fps, %lld ms, mem=%s",
             label, fps, elapsed / 1000,
             btype == BUF_INTERNAL_DMA ? "internal" : "psram");

    heap_caps_free(buf);
}

/* ====== 1ms Tick (Semaphore, 不忙等, 不丢 tick) ====== */
#include "freertos/semphr.h"
static SemaphoreHandle_t s_tick_sem = NULL;
static void IRAM_ATTR tick_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tick_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* ====== Workspace ====== */
typedef enum
{
    WS_UART_CMD = 0,
    WS_APP_STATE = 1,
    WS_PLAYER_TICK = 2,
    WS_SYSTEM_MON = 3,
    WS_RESERVED = 4,
    WS_NUM
} workspace_t;

/* ====== 应用状态 (app_uart.c 通过 extern 访问) ====== */
typedef enum
{
    MODE_IDLE = 0,
    MODE_VIDEO_PLAYING,
    MODE_VIDEO_PAUSED,
    MODE_AUDIO_PLAYING,
    MODE_SLEEP,
} app_mode_t;

app_mode_t g_mode = MODE_IDLE;

/* ---- 状态机 (占位, 状态切换由 UART 指令驱动) ---- */
static void state_tick(void) {}

/* ---- 播放器调度 ---- */
static void player_tick(void)
{
    player_ret_t ret;
    switch (g_mode)
    {
    case MODE_VIDEO_PLAYING:
        ret = flash_player_tick();
        if (ret == PLAYER_ERROR)
        {
            ESP_LOGE(TAG, "Flash player error");
            flash_player_stop();
            g_mode = MODE_IDLE;
        }
        break;
    case MODE_AUDIO_PLAYING:
        ret = audio_player_tick();
        if (ret == PLAYER_ERROR)
        {
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
    if (g_mon % 50 == 0) /* 250ms */
        gpio_set_level(GPIO_NUM_2, (g_mon / 50) % 2);
    if (g_mon % 400 == 0) /* 2s   */
        ESP_LOGI(TAG, "heap=%lu mode=%d", esp_get_free_heap_size(), g_mode);
}

/* ====== 主入口 ====== */
void app_main(void)
{
    /* 硬件初始化 */
    ESP_LOGI(TAG, "audio init");
    audio_init();
    ESP_LOGI(TAG, "SPI init");
    my_spi_init();
    ESP_LOGI(TAG, "LCD init");
    spilcd_init();
    ESP_LOGI(TAG, "SD mount...");
    sd_spi_init(); /* 音频用, 失败不阻塞 */

    /* 显示测试图片 */
    {
        extern const uint8_t testimage_data[];
#define IMG_STRIP_H 20
        uint16_t *buf = heap_caps_malloc(320 * IMG_STRIP_H * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!buf)
        {
            ESP_LOGE(TAG, "DMA buf alloc failed");
        }
        else
        {
            for (int y = 0; y < 320; y += IMG_STRIP_H)
            {
                int h = (y + IMG_STRIP_H > 320) ? 320 - y : IMG_STRIP_H;
                memcpy(buf, testimage_data + y * 320 * 2, 320 * h * 2);
                refresh_done_flag = 0;
                esp_lcd_panel_draw_bitmap(panel_handle, 0, y, 320, y + h, buf);
                while (!refresh_done_flag)
                    vTaskDelay(1);
            }
            heap_caps_free(buf);
            ESP_LOGI(TAG, "image draw done");
        }
    }

    // gpio_config_t lc = {.pin_bit_mask = BIT64(1), .mode = GPIO_MODE_OUTPUT};
    // gpio_config(&lc);
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT); // LED 改用 IO2

    app_uart_init();

    /* 上电自动播放 */
    if (flash_player_init() == ESP_OK)
    {
        g_mode = MODE_VIDEO_PLAYING;
        ESP_LOGI(TAG, "Auto VPLAY OK");
    }

    s_tick_sem = xSemaphoreCreateBinary();

    esp_timer_create_args_t ta = {.callback = tick_isr, .name = "tick"};
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, 1000);

    /* 主循环 */
    static uint8_t ws = WS_NUM - 1;
    ESP_LOGI(TAG, "loop start");

    static uint16_t test1 = 0;

    while (1)
    {
        xSemaphoreTake(s_tick_sem, portMAX_DELAY);  /* 阻塞等 1ms tick, 让出 CPU 给 idle */
        if (++ws >= WS_NUM)
            ws = 0;

        /* LCD 每条 40 行 DMA 约 5.1ms；视频每 1ms 检查一次可及时续传，
         * 音频仍保留在 5ms workspace 中，避免改变其阻塞写入节奏。 */
        if (g_mode == MODE_VIDEO_PLAYING)
            player_tick();
        // 颜色测试
        //  if (++test1 >= 1000)
        //  {
        //      test1 = 0;
        //      spilcd_fill_raw(0, 0, 320, 320, 0XF800); /* 测试: 每 10ms 填充红色 */
        //      ESP_LOGI(TAG, "测试信息");
        //  }

        // 1S打印一次日志 不要删除
        if (++test1 >= 1000)
        {
            test1 = 0;
            ESP_LOGI(TAG, "测试信息");
        }

        switch (ws)
        {
        case WS_UART_CMD:
            app_uart_tick();
            break;
        case WS_APP_STATE:
            state_tick();
            break;
        case WS_PLAYER_TICK:
            if (g_mode != MODE_VIDEO_PLAYING)
                player_tick();
            break;
        case WS_SYSTEM_MON:
            monitor_tick();
            break;
        case WS_RESERVED:
            break;
        }
    }
}
