/**
 * @brief  UART 指令系统 — 接收 CA51F352P4 指令, 驱动屏幕
 *
 * CA51F352P4 TX → IO44 → UART1 RX
 * PC/调试输入也可走 UART0 RX；调试输出: printf → UART0 TX (IO43)
 *
 * UART0 与 UART1 共用同一命令行解析器，便于直接从 idf.py monitor 调试。
 *
 * 指令: VLIST / VPLAY / FIMGLIST / FIMG
 *       VIDLIST / VID / VPAUSE / VRESUME / VSTOP
 *       APLAY / ALIST / ASTOP / AMUTE / VOL / BL / SDLIST / IMGLIST / IMG
 *       RST / STATUS / INFO / SLEEP / WAKE
 */

#include "app_uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
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
#include "video_player.h"
#include "audio.h"
#include "audio_player.h"
#include "sd_browser.h"
#include "image_viewer.h"

#define TAG "uart"

/* ---- UART1 仅 RX ---- */
#define UART_PORT UART_NUM_1
#define UART_RX_PIN GPIO_NUM_44
#define UART_BAUD 115200
#define UART_BUF_SIZE 512
#define UART_LINE_SIZE 544
#define UART0_RX_PIN GPIO_NUM_44
#define UART0_TX_PIN GPIO_NUM_43

/* ---- 指令缓冲 ---- */
static char g_line[UART_LINE_SIZE];
static int g_pos = 0;
static bool s_uart0_input_ready;

/* ====== 内部 ====== */

/* printf → UART0 TX=IO43 (CA51 和电脑都收得到) */
static void uart_send_str(const char *msg)
{
    printf("%s\r\n", msg);
}

static bool is_flash_video_mode(void)
{
    return g_display_mode == DISPLAY_VIDEO_PLAYING ||
           g_display_mode == DISPLAY_VIDEO_PAUSED;
}

static bool is_sd_video_mode(void)
{
    return g_display_mode == DISPLAY_SD_VIDEO_PLAYING ||
           g_display_mode == DISPLAY_SD_VIDEO_PAUSED;
}

static void stop_display_video(void)
{
    if (is_flash_video_mode())
        flash_player_stop();
    else if (is_sd_video_mode())
        video_player_stop();
}

static void cmd_handle(const char *cmd)
{
    ESP_LOGI(TAG, "CMD: [%s]", cmd);

    /* === Flash AVI视频 === */
    if (strcasecmp(cmd, "VLIST") == 0)
    {
        char list[512];
        int count = flash_player_list_files(list, sizeof(list));
        if (count > 0)
        {
            uart_send_str("VLIST");
            uart_send_str(list);
        }
        else if (count == 0)
            uart_send_str("VLIST NONE");
        else
            uart_send_str("ERR flash media index");
        return;
    }
    if (strcasecmp(cmd, "VPLAY") == 0 ||
        strncasecmp(cmd, "VPLAY ", 6) == 0)
    {
        bool use_default = cmd[5] == '\0';
        const char *selection = use_default ? "1" : cmd + 6;
        while (*selection == ' ')
            selection++;
        if (!*selection)
        {
            uart_send_str("ERR usage: VPLAY [n or filename.avi]");
            return;
        }
        if (use_default && is_flash_video_mode())
        {
            char response[96];
            snprintf(response, sizeof(response), "OK VPLAY %s (already)",
                     flash_player_name());
            uart_send_str(response);
            return;
        }

        stop_display_video();
        if (g_display_mode == DISPLAY_IMAGE_LOADING)
            image_viewer_cancel();
        g_display_mode = DISPLAY_IDLE;

        esp_err_t ret = flash_player_start(selection);
        if (ret == ESP_OK)
        {
            g_display_mode = DISPLAY_VIDEO_PLAYING;
            spilcd_show_string(0, 0, 320, 16, 16,
                               "Video playing", BLACK);
            char response[96];
            snprintf(response, sizeof(response), "OK VPLAY %s",
                     flash_player_name());
            uart_send_str(response);
        }
        else
        {
            char response[64];
            snprintf(response, sizeof(response), "ERR VPLAY %s",
                     esp_err_to_name(ret));
            uart_send_str(response);
        }
        return;
    }
    if (strcasecmp(cmd, "VPAUSE") == 0)
    {
        if (g_display_mode == DISPLAY_VIDEO_PLAYING)
        {
            g_display_mode = DISPLAY_VIDEO_PAUSED;
            spilcd_show_string(0, 0, 320, 16, 16, "Video paused", BLACK);
            uart_send_str("OK VPAUSE");
        }
        else if (g_display_mode == DISPLAY_SD_VIDEO_PLAYING)
        {
            g_display_mode = DISPLAY_SD_VIDEO_PAUSED;
            uart_send_str("OK VPAUSE");
        }
        else
            uart_send_str("ERR not playing");
        return;
    }
    if (strcasecmp(cmd, "VRESUME") == 0)
    {
        if (g_display_mode == DISPLAY_VIDEO_PAUSED)
        {
            g_display_mode = DISPLAY_VIDEO_PLAYING;
            spilcd_show_string(0, 0, 320, 16, 16, "Video playing", BLACK);
            uart_send_str("OK VRESUME");
        }
        else if (g_display_mode == DISPLAY_SD_VIDEO_PAUSED)
        {
            g_display_mode = DISPLAY_SD_VIDEO_PLAYING;
            uart_send_str("OK VRESUME");
        }
        else
            uart_send_str("ERR not paused");
        return;
    }
    if (strcasecmp(cmd, "VSTOP") == 0)
    {
        if (is_flash_video_mode() || is_sd_video_mode())
        {
            stop_display_video();
            g_display_mode = DISPLAY_IDLE;
            spilcd_show_string(0, 0, 320, 16, 16, "Idle", BLACK);
            uart_send_str("OK VSTOP");
        }
        else
            uart_send_str("ERR no video");
        return;
    }

    /* === TF 卡 AVI 视频 === */
    if (strcasecmp(cmd, "VIDLIST") == 0)
    {
        char list[512];
        int count = video_player_list_files(list, sizeof(list));
        if (count > 0)
        {
            uart_send_str("VIDLIST");
            uart_send_str(list);
        }
        else if (count == 0)
            uart_send_str("VIDLIST NONE");
        else
            uart_send_str("ERR no SD card (VIDLIST failed)");
        return;
    }
    if (strcasecmp(cmd, "VID") == 0)
    {
        uart_send_str("ERR usage: VID <n> or VID <filename.avi>");
        return;
    }
    if (strncasecmp(cmd, "VID ", 4) == 0)
    {
        const char *arg = cmd + 4;
        while (*arg == ' ')
            arg++;
        if (!*arg)
        {
            uart_send_str("ERR usage: VID <n> or VID <filename.avi>");
            return;
        }

        stop_display_video();
        if (g_display_mode == DISPLAY_IMAGE_LOADING)
            image_viewer_cancel();
        g_display_mode = DISPLAY_IDLE;

        esp_err_t ret = video_player_start(arg);
        if (ret == ESP_OK)
        {
            g_display_mode = DISPLAY_SD_VIDEO_PLAYING;
            char response[UART_LINE_SIZE];
            snprintf(response, sizeof(response), "OK VID %s",
                     video_player_name());
            uart_send_str(response);
        }
        else
        {
            char response[64];
            snprintf(response, sizeof(response), "ERR VID %s",
                     esp_err_to_name(ret));
            uart_send_str(response);
        }
        return;
    }

    /* === TF 卡目录 === */
    if (strcasecmp(cmd, "SDLIST") == 0 || strncasecmp(cmd, "SDLIST ", 7) == 0)
    {
        int requested_page = 1;
        if (cmd[6] != '\0')
        {
            const char *argument = cmd + 7;
            char *end = NULL;
            long parsed_page = strtol(argument, &end, 10);
            while (end && *end == ' ')
                end++;
            if (argument == end || (end && *end != '\0') || parsed_page < 1 || parsed_page > INT_MAX)
            {
                uart_send_str("ERR usage: SDLIST [page]");
                return;
            }
            requested_page = (int)parsed_page;
        }

        stop_display_video();
        if (g_display_mode == DISPLAY_IMAGE_LOADING)
            image_viewer_cancel();
        g_display_mode = DISPLAY_IDLE;

        int shown_page = 1;
        int page_count = 1;
        int entry_count = 0;
        esp_err_t ret = sd_browser_show_page(requested_page, &shown_page,
                                             &page_count, &entry_count);
        if (ret == ESP_OK)
        {
            char response[64];
            snprintf(response, sizeof(response), "OK SDLIST page=%d/%d items=%d",
                     shown_page, page_count, entry_count);
            uart_send_str(response);
        }
        else
        {
            char response[64];
            snprintf(response, sizeof(response), "ERR SDLIST %s", esp_err_to_name(ret));
            uart_send_str(response);
        }
        return;
    }

    /* === Flash JPEG图片 === */
    if (strcasecmp(cmd, "FIMGLIST") == 0)
    {
        char list[512];
        int count = image_viewer_list_flash_files(list, sizeof(list));
        if (count > 0)
        {
            uart_send_str("FIMGLIST");
            uart_send_str(list);
        }
        else if (count == 0)
            uart_send_str("FIMGLIST NONE");
        else
            uart_send_str("ERR flash media index");
        return;
    }
    if (strcasecmp(cmd, "FIMG") == 0)
    {
        uart_send_str("ERR usage: FIMG <n> or FIMG <filename.jpg>");
        return;
    }
    if (strncasecmp(cmd, "FIMG ", 5) == 0)
    {
        const char *arg = cmd + 5;
        while (*arg == ' ')
            arg++;
        if (!*arg)
        {
            uart_send_str("ERR usage: FIMG <n> or FIMG <filename.jpg>");
            return;
        }
        stop_display_video();
        if (g_display_mode == DISPLAY_IMAGE_LOADING)
            image_viewer_cancel();
        g_display_mode = DISPLAY_IDLE;
        esp_err_t ret = image_viewer_start_flash(arg);
        if (ret == ESP_OK)
        {
            g_display_mode = DISPLAY_IMAGE_LOADING;
            uart_send_str("OK FIMG loading");
        }
        else
        {
            char response[64];
            snprintf(response, sizeof(response), "ERR FIMG %s",
                     esp_err_to_name(ret));
            uart_send_str(response);
        }
        return;
    }

    /* === TF卡JPEG图片 === */
    if (strcasecmp(cmd, "IMGLIST") == 0)
    {
        char list[512];
        int count = image_viewer_list_files(list, sizeof(list));
        if (count > 0)
        {
            uart_send_str("IMGLIST");
            uart_send_str(list);
        }
        else if (count == 0)
            uart_send_str("IMGLIST NONE");
        else
            uart_send_str("ERR no SD card (IMGLIST failed)");
        return;
    }
    if (strcasecmp(cmd, "IMG") == 0)
    {
        uart_send_str("ERR usage: IMG <n> or IMG <filename.jpg>");
        return;
    }
    if (strncasecmp(cmd, "IMG ", 4) == 0)
    {
        const char *arg = cmd + 4;
        while (*arg == ' ')
            arg++;
        if (!*arg)
        {
            uart_send_str("ERR usage: IMG <n> or IMG <filename.jpg>");
            return;
        }
        stop_display_video();
        if (g_display_mode == DISPLAY_IMAGE_LOADING)
            image_viewer_cancel();
        g_display_mode = DISPLAY_IDLE;
        esp_err_t ret = image_viewer_start(arg);
        if (ret == ESP_OK)
        {
            g_display_mode = DISPLAY_IMAGE_LOADING;
            uart_send_str("OK IMG loading");
        }
        else
        {
            char response[64];
            snprintf(response, sizeof(response), "ERR IMG %s",
                     esp_err_to_name(ret));
            uart_send_str(response);
        }
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

        if (!audio_is_ready())
        {
            uart_send_str("ERR audio codec not ready");
        }
        else if (audio_player_start(arg) == ESP_OK)
        {
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
        if (audio_player_is_active())
        {
            audio_player_stop();
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
        if (v < 0)
            v = 0;
        if (v > 100)
            v = 100;
        esp_err_t ret = audio_player_set_volume(v);
        if (ret == ESP_OK)
        {
            char r[32];
            snprintf(r, sizeof(r), "OK VOL %d", v);
            uart_send_str(r);
        }
        else
        {
            char r[48];
            snprintf(r, sizeof(r), "ERR VOL %s", esp_err_to_name(ret));
            uart_send_str(r);
        }
        return;
    }

    /* === LCD 背光亮度 (V1.4 GPIO1 PWM, 0=灭, 100=全亮) === */
    if (strcasecmp(cmd, "BL") == 0)
    {
        uart_send_str("ERR usage: BL <0-100>");
        return;
    }
    if (strncasecmp(cmd, "BL ", 3) == 0)
    {
        char *end = NULL;
        long value = strtol(cmd + 3, &end, 10);
        while (end && (*end == ' ' || *end == '\t'))
            end++;
        if (end == cmd + 3 || (end && *end != '\0'))
        {
            uart_send_str("ERR usage: BL <0-100>");
            return;
        }
        if (value < 0 || value > 100)
        {
            uart_send_str("ERR BL range 0-100");
            return;
        }
        spilcd_backlight_set((uint8_t)value);
        char response[32];
        snprintf(response, sizeof(response), "OK BL %ld", value);
        uart_send_str(response);
        return;
    }

    /* === GPIO5 手动控制 === */
    if (strcasecmp(cmd, "GPIO5") == 0 || strcasecmp(cmd, "G5") == 0)
    {
        gpio_config_t cfg = {
            .pin_bit_mask = BIT64(5),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        int lev = gpio_get_level(GPIO_NUM_5);
        gpio_set_level(GPIO_NUM_5, !lev);
        char r[32];
        snprintf(r, sizeof(r), "GPIO5: %d -> %d", lev, !lev);
        uart_send_str(r);
        return;
    }
    if (strncasecmp(cmd, "GPIO5 ", 6) == 0)
    {
        int v = atoi(cmd + 6);
        v = (v != 0);
        gpio_config_t cfg = {
            .pin_bit_mask = BIT64(5),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(GPIO_NUM_5, v);
        char r[32];
        snprintf(r, sizeof(r), "GPIO5 set %d, readback=%d",
                 v, gpio_get_level(GPIO_NUM_5));
        uart_send_str(r);
        return;
    }
    if (strncasecmp(cmd, "GPIO4 ", 6) == 0)
    {
        int v = atoi(cmd + 6);
        v = (v != 0);
        gpio_config_t cfg = {
            .pin_bit_mask = BIT64(4),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
        gpio_set_level(GPIO_NUM_4, v);
        char r[32];
        snprintf(r, sizeof(r), "GPIO4 set %d, readback=%d",
                 v, gpio_get_level(GPIO_NUM_4));
        uart_send_str(r);
        return;
    }

    /* === I2C 总线测试/恢复 (示波器诊断) === */
    if (strcasecmp(cmd, "I2CTEST") == 0)
    {
        if (audio_i2c_test_is_running())
            audio_i2c_test_stop();
        else
            audio_i2c_test_start();
        return;
    }
    if (strcasecmp(cmd, "I2CFIX") == 0)
    {
        audio_i2c_bus_recover();
        return;
    }

    /* === 系统控制 === */
    if (strcasecmp(cmd, "RST") == 0)
    {
        uart_send_str("OK RST");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    if (strcasecmp(cmd, "STATUS") == 0)
    {
        const char *display = "idle";
        if (g_display_mode == DISPLAY_VIDEO_PLAYING)
            display = "video_playing";
        else if (g_display_mode == DISPLAY_VIDEO_PAUSED)
            display = "video_paused";
        else if (g_display_mode == DISPLAY_SD_VIDEO_PLAYING)
            display = "sd_video_playing";
        else if (g_display_mode == DISPLAY_SD_VIDEO_PAUSED)
            display = "sd_video_paused";
        else if (g_display_mode == DISPLAY_IMAGE_LOADING)
            display = "image_loading";
        else if (g_display_mode == DISPLAY_SLEEP)
            display = "sleep";
        const char *af = audio_player_current_file();
        char r[UART_LINE_SIZE];
        if (af)
            snprintf(r, sizeof(r), "STATUS display=%s audio=%s",
                     display, af);
        else
            snprintf(r, sizeof(r), "STATUS display=%s audio=stopped",
                     display);
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
        stop_display_video();
        if (audio_player_is_active())
            audio_player_stop();
        if (g_display_mode == DISPLAY_IMAGE_LOADING)
            image_viewer_cancel();
        g_display_mode = DISPLAY_SLEEP;
        spilcd_clear(BLACK);
        spilcd_show_string(40, 140, 280, 170, 16, "Sleep", BLACK);
        uart_send_str("OK SLEEP");
        return;
    }
    if (strcasecmp(cmd, "WAKE") == 0)
    {
        if (g_display_mode == DISPLAY_SLEEP)
        {
            g_display_mode = DISPLAY_IDLE;
            spilcd_clear(BLACK);
            spilcd_show_string(40, 140, 280, 170, 16, "ESP32-S3 Eye", BLUE);
        }
        uart_send_str("OK WAKE");
        return;
    }

    /* unknown */
    char r[64];
    snprintf(r, sizeof(r), "ERR unknown: %.50s", cmd);
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

    /* UART0 控制台在部分 ESP-IDF 配置中只使用 ROM/低层输出，并未安装
     * UART 驱动。安装后才能安全调用 uart_read_bytes() 接收 monitor 输入。 */
    if (!uart_is_driver_installed(UART_NUM_0))
    {
        esp_err_t ret = uart_driver_install(
            UART_NUM_0, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0);
        if (ret == ESP_OK)
        {
            uart_param_config(UART_NUM_0, &cfg);
            uart_set_pin(UART_NUM_0, UART0_TX_PIN, UART0_RX_PIN,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
            s_uart0_input_ready = true;
        }
        else
        {
            ESP_LOGW(TAG, "UART0 input unavailable: %s",
                     esp_err_to_name(ret));
        }
    }
    else
    {
        s_uart0_input_ready = true;
    }

    // /* TODO: USB-serial-JTAG 启用后设 stdin 为非阻塞 */
    // fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    ESP_LOGI(TAG, "UART0 RX + UART1 RX=IO%d @%d (UART0 input=%s)",
             UART_RX_PIN, UART_BAUD, s_uart0_input_ready ? "ready" : "off");
}

void app_uart_tick(void)
{
    /* UART0 与 UART1 当前都映射到 GPIO44。UART0 驱动可用时优先读取
     * UART0；随后丢弃 UART1 中同一物理信号产生的镜像字节，避免每条
     * 命令执行两次。若 UART0 不可用，则回退到 UART1。 */
    uint8_t uch;
    if (s_uart0_input_ready)
    {
        while (uart_read_bytes(UART_NUM_0, &uch, 1, 0) > 0)
            feed_char((char)uch);
        /* UART1 仍由外部控制器兼容保留，但 GPIO44 上的数据已由 UART0
         * 消费；清空其接收 FIFO，防止长期运行后溢出。 */
        if (UART_PORT != UART_NUM_0)
            uart_flush_input(UART_PORT);
    }
    else
    {
        while (uart_read_bytes(UART_PORT, &uch, 1, 0) > 0)
            feed_char((char)uch);
    }
}

void app_uart_send(const char *msg)
{
    uart_send_str(msg);
}

void app_uart_inject(const char *cmd)
{
    /* 注入指令到缓冲区, 下一个 tick 自动解析 */
    size_t len = strlen(cmd);
    if (len >= sizeof(g_line))
        len = sizeof(g_line) - 1;
    memcpy(g_line, cmd, len);
    g_pos = len;
}
