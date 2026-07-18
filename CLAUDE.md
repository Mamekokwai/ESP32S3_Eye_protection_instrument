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
| **ES8311 I2C** | SDA=IO4, SCL=IO5 | 和旧板相同 |
| **ES8311 I2S** | MCLK=IO45 | BCLK/WS/DOUT/DIN 待确认 |
| **USB OTG** | D_N=IO3, D_P=IO46 | |
| **UART** | TX=IO38, RX=IO44 | JP3 跳线 |
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

## 与旧板 (Hiwonder) 差异

| 功能 | 旧板 | YT06 V1.1 |
|------|------|-----------|
| LCD 接口 | SPI ST7789 | **8080 并口 8-bit** |
| LCD 分辨率 | 320×240 | **320×320** 圆屏 |
| SD SPI | 共享 SPI2 | **独立 SPI** (不抢总线!) |
| GPIO 扩展 | XL9555 (I2C) | **CA51F352P4** |
| 蜂鸣器 | IO46 PWM | **无** |
| USB OTG | 无 | **有** |
| 电池管理 | 无 | **TP4056** |

## 架构

```
main/
├── main.c              # 入口, 已去掉 beep/XL9555
├── video_player.c/h    # AVI/MJPEG 播放器 (双核流水线, 无缝循环)
├── raw_player.c/h      # RAW/RGB565 播放器
├── avi.c/h             # AVI 解析
├── mjpeg.c/h           # JPEG 解码器 (NEW=SIMD 加速)
├── audio.c/h           # ES8311 (已去 XL9555)

components/BSP/
├── MYSPI/              # SPI 总线 (仅 SD 卡)
├── SPI_SD/             # TF 卡 (SPI/SDMMC 宏切换)
├── SPILCD/             # LCD 8080 并口 (esp_lcd_panel_io_i80)
├── MYIIC/              # I2C (仅 ES8311)
```

## JPEG 解码

- **新版 esp_new_jpeg (SIMD)**: 默认, ~13ms/帧 → 20fps@320×240
- 切换: `mjpeg.c` 改 `JPEG_DECODER` 宏

## 注意事项

- 首次烧录需全量 `idf.py flash`
- ES8311 固定 16kHz 单声道
- Octal PSRAM 占 GPIO 26-37, 不可用
- 新板 SD 和 LCD 不共享总线 → 无 SPI 竞争瓶颈
- LCD 驱动在 `SPILCD/spilcd.c`, 使用 `esp_lcd_panel_io_i80` API
