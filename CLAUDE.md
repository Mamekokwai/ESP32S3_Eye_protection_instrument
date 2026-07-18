# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ESP32-S3-WROOM-1 (YT06 主板 V1.1) 的 ESP-IDF v5.4.4 视频播放项目。
硬件：**双 320×320 圆屏** (8080 并口, CS1/CS2 控制, 同画面)、ES8311 音频 DAC、
CA51F352P4 触控 MCU、TF 卡 SPI、16MB Flash + 8MB PSRAM。

## 构建命令

```bash
source /home/nywerya/esp/v5.4.4/esp-idf/export.fish
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

首次烧录需全量 `idf.py flash`。

## 硬件引脚 (YT06 V1.1)

| 功能 | GPIO | 备注 |
|---|---|---|
| **LCD 8080 并口 ×2** | | 双 320×320 圆屏, 共享数据总线 |
| DB0-DB7 | IO6-IO13 | 8-bit 数据总线 (两屏共享) |
| WR | IO1 | 写信号 (两屏共享) |
| D/C | IO38 | 数据/命令 (两屏共享) |
| CS1 | IO17 | 左眼屏片选 |
| CS2 | IO18 | 右眼屏片选 |
| TE | IO48 | Tearing Effect (两屏共享) |
| RESET | IO2 | LCD 复位 (两屏共享) |
| 同步方式 | CS1+CS2 同时拉低 | 写命令/数据, 两屏同画面 |
| **SD 卡 SPI** | | 独立总线, 不共享 |
| CLK | IO21 | |
| MOSI | IO47 | R21 10K 上拉 |
| MISO | IO14 | |
| CS | IO0 | R20 10K 上拉 |
| **ES8311 I2C** | SDA=IO4, SCL=IO5 | |
| **ES8311 I2S** | MCLK=IO45, BCLK=IO39, WS=IO41, DOUT=IO42, DIN=IO40 |
| **USB OTG** | D_N=IO3, D_P=IO46 | |
| **UART1 (CA51F352P4)** | RX=IO44, TX=IO38 | JP3 跳线, TX 与 LCD_DC 共享 |
| **JTAG** | TMS=IO42, TDI=IO41, TCK=IO40, TDO=IO39 | JP5 |
| PSRAM (Octal) | GPIO 26-37 | 8MB, 系统占用 |
| Flash | 内部 MSPI | 16MB, 80MHz DIO |

## 外围芯片

| 芯片 | 功能 |
|------|------|
| ES8311 (U4) | 音频 DAC, I2C 地址 0x30 |
| CA51F352P4 (U7) | 触控/按键 MCU |
| TP4056 (U3) | 锂电池充电 |
| HXL1509-ADJ (U9) | DC-DC 降压 (5V→3.3V) |

## 分区表

16MB Flash: `factory` 1MB + `storage` 14MB (FAT 类型, 用于存放烧录的视频)。

`partitions.csv` 定义了自定义分区，`sdkconfig.defaults` 中启用了 `CONFIG_PARTITION_TABLE_CUSTOM`。

## 架构

**存储分工**: Flash = AVI 视频, SD 卡 = PCM 音频。  
**控制方式**: UART 指令 (CA51F352P4 触控 MCU → ESP32 UART1: RX=IO44, TX=IO38)。  
**主循环**: `switch(workspace)` 协作多任务, 1ms tick 驱动, 5 槽位轮转。

```
main/
├── main.c              # 协作多任务主循环 + UART 指令解析 + 状态机
├── video_player.c/h    # SD卡 AVI 播放器 (tick化, 备份功能)
├── raw_player.c/h      # SD卡 RAW 播放器 (tick化, 备份功能)
├── flash_player.c/h    # Flash AVI 播放器 (tick化, 主力)
├── audio_player.c/h    # SD卡 PCM 音频播放器 (tick化)
├── avi.c/h             # AVI 解析 (RIFF/movi/strh/strf 全解析)
├── mjpeg.c/h           # JPEG 解码器 (默认 esp_new_jpeg SIMD 加速)
├── audio.c/h           # ES8311 音频驱动 (PCM 播放 + 流式写入)
├── reset_to_dl.c/h     # 软复位到下载模式 (RTC GPIO hold IO0+IO46→esp_restart)
├── beep.c/h            # PWM 蜂鸣器 (IO46冲突, 未编译, 仅保留)
├── canon.pcm           # 内嵌测试音频 (EMBED_FILES)
├── idf_component.yml   # IDF 组件管理器依赖

components/BSP/
├── SPILCD/             # LCD 8080 并口 (esp_lcd_panel_io_i80, NV3051G 驱动)
├── SPI_SD/             # TF 卡 (SPI/SDMMC 宏切换)
├── MYSPI/              # SPI 总线 (仅 SD 卡)
├── MYIIC/              # I2C (仅 ES8311)
├── KEY/                # GPIO 按键 (BOOT=IO0)
├── LED/                # GPIO LED (IO1, 心跳指示)
├── XL9555/             # I2C GPIO扩展 (旧板遗留, 仅编译未使用)
```

### 主循环 Workspace 分配

| 槽位 | 功能 | 周期 |
|------|------|------|
| 0 | UART 指令接收 + 解析 | 5ms |
| 1 | 应用状态机 | 5ms |
| 2 | 播放器 tick 调度 | 5ms |
| 3 | 系统监控 (LED心跳/堆日志) | 5ms |
| 4 | BOOT 按键 (仅长按→DL) | 5ms |

### 应用状态机

```
IDLE ──VPLAY──▶ VIDEO_PLAYING ──VPAUSE──▶ VIDEO_PAUSED
  │                 │                          │
  │◀──VSTOP────────┘◀──────VSTOP──────────────┘
  │
  ├──APLAY──▶ AUDIO_PLAYING ──ASTOP──▶ IDLE
  │
  └──SLEEP──▶ SLEEP ──WAKE──▶ IDLE
```

## UART 指令协议

UART1: 115200-8N1, 文本协议 (`\n` 终止), RX=IO44, TX=IO38 (与 LCD_DC 共享)。

| 指令 | 功能 |
|------|------|
| `VPLAY` | 播放 Flash 中 AVI 视频 |
| `VPAUSE` | 暂停视频 |
| `VRESUME` | 继续播放 |
| `VSTOP` | 停止视频, 回空闲 |
| `APLAY <N/fname>` | 播放 SD 卡第 N 个或指定 PCM 文件 |
| `ALIST` | 列出 SD 卡中 .pcm 文件 |
| `ASTOP` | 停止音频 |
| `AMUTE` | 静音切换 |
| `VOL <0-100>` | 设置音量 |
| `DL` | 软复位进入烧录模式 |
| `RST` | 系统重启 |
| `STATUS` | 查询播放状态 |
| `INFO` | 查询系统信息 |
| `SLEEP` | 关屏休眠 |
| `WAKE` | 唤醒 |
| `LCD ON/OFF` | 背光开关 (转发 CA51F352P4) |
| `LCD B<0-100>` | 亮度 (转发 CA51F352P4) |

## 播放器类型

所有播放器已 tick 化: `_init()` → `_tick()` 每 5ms 调用 → `_stop()`，非阻塞，适合协作多任务。

| 播放器 | 数据源 | 格式 | 角色 |
|--------|--------|------|------|
| `flash_player` | Flash storage | AVI (MJPEG) | **主力** 视频播放 |
| `audio_player` | SD 卡 | PCM (16bit/mono/16kHz) | **主力** 音频播放 |
| `video_player` | SD 卡 | AVI (MJPEG+PCM) | 备份 (不调用) |
| `raw_player` | SD 卡 | .raw (RGB565) | 备份 (不调用) |

`main.c` 启动流程: 硬件 init → SD mount → 空闲画面 → 等 UART 指令。

## 视频转换工具 (tools/)

所有工具在 `tools/` 目录下，配置文件为同目录的 `.conf` 文件。

| 工具 | 用途 | 依赖 |
|------|------|------|
| `convert.sh` | 视频 → MJPEG AVI (SD卡播放) | ffmpeg |
| `convert_raw.sh` | 视频 → RGB565 .raw (SD卡播放) | ffmpeg + python3 + numpy |
| `convert_video.py` | 被 convert_raw.sh 调用, RGB888→RGB565 转换 | numpy |
| `flash_video.sh` | 烧录 AVI 到 Flash storage 分区 | esptool, 需先 `idf.py build` |

```bash
# SD 卡播放: 转换后复制到 TF 卡根目录
./tools/convert.sh video.mp4           # → video.avi
./tools/convert_raw.sh video.mp4       # → video.raw

# Flash 播放: 直接烧录到 ESP32
./tools/flash_video.sh video.avi
```

`.raw` 格式 header (14B): `"RAWV"`(4) + width(2 LE) + height(2 LE) + fps(2 LE) + frames(4 LE)，然后每帧 `width*height*2` 字节 RGB565。

## JPEG 解码

- **esp_new_jpeg (SIMD)**: 默认, ~13ms/帧 → 20fps@320×240
- **esp_jpeg**: 备选, 传统解码器
- 切换: `mjpeg.c` 改 `JPEG_DECODER` 宏
- 依赖由 `main/idf_component.yml` 管理: `espressif/esp_new_jpeg` + `espressif/esp_jpeg`

## UART 软复位下载

`main/reset_to_dl.c` — 软件触发进入下载模式, 免手动接地 IO46:

```c
#include "reset_to_dl.h"
reboot_to_download();  // 立即复位到烧录模式
```

原理: RTC GPIO hold 在复位期间保持 IO0+IO46 低电平, ROM bootloader 检测到后进入下载模式。

开机检测示例 (加在 `main.c` 中 SPI/LCD 初始化之前):
```c
#include "reset_to_dl.h"
#include "driver/gpio.h"

static void check_boot_download(void) {
    gpio_config_t cfg = { .pin_bit_mask = BIT64(0), .mode = GPIO_MODE_INPUT, .pull_up_en = true };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(100));
    int hold = 0;
    while (gpio_get_level(0) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (++hold > 30) reboot_to_download();
    }
}
// 在 app_main 中 SPI init 之前调用 check_boot_download();
```

CMakeLists.txt 需包含 `"reset_to_dl.c"`。

## 注意事项

- ES8311 固定 16kHz 单声道, 音频格式不匹配会静音或变速
- Octal PSRAM 占 GPIO 26-37, 不可用作普通 GPIO
- 新板 SD 和 LCD 不共享 SPI 总线 → 无 SPI 竞争瓶颈
- LCD 驱动在 `SPILCD/spilcd.c`, 使用 `esp_lcd_panel_io_i80` API, 驱动 IC 为 NV3051G
- BSP 组件编译选项 `-ffast-math -O3`, 禁止 format warning
- `beep.c/h` 仅保留文件, 未编译 — IO46 已改为 USB D+, 蜂鸣器不可用
- `XL9555` 组件仍编译但代码中未调用 — 新板用 CA51F352P4 替代
- `.gitignore` 忽略 `近视/` `散光/` 目录 (大视频文件不入库)
- `sample/` 为参考项目, `.gitignore` 忽略不入库
