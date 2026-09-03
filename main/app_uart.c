/**
 * @brief  UART 指令系统 — 接收 CA51F352P4 指令, 驱动屏幕
 *
 * CA51F352P4 TX → IO44 → UART1 RX；UART1 TX IO43 → CA51F352P4 RX
 * PC/调试输入输出走原生 USB Serial-JTAG。
 *
 * UART1 与 USB 使用独立缓冲并共用同一命令解析器。
 *
 * 指令: VLIST / VPLAY / FIMGLIST / FIMG
 *       VIDLIST / VID / VPAUSE / VRESUME / VSTOP
 *       APLAY / ALIST / ASTOP / AMUTE / VOL / BL / SDLIST / IMGLIST / IMG
 *       ENC / RST / STATUS / INFO / SLEEP / WAKE
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
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ff.h"
#include "spilcd.h"
#include "flash_player.h"
#include "video_player.h"
#include "audio.h"
#include "audio_player.h"
#include "sd_browser.h"
#include "sd_card.h"
#include "image_viewer.h"
#include "app_settings.h"

#define TAG "uart"

/* ---- UART1 与 CA51 双向通信 ---- */
#define UART_PORT UART_NUM_1
#define UART_RX_PIN GPIO_NUM_44
#define UART_BAUD 115200
#define UART_BUF_SIZE 512
#define UART_LINE_SIZE 544
#define UART_ENCODED_LINE_SIZE (UART_LINE_SIZE * 2 + 32)
#define UART_TX_PIN GPIO_NUM_43
#define CA51_FORWARD_QUEUE_LEN 4
#define CA51_FORWARD_PREFIX "CA51 "
#define CA51_FORWARD_LINE_SIZE (UART_ENCODED_LINE_SIZE + 8)

/* ---- 指令缓冲 ---- */
static char g_line[UART_LINE_SIZE];
static int g_pos = 0;
static char g_usb_line[UART_LINE_SIZE];
static int g_usb_pos = 0;
static bool s_usb_input_ready;
static uint8_t s_sleep_backlight = 100;
/* CA51->JTAG 转发默认开启；通过 JTAG 的 CA51FWD ON/OFF 临时切换。 */
static bool s_ca51_forward_enabled = true;
typedef struct
{
    char data[CA51_FORWARD_LINE_SIZE];
    size_t length;
    size_t offset;
    uint16_t stalled_ticks;
} ca51_forward_item_t;

/* UART1 接收到的 CA51 命令通过有界队列异步转发到 USB JTAG。
 * 主循环只入队/每 tick 发送一小块，避免 USB 写入阻塞 UART 和播放器。 */
static ca51_forward_item_t s_ca51_forward_queue[CA51_FORWARD_QUEUE_LEN];
static size_t s_ca51_forward_head;
static size_t s_ca51_forward_tail;
static size_t s_ca51_forward_count;
typedef enum { CMD_SOURCE_UART1, CMD_SOURCE_USB, CMD_SOURCE_BOTH } cmd_source_t;
static cmd_source_t s_cmd_source = CMD_SOURCE_BOTH;
/* 异步媒体结果（IMG/VID 完成或失败）返回到最近一次启动该媒体的链路。 */
static cmd_source_t s_async_source = CMD_SOURCE_USB;

typedef enum
{
    UART_ENCODING_GBK,
    UART_ENCODING_UTF8,
} uart_encoding_t;

/* CA51沿用移交协议的GBK；电脑终端默认使用UTF-8。 */
static uart_encoding_t s_uart1_encoding = UART_ENCODING_GBK;
static uart_encoding_t s_usb_encoding = UART_ENCODING_UTF8;

static bool sd_card_unavailable(void)
{
    return !sd_card_is_mounted() || sd_card_probe() != ESP_OK;
}

/* ====== 内部 ====== */

static const char *encoding_name(uart_encoding_t encoding)
{
    return encoding == UART_ENCODING_UTF8 ? "UTF8" : "GBK";
}

static uart_encoding_t *current_output_encoding(void)
{
    cmd_source_t source = s_cmd_source == CMD_SOURCE_BOTH
                              ? s_async_source
                              : s_cmd_source;
    if (source == CMD_SOURCE_UART1)
        return &s_uart1_encoding;
    return &s_usb_encoding;
}

static size_t append_utf8(uint32_t codepoint, char *output,
                          size_t used, size_t output_size)
{
    size_t needed = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 : 3;
    if (used + needed >= output_size)
        return used;

    if (needed == 1)
    {
        output[used++] = (char)codepoint;
    }
    else if (needed == 2)
    {
        output[used++] = (char)(0xC0 | (codepoint >> 6));
        output[used++] = (char)(0x80 | (codepoint & 0x3F));
    }
    else
    {
        output[used++] = (char)(0xE0 | (codepoint >> 12));
        output[used++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        output[used++] = (char)(0x80 | (codepoint & 0x3F));
    }
    return used;
}

/* FATFS ANSI/OEM API返回CP936/GBK字节；转换为电脑终端使用的UTF-8。 */
static size_t gbk_to_utf8(const char *input, char *output, size_t output_size)
{
    const uint8_t *source = (const uint8_t *)input;
    size_t used = 0;

    if (output_size == 0)
        return 0;

    while (*source)
    {
        uint32_t codepoint;
        if (*source < 0x80)
        {
            codepoint = *source++;
        }
        else if (source[1] != 0)
        {
            WCHAR gbk = (WCHAR)(((WCHAR)source[0] << 8) | source[1]);
            codepoint = ff_oem2uni(gbk, 936);
            source += 2;
            if (codepoint == 0)
                codepoint = 0xFFFD;
        }
        else
        {
            source++;
            codepoint = 0xFFFD;
        }

        size_t next = append_utf8(codepoint, output, used, output_size);
        if (next == used && codepoint != 0)
            break;
        used = next;
    }
    output[used] = '\0';
    return used;
}

static bool decode_utf8_char(const uint8_t **source, uint32_t *codepoint)
{
    const uint8_t *s = *source;
    if (s[0] < 0x80)
    {
        *codepoint = s[0];
        *source = s + 1;
        return true;
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF &&
        s[1] >= 0x80 && s[1] <= 0xBF)
    {
        *codepoint = ((uint32_t)(s[0] & 0x1F) << 6) |
                     (uint32_t)(s[1] & 0x3F);
        *source = s + 2;
        return true;
    }
    if (s[0] >= 0xE0 && s[0] <= 0xEF && s[1] != 0 && s[2] != 0 &&
        s[1] >= 0x80 && s[1] <= 0xBF &&
        s[2] >= 0x80 && s[2] <= 0xBF &&
        !(s[0] == 0xE0 && s[1] < 0xA0) &&
        !(s[0] == 0xED && s[1] >= 0xA0))
    {
        *codepoint = ((uint32_t)(s[0] & 0x0F) << 12) |
                     ((uint32_t)(s[1] & 0x3F) << 6) |
                     (uint32_t)(s[2] & 0x3F);
        *source = s + 3;
        return true;
    }

    *codepoint = '?';
    *source = s + 1;
    return false;
}

static size_t utf8_to_gbk(const char *input, char *output, size_t output_size)
{
    const uint8_t *source = (const uint8_t *)input;
    size_t used = 0;

    if (output_size == 0)
        return 0;

    while (*source)
    {
        uint32_t codepoint;
        decode_utf8_char(&source, &codepoint);
        WCHAR gbk = codepoint < 0x80 ? (WCHAR)codepoint
                                     : ff_uni2oem(codepoint, 936);
        if (gbk == 0)
            gbk = '?';

        size_t needed = gbk > 0xFF ? 2 : 1;
        if (used + needed >= output_size)
            break;
        if (needed == 2)
            output[used++] = (char)(gbk >> 8);
        output[used++] = (char)(gbk & 0xFF);
    }
    output[used] = '\0';
    return used;
}

static size_t make_response_line(const char *message,
                                 uart_encoding_t source_encoding,
                                 uart_encoding_t output_encoding,
                                 char *line, size_t line_size)
{
    if (line_size < 3)
        return 0;

    size_t used;
    if (source_encoding == output_encoding)
    {
        used = strlen(message);
        if (used > line_size - 3)
            used = line_size - 3;
        memcpy(line, message, used);
        line[used] = '\0';
    }
    else if (output_encoding == UART_ENCODING_UTF8)
    {
        used = gbk_to_utf8(message, line, line_size - 2);
    }
    else
    {
        used = utf8_to_gbk(message, line, line_size - 2);
    }

    line[used++] = '\r';
    line[used++] = '\n';
    line[used] = '\0';
    return used;
}

static void ca51_forward_enqueue(const char *message)
{
    if (!message || !s_usb_input_ready || !s_ca51_forward_enabled)
        return;

    if (s_ca51_forward_count >= CA51_FORWARD_QUEUE_LEN)
    {
        ESP_LOGW(TAG, "CA51->JTAG forward queue full; message dropped");
        return;
    }

    char encoded[UART_ENCODED_LINE_SIZE];
    size_t encoded_length = make_response_line(
        message, s_uart1_encoding, s_usb_encoding,
        encoded, sizeof(encoded));
    size_t prefix_length = sizeof(CA51_FORWARD_PREFIX) - 1;
    if (encoded_length == 0 ||
        prefix_length + encoded_length > CA51_FORWARD_LINE_SIZE)
    {
        ESP_LOGW(TAG, "CA51->JTAG message too long; message dropped");
        return;
    }

    ca51_forward_item_t *item = &s_ca51_forward_queue[s_ca51_forward_tail];
    memcpy(item->data, CA51_FORWARD_PREFIX, prefix_length);
    memcpy(item->data + prefix_length, encoded, encoded_length);
    item->length = prefix_length + encoded_length;
    item->offset = 0;
    item->stalled_ticks = 0;

    s_ca51_forward_tail =
        (s_ca51_forward_tail + 1) % CA51_FORWARD_QUEUE_LEN;
    s_ca51_forward_count++;
}

static void ca51_forward_clear_queue(void)
{
    s_ca51_forward_head = 0;
    s_ca51_forward_tail = 0;
    s_ca51_forward_count = 0;
}

static void ca51_forward_flush(void)
{
    if (!s_usb_input_ready || s_ca51_forward_count == 0)
        return;

    ca51_forward_item_t *item = &s_ca51_forward_queue[s_ca51_forward_head];
    size_t remaining = item->length - item->offset;
    size_t chunk = remaining > 64 ? 64 : remaining;
    int written = usb_serial_jtag_write_bytes(
        (const uint8_t *)item->data + item->offset, chunk, 0);
    if (written <= 0)
    {
        /* 非阻塞发送：USB TX 缓冲暂满时留到下一次 tick，超过约 1 秒丢弃。 */
        if (++item->stalled_ticks >= 200)
        {
            ESP_LOGW(TAG, "CA51->JTAG forward timeout; message dropped");
            s_ca51_forward_head =
                (s_ca51_forward_head + 1) % CA51_FORWARD_QUEUE_LEN;
            s_ca51_forward_count--;
        }
        return;
    }

    item->offset += (size_t)written;
    item->stalled_ticks = 0;
    if (item->offset >= item->length)
    {
        s_ca51_forward_head =
            (s_ca51_forward_head + 1) % CA51_FORWARD_QUEUE_LEN;
        s_ca51_forward_count--;
    }
}

static void usb_write_all(const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length)
    {
        size_t remaining = length - sent;
        size_t chunk = remaining > 64 ? 64 : remaining;
        int written = usb_serial_jtag_write_bytes(
            (const uint8_t *)data + sent, chunk, pdMS_TO_TICKS(100));
        if (written <= 0)
        {
            ESP_LOGW(TAG, "USB response truncated (%u/%u bytes)",
                     (unsigned)sent, (unsigned)length);
            break;
        }
        sent += (size_t)written;
    }
}

static void uart_send_encoded(const char *msg, uart_encoding_t source_encoding)
{
    cmd_source_t output_source = s_cmd_source == CMD_SOURCE_BOTH
                                     ? s_async_source
                                     : s_cmd_source;
    uart_encoding_t output_encoding = output_source == CMD_SOURCE_UART1
                                          ? s_uart1_encoding
                                          : s_usb_encoding;
    char line[UART_ENCODED_LINE_SIZE];
    size_t length = make_response_line(msg, source_encoding, output_encoding,
                                       line, sizeof(line));
    if (length == 0)
        return;
    if (output_source == CMD_SOURCE_UART1)
        uart_write_bytes(UART_PORT, line, length); /* UART1 TX=GPIO43 -> CA51 RX */
    else if (output_source == CMD_SOURCE_USB && s_usb_input_ready)
    {
        usb_write_all("JTAG ", 5);
        usb_write_all(line, length);
    }
    else
    {
        /* USB 驱动不可用时保留日志输出，避免丢失调试信息。 */
        printf("%s", line);
    }
}

/* 固件常量与 Flash 媒体索引名称使用 UTF-8。 */
static void uart_send_str(const char *msg)
{
    uart_send_encoded(msg, UART_ENCODING_UTF8);
}

/* FATFS ANSI/OEM API 返回的媒体路径使用 CP936/GBK。 */
static void uart_send_gbk_str(const char *msg)
{
    uart_send_encoded(msg, UART_ENCODING_GBK);
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

static bool sleep_command_allowed(const char *cmd)
{
    return strcasecmp(cmd, "WAKE") == 0 ||
           strcasecmp(cmd, "SLEEP") == 0 ||
           strcasecmp(cmd, "STATUS") == 0 ||
           strcasecmp(cmd, "INFO") == 0 ||
           strcasecmp(cmd, "ENC") == 0 ||
           strcasecmp(cmd, "ENC?") == 0 ||
           strcasecmp(cmd, "ENC UTF8") == 0 ||
           strcasecmp(cmd, "ENC UTF-8") == 0 ||
           strcasecmp(cmd, "ENC GBK") == 0 ||
           strcasecmp(cmd, "BL") == 0 ||
           strcasecmp(cmd, "BL?") == 0 ||
           strncasecmp(cmd, "BL ", 3) == 0 ||
           strcasecmp(cmd, "CA51FWD") == 0 ||
           strcasecmp(cmd, "CA51FWD?") == 0 ||
           strncasecmp(cmd, "CA51FWD ", 8) == 0 ||
           strcasecmp(cmd, "RST") == 0;
}

static void cmd_handle(const char *cmd)
{

    /* CA51->JTAG 转发开关只允许电脑 JTAG 配置，避免 CA51 改变调试链路。 */
    if (strcasecmp(cmd, "CA51FWD") == 0 ||
        strcasecmp(cmd, "CA51FWD?") == 0)
    {
        if (s_cmd_source != CMD_SOURCE_USB)
        {
            uart_send_str("ERR CA51FWD JTAG ONLY");
            return;
        }
        uart_send_str(s_ca51_forward_enabled
                          ? "OK CA51FWD ON"
                          : "OK CA51FWD OFF");
        return;
    }
    if (strncasecmp(cmd, "CA51FWD ", 8) == 0)
    {
        if (s_cmd_source != CMD_SOURCE_USB)
        {
            uart_send_str("ERR CA51FWD JTAG ONLY");
            return;
        }
        const char *mode = cmd + 8;
        if (strcasecmp(mode, "ON") == 0)
        {
            s_ca51_forward_enabled = true;
            uart_send_str("OK CA51FWD ON");
        }
        else if (strcasecmp(mode, "OFF") == 0)
        {
            s_ca51_forward_enabled = false;
            ca51_forward_clear_queue();
            uart_send_str("OK CA51FWD OFF");
        }
        else
        {
            uart_send_str("ERR usage: CA51FWD ON|OFF");
        }
        return;
    }

    /* feed_char() 已将 CA51 UART0 调试行转发到 USB JTAG。
     * 此处只终止业务解析，不向 CA51 回传 ERR unknown。 */
    if (strncmp(cmd, "DBG ", 4) == 0)
        return;

    if (g_display_mode == DISPLAY_SLEEP && !sleep_command_allowed(cmd))
    {
        uart_send_str("ERR SLEEP");
        return;
    }

    ESP_LOGI(TAG, "%s CMD: [%s]",
             s_cmd_source == CMD_SOURCE_UART1 ? "UART1" : "JTAG", cmd);

    /* === 当前链路的响应文本编码 === */
    if (strcasecmp(cmd, "ENC") == 0 || strcasecmp(cmd, "ENC?") == 0)
    {
        char response[24];
        snprintf(response, sizeof(response), "OK ENC %s",
                 encoding_name(*current_output_encoding()));
        uart_send_str(response);
        return;
    }
    if (strcasecmp(cmd, "ENC UTF8") == 0 ||
        strcasecmp(cmd, "ENC UTF-8") == 0)
    {
        *current_output_encoding() = UART_ENCODING_UTF8;
        uart_send_str("OK ENC UTF8");
        return;
    }
    if (strcasecmp(cmd, "ENC GBK") == 0)
    {
        *current_output_encoding() = UART_ENCODING_GBK;
        uart_send_str("OK ENC GBK");
        return;
    }
    if (strncasecmp(cmd, "ENC ", 4) == 0)
    {
        uart_send_str("ERR usage: ENC UTF8|GBK");
        return;
    }

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
            s_async_source = s_cmd_source;
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
            char response[UART_LINE_SIZE + 16];
            snprintf(response, sizeof(response), "VIDLIST\r\n%s", list);
            uart_send_gbk_str(response);
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
            s_async_source = s_cmd_source;
            g_display_mode = DISPLAY_SD_VIDEO_PLAYING;
            char response[UART_LINE_SIZE];
            snprintf(response, sizeof(response), "OK VID %s",
                     video_player_name());
            uart_send_gbk_str(response);
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
            s_async_source = s_cmd_source;
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
            uart_send_gbk_str(list);
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

        /* IMG 访问 TF 前先探测一次。无卡或拔卡后的短窗口内，不把
         * INSERT CARD/READ FAILED 等字体错误画到 LCD，而是统一显示
         * Flash 中的 SDCard.jpg。 */
        if (sd_card_unavailable())
        {
            if (app_uart_start_sd_error_image())
            {
                uart_send_str("OK FIMG loading");
                return;
            }
            uart_send_str("ERR IMG SD_FALLBACK");
            return;
        }

        esp_err_t ret = image_viewer_start(arg);
        if (ret == ESP_OK)
        {
            s_async_source = s_cmd_source;
            g_display_mode = DISPLAY_IMAGE_LOADING;
            uart_send_str("OK IMG loading");
        }
        else
        {
            /* 卡可能在目录扫描或打开文件的瞬间被拔出。再次确认后
             * 仍按 SD 卡异常处理，避免显示普通 IMAGE ERROR。 */
            if (sd_card_unavailable() && app_uart_start_sd_error_image())
            {
                uart_send_str("OK FIMG loading");
                return;
            }
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
            uart_send_gbk_str(list);
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
        char *end = NULL;
        long parsed = strtol(cmd + 4, &end, 10);
        while (end && (*end == ' ' || *end == '\t'))
            end++;
        if (end == cmd + 4 || (end && *end != '\0'))
        {
            uart_send_str("ERR usage: VOL <0-100>");
            return;
        }
        int v = (parsed < INT_MIN) ? INT_MIN :
                (parsed > INT_MAX) ? INT_MAX : (int)parsed;
        if (v < 0)
            v = 0;
        if (v > 100)
            v = 100;
        esp_err_t ret = audio_player_set_volume(v);
        if (ret == ESP_OK)
        {
            esp_err_t save_ret = app_settings_save_volume((uint8_t)v);
            if (save_ret != ESP_OK)
                ESP_LOGW(TAG, "Volume applied but not saved: %s",
                         esp_err_to_name(save_ret));
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
    if (strcasecmp(cmd, "BL") == 0 || strcasecmp(cmd, "BL?") == 0)
    {
        char response[32];
        uint8_t brightness = g_display_mode == DISPLAY_SLEEP
                                 ? s_sleep_backlight
                                 : spilcd_backlight_get();
        snprintf(response, sizeof(response), "OK BL %u", brightness);
        uart_send_str(response);
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
        if (g_display_mode == DISPLAY_SLEEP)
            s_sleep_backlight = (uint8_t)value;
        else
            spilcd_backlight_set((uint8_t)value);
        esp_err_t save_ret = app_settings_save_backlight((uint8_t)value);
        if (save_ret != ESP_OK)
            ESP_LOGW(TAG, "Backlight applied but not saved: %s",
                     esp_err_to_name(save_ret));
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
            .mode = GPIO_MODE_INPUT_OUTPUT,
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
            .mode = GPIO_MODE_INPUT_OUTPUT,
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
            .mode = GPIO_MODE_INPUT_OUTPUT,
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
            snprintf(r, sizeof(r), "STATUS display=%s audio=%s backlight=%u",
                     display, af, spilcd_backlight_get());
        else
            snprintf(r, sizeof(r), "STATUS display=%s audio=stopped backlight=%u",
                     display, spilcd_backlight_get());
        uart_send_gbk_str(r);
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
        if (g_display_mode != DISPLAY_SLEEP)
            s_sleep_backlight = spilcd_backlight_get();
        spilcd_backlight_set(0);
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
            spilcd_backlight_set(s_sleep_backlight);
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

bool app_uart_start_sd_error_image(void)
{
    /* 在命令上下文中记录原始链路；异步完成/失败响应仍回到该链路。
     * 主循环调用时 s_cmd_source 为 BOTH，因此保留原有 s_async_source。 */
    if (s_cmd_source != CMD_SOURCE_BOTH)
        s_async_source = s_cmd_source;

    stop_display_video();
    if (g_display_mode == DISPLAY_IMAGE_LOADING)
        image_viewer_cancel();
    g_display_mode = DISPLAY_IDLE;

    esp_err_t ret = image_viewer_start_flash("SDCard.jpg");
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Cannot start SD fallback image: %s",
                 esp_err_to_name(ret));
        return false;
    }
    g_display_mode = DISPLAY_IMAGE_LOADING;
    return true;
}

/* ---- 单字符指令解析 ---- */
static void feed_char(char ch, char *line, int *pos)
{
    if (ch == '\n' || ch == '\r')
    {
        if (*pos > 0)
        {
            line[*pos] = 0;
            while (*pos > 0 && line[*pos - 1] == ' ')
                line[--(*pos)] = 0;
            if (*pos > 0) {
                cmd_source_t previous = s_cmd_source;
                s_cmd_source = (line == g_usb_line) ? CMD_SOURCE_USB : CMD_SOURCE_UART1;
                if (s_cmd_source == CMD_SOURCE_UART1)
                    ca51_forward_enqueue(line);
                cmd_handle(line);
                s_cmd_source = previous;
            }
            *pos = 0;
        }
    }
    else if (*pos < UART_LINE_SIZE - 1)
    {
        line[(*pos)++] = ch;
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
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* UART0 控制台在部分 ESP-IDF 配置中只使用 ROM/低层输出，并未安装
     * UART 驱动。安装后才能安全调用 uart_read_bytes() 接收 monitor 输入。 */
    #if 0
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

    /* 原生 Type-C 的 USB Serial-JTAG 通道。若 ESP-IDF 控制台尚未安装
     * 驱动，则由业务 UART 初始化主动安装一个非阻塞驱动。 */
    #endif
    if (!usb_serial_jtag_is_driver_installed())
    {
        usb_serial_jtag_driver_config_t usb_cfg =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t ret = usb_serial_jtag_driver_install(&usb_cfg);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "USB Serial-JTAG input unavailable: %s",
                     esp_err_to_name(ret));
        }
        else
        {
            s_usb_input_ready = true;
        }
    }
    else
    {
        s_usb_input_ready = true;
    }

    // /* TODO: USB-serial-JTAG 启用后设 stdin 为非阻塞 */
    // fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    ESP_LOGI(TAG, "UART1 RX=IO%d @%d + USB-JTAG (independent, %s)",
             UART_RX_PIN, UART_BAUD, s_usb_input_ready ? "ready" : "off");
}

void app_uart_tick(void)
{
    /* UART0 与 UART1 当前都映射到 GPIO44。UART0 驱动可用时优先读取
     * UART0；随后丢弃 UART1 中同一物理信号产生的镜像字节，避免每条
     * 命令执行两次。若 UART0 不可用，则回退到 UART1。 */
    uint8_t uch;
    if (false)
    {
        while (uart_read_bytes(UART_NUM_0, &uch, 1, 0) > 0)
            feed_char((char)uch, g_line, &g_pos);
        /* UART1 仍由外部控制器兼容保留，但 GPIO44 上的数据已由 UART0
         * 消费；清空其接收 FIFO，防止长期运行后溢出。 */
        if (UART_PORT != UART_NUM_0)
            uart_flush_input(UART_PORT);
    }
    else
    {
        while (uart_read_bytes(UART_PORT, &uch, 1, 0) > 0)
            feed_char((char)uch, g_line, &g_pos);
    }

    /* 原生 Type-C USB Serial-JTAG 输入 */
    if (s_usb_input_ready)
    {
        while (usb_serial_jtag_read_bytes(&uch, 1, 0) > 0)
            feed_char((char)uch, g_usb_line, &g_usb_pos);
    }

    /* 每次只发送一个小块，保证 CA51 转发不会阻塞主任务分片循环。 */
    ca51_forward_flush();
}

void app_uart_send(const char *msg)
{
    uart_send_str(msg);
}

void app_uart_send_gbk(const char *msg)
{
    uart_send_gbk_str(msg);
}

void app_uart_inject(const char *cmd)
{
    /* 注入指令也必须经过换行触发的同一解析路径，不能只写入缓冲区。 */
    if (!cmd)
        return;
    size_t len = strnlen(cmd, sizeof(g_line) - 2);
    memcpy(g_line, cmd, len);
    g_line[len] = '\0';
    g_pos = (int)len;
    feed_char('\n', g_line, &g_pos);
}
