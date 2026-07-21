/**
 * @brief  UART 指令系统 — 接收 CA51F352P4 指令, 驱动屏幕
 *
 * CA51F352P4 TX → IO44 → UART1 RX
 * 调试输出: printf → UART0 TX (IO43)
 *
 * TODO: 启用 USB-serial-JTAG 后 stdout→USB, stdin→USB (双源收指令)
 *
 * 指令: VPLAY / VPAUSE / VRESUME / VSTOP
 *       APLAY / ALIST / ASTOP / AMUTE / VOL
 *       DL / RST / STATUS / INFO / SLEEP / WAKE
 */

#include "app_uart.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
// #include <fcntl.h>    /* TODO: USB-serial-JTAG stdin */
// #include <errno.h>
// #include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "spilcd.h"
#include "flash_player.h"
#include "audio_player.h"
#include "reset_to_dl.h"

#define TAG "uart"

/* ---- UART1 仅 RX ---- */
#define UART_PORT    UART_NUM_1
#define UART_RX_PIN  GPIO_NUM_44
#define UART_BAUD    115200
#define UART_BUF_SIZE 512

/* ---- 指令缓冲 ---- */
static char g_line[128];
static int g_pos = 0;

/* ---- 应用状态 (extern, main.c 定义) ---- */
typedef enum
{
    MODE_IDLE = 0,
    MODE_VIDEO_PLAYING,
    MODE_VIDEO_PAUSED,
    MODE_AUDIO_PLAYING,
    MODE_SLEEP,
} app_mode_t;

extern app_mode_t g_mode;

/* ====== 内部 ====== */

/* printf → UART0 TX=IO43 (CA51 和电脑都收得到) */
static void uart_send_str(const char *msg)
{
    printf("%s\r\n", msg);
}

static void cmd_handle(const char *cmd)
{
    ESP_LOGI(TAG, "CMD: [%s]", cmd);

    /* === 视频控制 === */
    if (strcasecmp(cmd, "VPLAY") == 0)
    {
        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED)
        {
            uart_send_str("OK VPLAY (already)");
        }
        else
        {
            if (g_mode == MODE_AUDIO_PLAYING)
                audio_player_stop();
            if (flash_player_init() == ESP_OK)
            {
                g_mode = MODE_VIDEO_PLAYING;
                spilcd_show_string(0, 0, 320, 16, 16, "Video playing", BLACK);
                uart_send_str("OK VPLAY");
            }
            else
            {
                uart_send_str("ERR no video in flash (use tools/flash_video.sh)");
            }
        }
        return;
    }
    if (strcasecmp(cmd, "VPAUSE") == 0)
    {
        if (g_mode == MODE_VIDEO_PLAYING)
        {
            g_mode = MODE_VIDEO_PAUSED;
            spilcd_show_string(0, 0, 320, 16, 16, "Video paused", BLACK);
            uart_send_str("OK VPAUSE");
        }
        else
            uart_send_str("ERR not playing");
        return;
    }
    if (strcasecmp(cmd, "VRESUME") == 0)
    {
        if (g_mode == MODE_VIDEO_PAUSED)
        {
            g_mode = MODE_VIDEO_PLAYING;
            spilcd_show_string(0, 0, 320, 16, 16, "Video playing", BLACK);
            uart_send_str("OK VRESUME");
        }
        else
            uart_send_str("ERR not paused");
        return;
    }
    if (strcasecmp(cmd, "VSTOP") == 0)
    {
        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED)
        {
            flash_player_stop();
            g_mode = MODE_IDLE;
            spilcd_show_string(0, 0, 320, 16, 16, "Idle", BLACK);
            uart_send_str("OK VSTOP");
        }
        else
            uart_send_str("ERR no video");
        return;
    }

    /* === 音频控制 === */
    if (strcasecmp(cmd, "ALIST") == 0)
    {
        char list[512];
        int n = audio_player_list_files(list, sizeof(list));
        if (n > 0)
        {
            uart_send_str("ALIST");
            uart_send_str(list);
        }
        else if (n == 0)
            uart_send_str("ALIST NONE");
        else
            uart_send_str("ERR no SD card (ALIST failed)");
        return;
    }
    if (strcasecmp(cmd, "APLAY") == 0)
    {
        uart_send_str("ERR usage: APLAY <n> or APLAY <filename>");
        return;
    }
    if (strncasecmp(cmd, "APLAY ", 6) == 0)
    {
        const char *arg = cmd + 6;
        while (*arg == ' ')
            arg++;

        char path[272] = {0};
        if (arg[0] == '/')
        {
            snprintf(path, sizeof(path), "/0:%s",
                     arg + (arg[1] == '0' && arg[2] == ':' ? 3 : 0));
        }
        else if (arg[0] >= '0' && arg[0] <= '9')
        {
            int idx = atoi(arg);
            DIR *dir = opendir("/0:");
            if (!dir)
            {
                uart_send_str("ERR no sd");
                return;
            }
            int cnt = 0;
            struct dirent *e;
            while ((e = readdir(dir)) != NULL)
            {
                size_t l = strlen(e->d_name);
                if (l > 4 && strcasecmp(e->d_name + l - 4, ".pcm") == 0)
                {
                    if (++cnt == idx)
                    {
                        snprintf(path, sizeof(path), "0:%s", e->d_name);
                        break;
                    }
                }
            }
            closedir(dir);
            if (path[0] == 0)
            {
                uart_send_str("ERR no such file");
                return;
            }
        }
        else
        {
            snprintf(path, sizeof(path), "0:%s", arg);
        }

        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED)
            flash_player_stop();
        if (audio_player_init(path) == ESP_OK)
        {
            g_mode = MODE_AUDIO_PLAYING;
            uart_send_str("OK APLAY");
        }
        else
        {
            uart_send_str("ERR cannot open file (SD card?)");
        }
        return;
    }
    if (strcasecmp(cmd, "ASTOP") == 0)
    {
        if (g_mode == MODE_AUDIO_PLAYING)
        {
            audio_player_stop();
            g_mode = MODE_IDLE;
            uart_send_str("OK ASTOP");
        }
        else
            uart_send_str("ERR no audio");
        return;
    }
    if (strcasecmp(cmd, "AMUTE") == 0)
    {
        bool muted = audio_player_toggle_mute();
        char r[32];
        snprintf(r, sizeof(r), "OK AMUTE %s", muted ? "on" : "off");
        uart_send_str(r);
        return;
    }
    if (strcasecmp(cmd, "VOL") == 0)
    {
        uart_send_str("ERR usage: VOL <0-100>");
        return;
    }
    if (strncasecmp(cmd, "VOL ", 4) == 0)
    {
        int v = atoi(cmd + 4);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_player_set_volume(v);
        char r[32];
        snprintf(r, sizeof(r), "OK VOL %d", v);
        uart_send_str(r);
        return;
    }

    /* === 系统控制 === */
    if (strcasecmp(cmd, "DL") == 0)
    {
        uart_send_str("OK DL");
        vTaskDelay(pdMS_TO_TICKS(100));
        reboot_to_download();
    }
    if (strcasecmp(cmd, "RST") == 0)
    {
        uart_send_str("OK RST");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    if (strcasecmp(cmd, "STATUS") == 0)
    {
        const char *s = "idle";
        if (g_mode == MODE_VIDEO_PLAYING)
            s = "video playing";
        else if (g_mode == MODE_VIDEO_PAUSED)
            s = "video paused";
        else if (g_mode == MODE_AUDIO_PLAYING)
            s = "audio playing";
        else if (g_mode == MODE_SLEEP)
            s = "sleep";
        const char *af = audio_player_current_file();
        char r[128];
        if (af)
            snprintf(r, sizeof(r), "STATUS %s audio=%s", s, af);
        else
            snprintf(r, sizeof(r), "STATUS %s", s);
        uart_send_str(r);
        return;
    }
    if (strcasecmp(cmd, "INFO") == 0)
    {
        char r[64];
        snprintf(r, sizeof(r), "INFO heap=%lu", esp_get_free_heap_size());
        uart_send_str(r);
        return;
    }
    if (strcasecmp(cmd, "SLEEP") == 0)
    {
        if (g_mode == MODE_VIDEO_PLAYING || g_mode == MODE_VIDEO_PAUSED)
            flash_player_stop();
        if (g_mode == MODE_AUDIO_PLAYING)
            audio_player_stop();
        g_mode = MODE_SLEEP;
        spilcd_clear(BLACK);
        spilcd_show_string(40, 140, 280, 170, 16, "Sleep", BLACK);
        uart_send_str("OK SLEEP");
        return;
    }
    if (strcasecmp(cmd, "WAKE") == 0)
    {
        if (g_mode == MODE_SLEEP)
        {
            g_mode = MODE_IDLE;
            spilcd_clear(BLACK);
            spilcd_show_string(40, 140, 280, 170, 16, "ESP32-S3 Eye", BLUE);
        }
        uart_send_str("OK WAKE");
        return;
    }

    /* unknown */
    char r[64];
    snprintf(r, sizeof(r), "ERR unknown: %s", cmd);
    uart_send_str(r);
}

/* ====== 公开 API ====== */

/* ---- 单字符指令解析 ---- */
static void feed_char(char ch)
{
    if (ch == '\n' || ch == '\r')
    {
        if (g_pos > 0)
        {
            g_line[g_pos] = 0;
            while (g_pos > 0 && g_line[g_pos - 1] == ' ')
                g_line[--g_pos] = 0;
            if (g_pos > 0)
                cmd_handle(g_line);
            g_pos = 0;
        }
    }
    else if (g_pos < (int)sizeof(g_line) - 1)
    {
        g_line[g_pos++] = ch;
    }
}

void app_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0);
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, UART_PIN_NO_CHANGE, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // /* TODO: USB-serial-JTAG 启用后设 stdin 为非阻塞 */
    // fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    ESP_LOGI(TAG, "UART1 RX=IO%d @%d", UART_RX_PIN, UART_BAUD);
}

void app_uart_tick(void)
{
    // /* TODO: USB-serial-JTAG 启用后从 stdin 读用户指令 */
    // char ch;
    // while (read(STDIN_FILENO, &ch, 1) > 0)
    //     feed_char(ch);

    /* UART1 IO44 (CA51F352P4 指令) */
    uint8_t uch;
    while (uart_read_bytes(UART_PORT, &uch, 1, 0) > 0)
        feed_char((char)uch);
}

void app_uart_send(const char *msg)
{
    uart_send_str(msg);
}
