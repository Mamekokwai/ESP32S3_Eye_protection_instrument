/**
 * @brief  ESP32-S3 眼保仪 — 协作多任务 + UART 指令控制
 *
 * switch(work_space) 时间片轮询, 参考 MC32P7031 协作多任务架构:
 *   1ms timer ISR → flag → 主循环等 tick → workspace++ → switch 分发
 *
 * 存储分工: Flash = AVI 视频, SD 卡 = PCM 音频
 * 控制方式: UART 指令 (CA51F352P4 触控 MCU → ESP32)
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "audio.h"
#include "my_spi.h"
#include "spilcd.h"
#include "spi_sd.h"
#include "key.h"
#include "video_player.h"
#include "raw_player.h"
#include "flash_player.h"
#include "audio_player.h"
#include "reset_to_dl.h"

#define TAG "app"

/* ====== Tick 驱动 ====== */
static volatile bool g_tick_flag = false;
static void tick_isr(void *arg) { g_tick_flag = true; }

/* ====== Workspace 槽位 (5 槽 @5ms) ====== */
typedef enum {
    WS_UART_CMD    = 0,
    WS_APP_STATE   = 1,
    WS_PLAYER_TICK = 2,
    WS_SYSTEM_MON  = 3,
    WS_KEY_SCAN    = 4,
    WS_NUM
} workspace_t;

/* ====== 应用状态机 ====== */
typedef enum {
    MODE_IDLE          = 0,
    MODE_VIDEO_PLAYING,
    MODE_VIDEO_PAUSED,
    MODE_AUDIO_PLAYING,
    MODE_SLEEP,
} app_mode_t;

static app_mode_t g_mode = MODE_IDLE;

/* ====== UART 指令 (CA51F352P4 → ESP32) ====== */
#define UART_PORT       UART_NUM_1
#define UART_RX_PIN     GPIO_NUM_44
#define UART_TX_PIN     GPIO_NUM_38   /* 与 LCD_DC 共享, 仅短响应 */
#define UART_BAUD       115200
#define UART_BUF_SIZE   512

static char g_cmd_line[128];
static int  g_cmd_pos = 0;

/* ---- UART 初始化 ---- */
static void uart_cmd_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0);
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "UART1 init: RX=IO%d TX=IO%d @%d", UART_RX_PIN, UART_TX_PIN, UART_BAUD);
}

/* ---- UART 发送响应 ---- */
static void uart_send(const char *msg)
{
    uart_write_bytes(UART_PORT, msg, strlen(msg));
    uart_write_bytes(UART_PORT, "\r\n", 2);
}

/* ---- 指令解析 ---- */
static void cmd_handle(const char *cmd)
{
    ESP_LOGI(TAG, "CMD: [%s]", cmd);

    /* === 视频控制 === */
    if (strcasecmp(cmd, "VPLAY") == 0) {
        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED) {
            uart_send("OK VPLAY (already)");
        } else {
            if (g_mode == MODE_AUDIO_PLAYING) audio_player_stop();
            if (flash_player_init() == ESP_OK) {
                g_mode = MODE_VIDEO_PLAYING;
                spilcd_show_string(0, 0, 320, 16, 16, "Video playing", BLACK);
                uart_send("OK VPLAY");
            } else {
                uart_send("ERR no video");
            }
        }
    }
    else if (strcasecmp(cmd, "VPAUSE") == 0) {
        if (g_mode == MODE_VIDEO_PLAYING) {
            g_mode = MODE_VIDEO_PAUSED;
            spilcd_show_string(0, 0, 320, 16, 16, "Video paused", BLACK);
            uart_send("OK VPAUSE");
        } else {
            uart_send("ERR not playing");
        }
    }
    else if (strcasecmp(cmd, "VRESUME") == 0) {
        if (g_mode == MODE_VIDEO_PAUSED) {
            g_mode = MODE_VIDEO_PLAYING;
            spilcd_show_string(0, 0, 320, 16, 16, "Video playing", BLACK);
            uart_send("OK VRESUME");
        } else {
            uart_send("ERR not paused");
        }
    }
    else if (strcasecmp(cmd, "VSTOP") == 0) {
        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED) {
            flash_player_stop();
            g_mode = MODE_IDLE;
            spilcd_show_string(0, 0, 320, 16, 16, "Idle", BLACK);
            uart_send("OK VSTOP");
        } else {
            uart_send("ERR no video");
        }
    }
    /* === 音频控制 === */
    else if (strcasecmp(cmd, "ALIST") == 0) {
        char list[512];
        int n = audio_player_list_files(list, sizeof(list));
        if (n > 0) {
            uart_send("ALIST");
            uart_send(list);
        } else if (n == 0) {
            uart_send("ALIST NONE");
        } else {
            uart_send("ERR no sd");
        }
    }
    else if (strncasecmp(cmd, "APLAY ", 6) == 0) {
        const char *arg = cmd + 6;
        while (*arg == ' ') arg++;

        /* 构建 SD 卡路径 */
        char path[272];
        if (arg[0] == '/') {
            snprintf(path, sizeof(path), "/0:%s", arg + (arg[1] == '0' && arg[2] == ':' ? 3 : 0));
        } else if (arg[0] >= '0' && arg[0] <= '9') {
            /* 按序号: APLAY 1 → SD 卡第一个 .pcm 文件 */
            int idx = atoi(arg);
            DIR *dir = opendir("/0:");
            if (!dir) { uart_send("ERR no sd"); return; }
            int cnt = 0;
            struct dirent *e;
            while ((e = readdir(dir)) != NULL) {
                size_t l = strlen(e->d_name);
                if (l > 4 && strcasecmp(e->d_name + l - 4, ".pcm") == 0) {
                    if (++cnt == idx) {
                        snprintf(path, sizeof(path), "0:%s", e->d_name);
                        break;
                    }
                }
            }
            closedir(dir);
            if (path[0] == 0) { uart_send("ERR no such file"); return; }
        } else {
            snprintf(path, sizeof(path), "0:%s", arg);
        }

        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED) flash_player_stop();
        if (audio_player_init(path) == ESP_OK) {
            g_mode = MODE_AUDIO_PLAYING;
            uart_send("OK APLAY");
        } else {
            uart_send("ERR open fail");
        }
    }
    else if (strcasecmp(cmd, "ASTOP") == 0) {
        if (g_mode == MODE_AUDIO_PLAYING) {
            audio_player_stop();
            g_mode = MODE_IDLE;
            uart_send("OK ASTOP");
        } else {
            uart_send("ERR no audio");
        }
    }
    else if (strcasecmp(cmd, "AMUTE") == 0) {
        bool muted = audio_player_toggle_mute();
        char resp[32];
        snprintf(resp, sizeof(resp), "OK AMUTE %s", muted ? "on" : "off");
        uart_send(resp);
    }
    else if (strncasecmp(cmd, "VOL ", 4) == 0) {
        int vol = atoi(cmd + 4);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        audio_player_set_volume(vol);
        char resp[32];
        snprintf(resp, sizeof(resp), "OK VOL %d", vol);
        uart_send(resp);
    }
    /* === 系统控制 === */
    else if (strcasecmp(cmd, "DL") == 0) {
        uart_send("OK DL");
        vTaskDelay(pdMS_TO_TICKS(100));
        reboot_to_download();
    }
    else if (strcasecmp(cmd, "RST") == 0) {
        uart_send("OK RST");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    else if (strcasecmp(cmd, "STATUS") == 0) {
        const char *s = "idle";
        if (g_mode == MODE_VIDEO_PLAYING) s = "video playing";
        else if (g_mode == MODE_VIDEO_PAUSED) s = "video paused";
        else if (g_mode == MODE_AUDIO_PLAYING) s = "audio playing";
        else if (g_mode == MODE_SLEEP) s = "sleep";

        const char *af = audio_player_current_file();
        char resp[128];
        if (af) snprintf(resp, sizeof(resp), "STATUS %s audio=%s", s, af);
        else    snprintf(resp, sizeof(resp), "STATUS %s", s);
        uart_send(resp);
    }
    else if (strcasecmp(cmd, "INFO") == 0) {
        char resp[64];
        snprintf(resp, sizeof(resp), "INFO heap=%lu", esp_get_free_heap_size());
        uart_send(resp);
    }
    else if (strcasecmp(cmd, "SLEEP") == 0) {
        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED) flash_player_stop();
        if (g_mode == MODE_AUDIO_PLAYING) audio_player_stop();
        g_mode = MODE_SLEEP;
        spilcd_clear(BLACK);
        spilcd_show_string(40, 140, 280, 170, 16, "Sleep", BLACK);
        uart_send("OK SLEEP");
    }
    else if (strcasecmp(cmd, "WAKE") == 0) {
        if (g_mode == MODE_SLEEP) {
            g_mode = MODE_IDLE;
            spilcd_clear(BLACK);
            spilcd_show_string(40, 140, 280, 170, 16, "ESP32-S3 Eye", BLUE);
            uart_send("OK WAKE");
        } else {
            uart_send("OK WAKE (already awake)");
        }
    }
    /* === 显示控制 === */
    else if (strcasecmp(cmd, "LCD ON") == 0) {
        /* 背光由 CA51F352P4 控制, 转发指令 */
        uart_send("LCD ON");  /* CA51F352P4 收到后打开 PWM */
    }
    else if (strcasecmp(cmd, "LCD OFF") == 0) {
        uart_send("LCD OFF");
    }
    else if (strncasecmp(cmd, "LCD B", 5) == 0) {
        /* 亮度 0-100, 转发给 CA51F352P4 */
        int b = atoi(cmd + 5);
        if (b < 0) b = 0;
        if (b > 100) b = 100;
        char resp[32];
        snprintf(resp, sizeof(resp), "LCD B%d", b);
        uart_send(resp);
    }
    else {
        char resp[64];
        snprintf(resp, sizeof(resp), "ERR unknown: %s", cmd);
        uart_send(resp);
    }
}

/* ---- UART 接收 + 行解析 (每 5ms) ---- */
static void uart_cmd_tick(void)
{
    uint8_t ch;
    while (uart_read_bytes(UART_PORT, &ch, 1, 0) > 0) {
        if (ch == '\n' || ch == '\r') {
            if (g_cmd_pos > 0) {
                g_cmd_line[g_cmd_pos] = 0;
                /* 去除尾部空白 */
                while (g_cmd_pos > 0 && g_cmd_line[g_cmd_pos - 1] == ' ')
                    g_cmd_line[--g_cmd_pos] = 0;
                if (g_cmd_pos > 0) cmd_handle(g_cmd_line);
                g_cmd_pos = 0;
            }
        } else if (g_cmd_pos < (int)sizeof(g_cmd_line) - 1) {
            g_cmd_line[g_cmd_pos++] = (char)ch;
        }
    }
}

/* ====== 按键 (BOOT 仅长按 → DL) ====== */
static uint16_t g_key_hold = 0;
#define LONG_PRESS_TICKS ((2000 * 1000) / 5000)

static void key_tick_handler(void)
{
    if (key_scan(0) == BOOT_PRES) {
        if (++g_key_hold > LONG_PRESS_TICKS) {
            ESP_LOGI(TAG, "BOOT long press -> DL");
            reboot_to_download();
        }
    } else {
        g_key_hold = 0;
    }
}

/* ====== 应用状态机 (每 5ms) ====== */
static void app_state_tick(void)
{
    /* 状态机由 UART 指令驱动 (cmd_handle 中直接切换 g_mode) */
    /* 此 slot 可用于周期性状态维护 */
}

/* ====== 播放器帧调度 (每 5ms) ====== */
static void player_tick_handler(void)
{
    player_ret_t ret = PLAYER_BUSY;

    switch (g_mode) {
    case MODE_VIDEO_PLAYING:
        ret = flash_player_tick();
        if (ret == PLAYER_ERROR) {
            ESP_LOGE(TAG, "Flash player error");
            flash_player_stop();
            g_mode = MODE_IDLE;
        }
        break;
    case MODE_VIDEO_PAUSED:
        /* 不推进帧, 保持当前画面 */
        break;
    case MODE_AUDIO_PLAYING:
        ret = audio_player_tick();
        if (ret == PLAYER_ERROR) {
            ESP_LOGE(TAG, "Audio player error");
            audio_player_stop();
            g_mode = MODE_IDLE;
        }
        break;
    default:
        break;
    }
}

/* ====== 系统监控 (LED 心跳, 堆日志) ====== */
static uint16_t g_mon_cnt = 0;
static void system_tick_handler(void)
{
    g_mon_cnt++;
    if (g_mon_cnt % 50 == 0) {   /* 250ms */
        gpio_set_level(GPIO_NUM_1, (g_mon_cnt / 50) % 2);
    }
    if (g_mon_cnt % 400 == 0) {  /* 2s */
        ESP_LOGI(TAG, "heap: %lu mode=%d", esp_get_free_heap_size(), g_mode);
    }
}

/* ====== 主入口 ====== */
void app_main(void)
{
    /* ---- 硬件初始化 ---- */
    ESP_LOGI(TAG, "init audio (ES8311)");
    audio_init();

    ESP_LOGI(TAG, "init SPI bus");
    my_spi_init();

    ESP_LOGI(TAG, "init LCD");
    spilcd_init();

    /* 尝试挂载 SD 卡 (音频用) */
    ESP_LOGI(TAG, "mounting SD card...");
    if (sd_spi_init() == ESP_OK) {
        ESP_LOGI(TAG, "SD card OK");
    } else {
        ESP_LOGW(TAG, "No SD card (audio unavailable)");
    }

    /* 空闲画面 */
    spilcd_show_string(40, 140, 280, 170, 16, "ESP32-S3 Eye", BLUE);

    /* 按键 (长按→DL) */
    key_init();

    /* LED 心跳 */
    gpio_config_t led_cfg = {
        .pin_bit_mask = BIT64(1),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    gpio_set_level(GPIO_NUM_1, 0);

    /* UART 指令 (CA51F352P4) */
    uart_cmd_init();

    /* 1ms timer */
    esp_timer_create_args_t ta = { .callback = tick_isr, .name = "tick" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, 1000);

    /* ---- 协作多任务主循环 ---- */
    static uint8_t ws = WS_NUM - 1;
    ESP_LOGI(TAG, "Enter main loop");

    while (1) {
        while (!g_tick_flag) { asm volatile("nop"); }
        g_tick_flag = false;

        if (++ws >= WS_NUM) ws = 0;

        switch (ws) {
        case WS_UART_CMD:    uart_cmd_tick();       break;
        case WS_APP_STATE:   app_state_tick();      break;
        case WS_PLAYER_TICK: player_tick_handler(); break;
        case WS_SYSTEM_MON:  system_tick_handler(); break;
        case WS_KEY_SCAN:    key_tick_handler();    break;
        }
    }
}
