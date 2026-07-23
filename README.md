# ESP32-S3 眼保仪固件

这是基于 ESP-IDF v5.4.4 的 ESP32-S3-WROOM-1 固件，运行于 YT06 V1.1 主板。设备驱动双 320×320 JD9855 圆形 LCD，支持 Flash 视频播放、TF 卡音频播放和 JPEG 图片显示，并通过 UART 接收 CA51F352P4 的控制指令。

## 功能概览

- Flash FAT 分区：存放并播放 AVI/MJPEG 视频；视频播放器不输出 AVI 音频。
- TF 卡：读取 PCM/MP3 音频、浏览目录、显示 Baseline JPEG 图片。
- LCD：RGB565，PSRAM 保存 JPEG/视频帧，内部 SRAM DMA 条带负责屏幕传输，避免 PSRAM 直接 DMA 花屏。
- 主循环：1 ms 定时驱动、5 个 workspace 协作调度，媒体读取和显示采用 tick 化流程。

## 目录结构

```text
main/                         应用状态机、UART、播放器和 JPEG 解码
components/BSP/SPILCD/       JD9855 双屏 8080 驱动
components/BSP/SPI_SD/       TF 卡 SPI 驱动
components/esp_lcd_jd9855/   LCD 面板初始化
tools/                        视频转换和 Flash 写入脚本
partitions.csv                16 MB Flash 分区表
UART_COMMANDS.md              完整 UART 指令说明
```

## 构建与烧录

```bash
source /home/nywerya/esp/v5.4.4/esp-idf/export.fish
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Flash 视频：

```bash
./tools/convert.sh input.mp4
./tools/flash_video.sh output.avi
```

## UART 快速示例

指令以 `\n` 结束，参数为 ASCII 文本：

```text
VPLAY
IMGLIST
IMG 1
IMG photo.jpg
APLAY music.mp3
STATUS
VSTOP
```

图片要求：Baseline `.jpg/.jpeg`、文件 ≤1 MiB、尺寸 ≤320×320；图片读取按 16 KiB 分块，LCD 按 40 行 DMA 条带刷新。完整指令、响应和错误码见 [UART_COMMANDS.md](UART_COMMANDS.md)。

## 硬件关键引脚

LCD 数据线为 GPIO6–13，WR=46，D/C=38，CS1=17，CS2=18，TE=1，RESET=3。TF 卡 SPI 为 CLK=21、MOSI=47、MISO=14、CS=0。CA51F352P4 指令输入为 UART1 RX GPIO44；ESP32 调试输出使用 UART0 GPIO43。

## 验证

当前没有自动化测试。提交前至少运行 `idf.py build`，并在硬件上验证 `STATUS`、视频控制、`IMGLIST`、`IMG <file>`、`ALIST` 和 `APLAY <file>`。显示相关改动需检查双屏同步、RGB565 颜色顺序、撕裂和 PSRAM/DMA 行为。
