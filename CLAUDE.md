# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

ESP32-S3 (Hiwonder 开发板) 的 ESP-IDF v5.4.4 视频播放项目。硬件：2.4" ST7789 SPI LCD (320×240)、ES8311 音频 DAC、XL9555 I2C GPIO 扩展器、TF 卡 SPI、16MB Flash + 8MB PSRAM。

## 构建命令

```bash
# fish shell 下激活 ESP-IDF 环境
source /home/nywerya/esp/v5.4.4/esp-idf/export.fish
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## 硬件引脚

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD SPI (SPI2_HOST) | SCLK=21, MOSI=47, DC=3, CS=2 | 60MHz, ST7789 320×240 |
| LCD 背光 | XL9555 P1.6 (0x4000) | 仅开/关 |
| SD 卡 SPI | CS=0, MISO=14, SCLK=21, MOSI=47 | **共享 SPI2_HOST, 和 LCD 串行** |
| ES8311 I2C | I2C1: SDA=4, SCL=5 | 地址 0x30 |
| ES8311 I2S | MCLK=45, BCLK=39, WS=41, DOUT=42, DIN=40 | 16kHz/mono/16bit |
| XL9555 I2C | I2C0: SDA=38, SCL=48 | 地址 0x20 |
| 蜂鸣器 | GPIO46 | PWM 2kHz |
| PSRAM (Octal) | GPIO 26-37 | 8MB, 系统占用 |
| Flash | 内部 MSPI | 16MB, 80MHz DIO |

## 架构

```
main/
├── main.c              # 入口: 搜索SD卡上.raw/.avi → 循环播放
├── video_player.c/h    # AVI/MJPEG 播放器 (双核流水线, 无缝循环)
├── raw_player.c/h      # RAW/RGB565 播放器 (CMD18多块读, 无解码)
├── avi.c/h             # AVI 文件解析 (RIFF/MJPG/PCM)
├── mjpeg.c/h           # JPEG 解码器 (宏切换旧版ROM/新版SIMD)
├── audio.c/h           # ES8311 音频 (I2C+I2S)
├── beep.c/h            # PWM 蜂鸣器
├── idf_component.yml   # 依赖: es8311, esp_codec_dev, esp_jpeg, esp_new_jpeg
└── CMakeLists.txt

components/BSP/         # 板级支持包
├── MYSPI/              # SPI 总线初始化 (仅SPI2_HOST, 供LCD用)
├── SPI_SD/             # TF卡初始化 (SPI/SDMMC宏切换)
├── SPILCD/             # ST7789 LCD (esp_lcd API)
├── MYIIC/              # I2C 驱动
├── XL9555/             # GPIO 扩展器
├── LED/ KEY/           # LED/按键

tools/
├── convert.sh/.conf    # AVI/MJPEG 转换 (ffmpeg)
├── convert_raw.sh/.conf # RAW/RGB565 转换 (python + ffmpeg + numpy)
└── convert_video.py    # RAW 转换引擎
```

## 关键瓶颈与优化 (已验证)

### SD 卡读取
- **SPI 20MHz 上限**: 模块硬件限制, 26/40MHz 初始化失败
- **CMD17 vs CMD18**: PSRAM buffer 触发 `sdmmc_read_sectors` 逐扇区回退 → 710 KB/s。换内部 DMA buffer → CMD18 多块读 → **1640 KB/s**。
- **DMA buffer 对齐**: `f_read` 目标地址必须 32 字节对齐 + `MALLOC_CAP_DMA`, 否则回落 CMD17
- SDMMC 1-bit 不可行: CLK/CMD 与 LCD SPI 引脚冲突

### JPEG 解码
- **旧版 ROM TJPGD** (`esp_jpeg`): 90ms/帧 → 11fps@320×240
- **新版 SIMD** (`esp_new_jpeg`): 13ms/帧 → 20fps@320×240 (需 `heap_caps_aligned_alloc(16, ...)` 分配输出 buffer)
- 切换方式: `mjpeg.c` 头部 `#define JPEG_DECODER JPEG_DECODER_NEW`

### SPI 总线瓶颈
- LCD 和 SD 共享 SPI2 → 串行化 → 每帧 ~42ms SPI 时间 → **理论极限 ~24fps**
- 降分辨率可显著提升: 160×120 → LCD 传 9ms + SD 读 4ms → 70fps+

### 视频播放帧率公式
```
RAW:    fps = 1000 / (帧B÷1640 + 帧B÷6400)
MJPEG:  fps = 1000 / max(13ms, 帧B÷1640 + 帧B÷6400)   (13ms = esp_new_jpeg decode)
```

## 双核流水线 (video_player)

```
Core0 (SPI):  SD读取 + LCD发送  (串行, 共享SPI2)
Core1 (CPU):  JPEG解码         (并行, 与SPI DMA重叠)
```

- `decode_task`: Core1 死循环, 通过 FreeRTOS Queue 收 job, Semaphore 通知完成
- 主循环: `wait_decode → submit_next → read_next → display_cur → swap`
- **无缝循环**: 读到文件尾自动 `f_lseek` 回 movi 头, 无外层循环切换停顿

## TF 卡协议切换

`components/BSP/SPI_SD/spi_sd.h`:
```c
#define SD_PROTOCOL  SD_PROTOCOL_SPI         // 当前
#define SD_PROTOCOL  SD_PROTOCOL_SDMMC_1BIT  // 需改硬件引脚
```
SPI 和 SDMMC 共用 `spi_sd_init()` 接口, 上层代码不感知协议差异。

## Flash 分区

`partitions.csv`: 16MB Flash → nvs(24KB) + phy_init(4KB) + factory(1M) + storage(14M)

`storage` 分区预留给视频数据 (内存映射访问, ~10-15 MB/s, 不占 SPI2)。

## 转换工具

```bash
# AVI/MJPEG (video_player 播放)
./tools/convert.sh video.mp4

# RAW/RGB565 (raw_player 播放)
./tools/convert_raw.sh video.mp4
```

配置在对应 `.conf` 文件。ffmpeg mjpeg `-q:v` 范围 2~31 (2=最高画质)。

## 注意事项

- 首次烧录新分区表需 `idf.py build flash`, 后续可 `idf.py app-flash` 只更新固件
- `audio_init()` 内部调用 `xl9555_init()+myiic_init()`, 在此之前操作 XL9555 需手动初始化
- `myiic_init1()` → I2C1 (ES8311), `myiic_init()` → I2C0 (XL9555)
- ES8311 固定 16kHz 单声道, 与 AVI 文件 48kHz 立体声不匹配
- ESP32-S3 不支持经典蓝牙, 仅 BLE
- Octal PSRAM 占用 GPIO 26-37, 不可用于其他外设
