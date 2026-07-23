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
| WR | IO46 | 写信号 (两屏共享) |
| D/C | IO38 | 数据/命令 (两屏共享) |
| CS1 | IO17 | 左眼屏片选 |
| CS2 | IO18 | 右眼屏片选 |
| TE | IO1 | Tearing Effect (两屏共享) |
| RESET | IO3 | LCD 复位 (两屏共享) |
| 同步方式 | CS1+CS2 同时拉低 | 写命令/数据, 两屏同画面 |
| **SD 卡 SPI** | | 独立总线, 不共享 |
| CLK | IO21 | |
| MOSI | IO47 | R21 10K 上拉 |
| MISO | IO14 | |
| CS | IO0 | R20 10K 上拉 |
| **ES8311 I2C** | SDA=IO4, SCL=IO5 | |
| **ES8311 I2S** | MCLK=IO45, BCLK=IO39, WS=IO41, DOUT=IO42, DIN=IO40 |
| **USB OTG** | D_N=IO19, D_P=IO20 | |
| **UART 共享总线** | | 三设备共享一条 UART 线路 |
| CA51F352P4 TX → ESP32 RX | IO44 | CA51F352P4 发送指令给 ESP32 |
| ESP32 TX (调试) → 电脑 | IO43 | ESP32 调试输出, 电脑接收 |
| 电脑 TX → CA51F352P4 RX | IO43 | 电脑发指令给 CA51F352P4 |
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
**控制方式**: CA51F352P4 (主芯片) 通过共享 UART 总线发指令 → ESP32 (从机) 接收并执行。  
背光由 CA51F352P4 自己控制, ESP32 不管。  
**主循环**: `switch(workspace)` 协作多任务, 1ms tick 驱动, 5 槽位轮转。  
**FreeRTOS**: `CONFIG_FREERTOS_HZ=1000` (1 tick = 1ms, 在 `sdkconfig.defaults` 中配置)。

```
main/
├── main.c              # 协作多任务主循环 + 状态机
├── app_uart.c/h        # UART 指令接收 + 解析 + 调试输出
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
├── SPILCD/             # LCD 8080 并口 (esp_lcd_panel_io_i80, JD9855 驱动)
├── SPI_SD/             # TF 卡 (SPI/SDMMC 宏切换)
├── MYSPI/              # SPI 总线 (仅 SD 卡)
├── MYIIC/              # I2C (仅 ES8311)
├── KEY/                # GPIO 按键 (BOOT=IO0)
├── LED/                # GPIO LED (IO1, 心跳指示)
├── XL9555/             # I2C GPIO扩展 (旧板遗留, 仅编译未使用)

components/esp_lcd_jd9855/
├── esp_lcd_jd9855.c    # JD9855 驱动 (WA54TE057I-20Z, 320x320)
└── include/esp_lcd_jd9855.h
```

### 主循环 Workspace 分配

| 槽位 | 功能 | 周期 |
|------|------|------|
| 0 | UART 指令接收 + 解析 | 5ms |
| 1 | 应用状态机 | 5ms |
| 2 | 音频播放器 tick；视频另走 1ms 快速服务 | 音频5ms / 视频1ms |
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

## UART 通信架构

三设备共享一条 UART 总线 (115200-8N1, 文本协议 `\n` 终止):

```
CA51F352P4 TX ──→ IO44 ──→ ESP32 UART1 RX   (指令)
                         └──→ 电脑 USB RX    (监控)

电脑 USB TX ──→ IO43 ──→ CA51F352P4 RX       (电脑发指令给 CA51)
              └──→ 电脑终端回显

ESP32 UART0 TX ──→ IO43 ──→ 电脑 USB RX      (调试输出)
                 └──→ CA51F352P4 RX           (忽略)
```

**原则**: 一发三收。ESP32 是从机, 只收 CA51F352P4 的指令, 执行后通过 UART0 回调试信息。

## UART 指令协议

| 指令 | 功能 | 响应示例 |
|------|------|---------|
| `VPLAY` | 播放 Flash 中 AVI 视频 | `OK VPLAY` |
| `VPAUSE` | 暂停视频 | `OK VPAUSE` |
| `VRESUME` | 继续播放 | `OK VRESUME` |
| `VSTOP` | 停止视频, 回空闲 | `OK VSTOP` |
| `APLAY <N/fname>` | 播放 SD 卡第 N 个或指定 PCM 文件 | `OK APLAY` |
| `ALIST` | 列出 SD 卡中 .pcm 文件 | `ALIST` + 文件列表 |
| `ASTOP` | 停止音频 | `OK ASTOP` |
| `AMUTE` | 静音切换 | `OK AMUTE on/off` |
| `VOL <0-100>` | 设置音量 | `OK VOL 80` |
| `DL` | 软复位进入烧录模式 | `OK DL` |
| `RST` | 系统重启 | `OK RST` |
| `STATUS` | 查询播放状态 | `STATUS idle` |
| `INFO` | 查询系统信息 | `INFO heap=xxx` |
| `SLEEP` | 关屏休眠 | `OK SLEEP` |
| `WAKE` | 唤醒 | `OK WAKE` |

ESP32 收到未知指令返回 `ERR unknown: <cmd>`。

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

`main/reset_to_dl.c` — 软件触发进入下载模式:

```c
#include "reset_to_dl.h"
reboot_to_download();  // 立即复位到烧录模式
```

原理: `gpio_hold_en(GPIO_NUM_0)` + `gpio_deep_sleep_hold_en()` 在 `esp_restart()` 期间保持 IO0 低电平,
ROM bootloader 检测到后进入下载模式。

注意: ESP32-S3 只有 GPIO 0-21 是 RTC GPIO, IO46 不是, 已从代码中移除。
开机检测 GPIO0 长按的代码已注释掉 (硬件问题: GPIO0 被 R57 0Ω 拉低, 需拆掉)。

## 注意事项

- ES8311 固定 16kHz 单声道, 音频格式不匹配会静音或变速
- Octal PSRAM 占 GPIO 26-37, 不可用作普通 GPIO
- 新板 SD 和 LCD 不共享 SPI 总线 → 无 SPI 竞争瓶颈
- **LCD 驱动 IC 为 JD9855** (WA54TE057I-20Z, 320×320), 不是 NV3051G/ST7789
- LCD 驱动在 `components/esp_lcd_jd9855/`, 初始化序列对齐原厂参考代码
- `SPILCD/spilcd.c` 中 `LCD_DUAL` 宏控制单屏/双屏: `0`=仅 CS1 单屏测试, `1`=CS1+CS2 双屏
- **背光由 CA51F352P4 控制** (PWM_LED → Q3 → LEDK), ESP32 不参与
- ESP32 UART1 仅配置 RX (IO44), 不配置 TX (IO38 是 LCD D/C, 不能用作 UART TX)
- 调试输出走 UART0 (IO43), `uart_send_str()` 在 `app_uart.c` 中
- BSP 组件编译选项 `-ffast-math -O3`, 禁止 format warning
- `beep.c/h` 仅保留文件, 未编译 — IO46 已改为 USB D+
- `XL9555` 组件仍编译但代码中未调用 — 新板用 CA51F352P4 替代
- `.gitignore` 忽略 `近视/` `散光/` 目录 (大视频文件不入库)
- `sample/` 为参考项目, `.gitignore` 忽略不入库
