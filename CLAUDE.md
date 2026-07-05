# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ESP32-S3 (Hiwonder 开发板) 的 ESP-IDF v5.4.4 模板项目。硬件：2.4" ST7789 SPI LCD (320×240)、ES8311 音频 DAC、XL9555 I2C GPIO 扩展器、无源蜂鸣器。

## 构建命令

```bash
# 需在 fish shell 下，先激活 ESP-IDF 环境
source /home/nywerya/esp/v5.4.4/esp-idf/export.fish
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

VSCode 需配置 `.vscode/settings.json` 的 `cmake.configureEnvironment` 指向工具链路径。

## 硬件引脚

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD SPI (SPI2_HOST) | SCLK=21, MOSI=47, DC=3, CS=2 | 2.4" ST7789 320×240 |
| LCD 背光 | XL9555 P1.6 (0x4000) | 仅开/关，非 PWM |
| ES8311 I2C | I2C1: SDA=4, SCL=5 | 地址 0x30 |
| ES8311 I2S | MCLK=45, BCLK=39, WS=41, DOUT=42, DIN=40 | 16kHz/mono/16bit |
| XL9555 I2C | I2C0: SDA=38, SCL=48 | 地址 0x20 |
| 蜂鸣器 | GPIO46 | PWM 2kHz (LEDC) |
| SD 卡 SPI | CS=0, MISO=14 | 与 LCD 共享 SPI2_HOST |

## 架构

```
main/
├── main.c              # 入口（beep → audio → PCM播放 → 循环）
├── beep.h/c            # PWM 蜂鸣器
├── audio.h/c           # ES8311 音频（I2C 配置 + I2S 数据）
├── idf_component.yml   # es8311, esp_codec_dev 依赖
└── CMakeLists.txt      # 源文件 + EMBED_FILES

components/BSP/         # 板级支持包（从参考项目复制）
├── MYIIC/              # I2C 驱动 (I2C0 + I2C1)
├── XL9555/             # GPIO 扩展器 (CS/背光/静音/按键)
├── MYSPI/              # SPI 总线初始化 (SPI2_HOST)
├── SPILCD/             # ST7789 LCD 驱动 (esp_lcd API)
├── SPI_SD/             # SPI SD 卡 (FATFS)
├── LED/                # LED (GPIO1)
└── KEY/                # BOOT 按键 (GPIO0)
```

## 关键依赖

- **ES8311**: `espressif/es8311: ^1.0.0` (IDF Component Manager)
- **esp_codec_dev**: `espressif/esp_codec_dev: ~1.4.0` (音频编解码抽象层)
- **esp_lcd**: ESP-IDF 内置 LCD 驱动 (ST7789 panel IO)
- BSP 组件编译选项：`-O3 -ffast-math`，需 `driver` 依赖

## Flash 分区

sdkconfig 配置为 16MB flash + 自定义 `partitions.csv`。factory 分区建议 ≥4MB（嵌入视频/音频时需要）。当前未创建 `partitions.csv`，需先创建或用 `idf.py menuconfig` 选择预制分区表。

## EMBED_FILES 嵌入文件

`canon.pcm` (625KB, 16kHz/mono/16bit PCM) 嵌入在固件 `.rodata` 段。访问方式：
```c
extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t canon_pcm_end[]   asm("_binary_canon_pcm_end");
size_t len = canon_pcm_end - canon_pcm_start;
```

## XL9555 引脚功能

| 位 | 宏 | 功能 |
|---|---|---|
| 0x0001 | CAMERA_LED_IO | 摄像头 LED |
| 0x0008 | TF_CS_IO | TF 卡 CS |
| 0x2000 | LCD_CS_IO | LCD 片选（辅助） |
| 0x4000 | BACKLIGHT_IO | LCD 背光 |
| 0x8000 | MUTE | 扬声器静音 (P1.7) |

## 注意事项

- `audio_init()` 内部调用 `xl9555_init()` 和 `myiic_init()`，如果在此之前需要操作 XL9555（如开背光），必须先调 `myiic_init()` + `xl9555_init()`
- ES8311 固定 16kHz 单声道，与 AVI 文件的 48kHz 立体声不匹配时需要下采样
- `myiic_init1()` 初始化 I2C1 (ES8311)，`myiic_init()` 初始化 I2C0 (XL9555)
- ESP32-S3 **不支持**经典蓝牙 (BR/EDR)，仅支持 BLE
