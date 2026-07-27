# CODEBUDDY.md

## 项目概述

ESP32-S3-WROOM-1 (YT06 主板 V1.1) 眼保仪固件。双 320×320 圆屏 (i80 8080 8-bit 并口, JD9855 IC)、ES8311 音频 DAC、CA51F352P4 触控 MCU、TF 卡 SDMMC 1-bit、16MB Flash + 8MB PSRAM。

**控制方式**: CA51F352P4 → UART 指令 → ESP32 从机执行。

**主循环**: `switch(workspace)` 协作多任务, 5 槽位轮转, 1ms tick (Semaphore + ISR, 不阻塞)。

## 构建 & 烧录

```bash
source /home/nywerya/esp/v5.4.4/esp-idf/export.fish   # fish shell
# 或
source /home/nywerya/esp/v5.4.4/esp-idf/export.sh       # bash

idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

首次烧录: `idf.py flash` (全量, 含 bootloader + 分区表)。

## 当前状态 (2026-07-22)

| 功能 | 状态 | 备注 |
|------|------|------|
| Flash AVI 视频播放 | 🔧 已优化待实测 | 原基线 18–20fps；当前 i80 PCLK 40MHz |
| 双屏同步 | ✅ | CS1(IO17)+CS2(IO18) 同画面 |
| TE 帧同步 | ❌ 不可用 | JD9855 0x35 无法使能 TE, GPIO1 始终 LOW |
| SD 卡音频播放 | ⚠️ 未验证 | PCM 16bit/mono/16kHz、MP3, 待真机验证 |
| UART 指令 | ✅ | VPLAY/VIDLIST/VID/VSTOP/VPAUSE/STATUS/INFO/... |
| 软复位烧录 | ✅ | DL 指令 → GPIO0 hold → esp_restart |

## 已解决的关键问题

### 1. PSRAM DMA 数据损坏 (最后解决)

**症状**: 视频画面撕裂, 纯色帧也撕裂。

**根因**: `esp_cache_msync(C2M)` 后 PSRAM Controller 内部写缓冲未排空时 DMA 就读了 → 通道: `PSRAM → memcpy(CPU) → 内部 SRAM strip_buf → DMA → LCD`。

**修复尝试**: memw 屏障 ❌, msync+invalidate ❌, SRAM 中转 ✅。

详细分析: `笔记/开发/嵌入式/项目/2026/0604眼保仪_ESP32S3_320x320/PSRAM DMA 数据损坏分析.md`

### 2. GPIO1 TE 冲突与 GPIO2 功放控制

GPIO1 曾同时作为 LCD TE 输入和旧 LED 输出，输出模式破坏 LCD 状态机并导致黑屏。心跳 LED 已完全移除；GPIO2 现为功放 MUTE/使能脚，高电平开启喇叭。

### 3. 主循环 Tick 重构

从 `while(!flag) vTaskDelay(1)` 改为 `xSemaphoreTake(s_tick_sem, portMAX_DELAY)` + ISR `xSemaphoreGiveFromISR`。不阻塞, 不丢 tick, 看门狗正常。

### 4. 播放器非阻塞状态机

`flash_player_tick()` 为非阻塞状态机。视频链路每 1ms 服务一次，其他 workspace 保持 5ms；Flash 视频使用双 40 行条带，TF 视频使用单 160 行条带。

显示状态和音频状态相互独立：视频/图片共用 LCD，二者互斥；Flash/TF JPEG 解码固定 CPU0，音频固定 CPU1，不做音画同步。ES8311/I2S 固定 44.1kHz，其他输入采样率由音频任务软件转换，避免播放期间 I2C 重配。

### 5. 传输与运行速度优化

- 固件使用 ESP-IDF Performance (`-O2`) 编译，关闭逐帧性能日志和 JPEG 像素转储
- 修复帧率等待期间重复消费解码信号、重复提交下一帧的问题
- 两个内部 SRAM DMA 条带缓冲交替使用，PSRAM 仍只保存完整帧
- 首条发送 `RAMWR` 并设置整帧窗口，后续条带用 `RAMWRC` 连续写，避免每条重发 CASET/RASET 和同步等待
- 16px ASCII UI 使用完整行 SRAM DMA，一行一次事务；其他字号至少保持完整字形一次事务，不要恢复为逐像素行短事务
- 文字整行使用固定 64 字节对齐的内部 SRAM 缓冲，禁止将字体像素 DMA 缓冲放入 PSRAM；20MHz 降频对 P1 乱码无效，已恢复 40MHz
- 每 100 帧打印一次轻量 `perf` 日志，用于实机对比

## 文件结构

```
main/
├── main.c              # 主循环 + workspace 调度 + state machine
├── app_uart.c/h        # UART 指令接收/解析/响应
├── media_catalog.c/h   # 统一媒体列表、序号/文件名选择和扩展名校验
├── flash_player.c/h    # Flash AVI 播放器 (tick化, 非阻塞状态机)
├── audio_player.c/h    # SD 卡 PCM/MP3 音频 (tick化)
├── image_viewer.c/h    # SD 卡 JPEG 图片 (16KiB 分块读取 + SRAM DMA 条带显示)
├── video_player.c/h    # SD 卡 MJPEG AVI（序号/文件名选择、SRAM 条带 DMA）
├── raw_player.c/h      # SD 卡 RAW (备份, 未使用)
├── avi.c/h             # AVI 解析 (RIFF/movi/strh/strf)
├── mjpeg.c/h           # JPEG 解码 (esp_new_jpeg SIMD, RGB565_LE)
├── audio.c/h           # ES8311 音频驱动 (I2C+I2S)
├── reset_to_dl.c/h     # 软复位到烧录模式
└── idf_component.yml   # esp_new_jpeg, esp_jpeg 依赖

components/BSP/
├── SPILCD/spilcd.c/h   # LCD i80 驱动 + TE 诊断 + 绘图函数
├── SD_CARD/             # TF 卡 SDMMC/SPI 回退配置和挂载服务
├── SPI_SD/spi_sd.h     # 旧 sd_spi_* API 兼容头
├── MYIIC/my_iic.c/h    # I2C (ES8311)
├── KEY/key.c/h         # 旧 BOOT 按键代码，已停止编译
└── XL9555/             # 旧板遗留, 未使用

components/esp_lcd_jd9855/
└── esp_lcd_jd9855.c    # JD9855 初始化序列 (320×320)

info/                    # 硬件资料 (HTML, 图片, 原理图)
tools/                   # 视频转换 + 烧录脚本
```

## 宏开关

| 宏 | 文件 | 作用 |
|-----|------|------|
| `LCD_DUAL` | spilcd.c:20 | 双屏 `1`/单屏 `0` |
| `LCD_TE_ENABLE` | spilcd.h:27 | TE 软件等待 |
| `JD9855_TE_ENABLE` | esp_lcd_jd9855.c:23 | JD9855 TE 寄存器 (需同步 LCD_TE_ENABLE) |
| `FP_PROF_ENABLE` | flash_player.c:28 | 每 10 帧耗时占比打印 |
| `FP_LCD_CONTINUOUS_WRITE` | flash_player.c | `1`=RAMWRC 高吞吐；`0`=逐条带兼容模式 |

## 硬件引脚

| 功能 | GPIO | 备注 |
|------|------|------|
| LCD DB0-DB7 | IO6-IO13 | 8-bit 数据总线 (双屏共享) |
| LCD WR | IO46 | |
| LCD D/C | IO38 | |
| LCD CS1 | IO17 | 双屏模式手动 LOW |
| LCD CS2 | IO18 | 双屏模式手动 LOW |
| LCD TE | IO1 | INPUT+PULLUP, JD9855 未输出 |
| LCD RST | IO3 | |
| SD CLK | IO21 | |
| SD CMD | IO47 | |
| SD D0 | IO14 | |
| SD DAT3/BOOT | IO0 | 1-bit 运行时不用；`DL` 复位时拉低 |
| ES8311 I2C | SDA=IO4 SCL=IO5 | |
| ES8311 I2S | MCLK=IO45 BCLK=IO39 WS=IO41 DOUT=IO42 | |
| UART1 RX | IO44 | CA51F352P4 → ESP32 |
| UART0 TX | IO43 | ESP32 → 电脑调试 |
| 功放 MUTE/使能 | IO2 | HIGH=开启喇叭，停止/静音时 LOW |
| PSRAM Octal | IO26-37 | 8MB, 80MHz |

## UART 指令协议

115200-8N1, 文本, `\n` 终止。ESP32 是从机, 只收不发。

| 指令 | 功能 |
|------|------|
| VPLAY / VSTOP / VPAUSE / VRESUME | 视频控制 |
| VIDLIST | 列出 TF 卡根目录的 `.avi` 视频 |
| VID N / VID fname.avi | 播放第 N 个或指定 TF 卡 MJPEG AVI |
| APLAY N / APLAY fname | 音频播放 |
| ALIST / ASTOP / AMUTE | 音频列表/停止/静音 |
| SDLIST [page] | 屏幕分页显示 TF 卡根目录 |
| IMGLIST | 列出 TF 卡根目录的 JPEG 图片 |
| IMG N / IMG fname.jpg | 异步读取并显示 Baseline JPEG |
| VOL 0-100 | 音量 |
| STATUS / INFO | 查询状态/系统信息 |
| SLEEP / WAKE | 休眠/唤醒 |
| DL / RST | 烧录模式/重启 |

`IMG` 支持不超过 1 MiB、最大 320×320 的 Baseline `.jpg/.jpeg`；小图居中显示。完整压缩数据和 RGB565 帧位于 PSRAM，LCD 仅接收内部 SRAM 的 80 行 DMA 条带。
`VPLAY`、`VID`、`IMG` 不会停止音频，`APLAY` 也不会停止视频或取消图片加载；仅 `SLEEP` 会同时停止显示和音频。TF 视频解码固定 CPU0、音频固定 CPU1；TF 视频帧缓冲位于 PSRAM并显式同步 cache，LCD 使用单个内部 SRAM 160 行条带逐窗口 DMA。
TF 视频每 100 帧打印 `VID profile`，包含 SD 读取、JPEG 解码、cache 同步、PSRAM→SRAM、LCD 提交/刷新及等待阶段的耗时和窗口占比。

## 开发经验文档

Obsidian vault: `~/Documents/note/Obsidian/笔记/开发/嵌入式/项目/2026/0604眼保仪_ESP32S3_320x320/`

- `开发经验.md` — 完整开发记录 (FreeRTOS tick, LCD 引脚, PSRAM cache, GPIO1 冲突, tick 重构...)
- `PSRAM DMA 数据损坏分析.md` — PSRAM 问题专题

## 待办 / 已知问题

1. **性能实测**: 烧录后确认 `perf` 日志、双屏画面和 RAMWRC 连续写兼容性
2. **TE 帧同步**: JD9855 0x35 寄存器未调通, 需原厂 datasheet。当前无 TE 同步, 17ms 软件最小间隔保底
3. **音频**: SD 卡未插入, 音频播放未端到端验证
4. **UART 指令**: VPLAY 已收到但不重启播放 (app_uart.c 处理逻辑)
5. **PSRAM 降频测试**: 未测试 40MHz PSRAM 是否消除 DMA 问题
6. **内部 SRAM 帧缓冲**: 未测试直接用内部 SRAM 分配 200KB 帧缓冲 (可能内存不足)
