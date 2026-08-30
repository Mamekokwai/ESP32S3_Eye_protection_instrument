# ESP32-S3 眼保仪媒体链路 Archify 流程图

> 本文梳理项目内视频、音频、图像三条媒体链路，作为 `info/Map` 下的 Archify 流程图索引。
> 代码位置：`main/flash_player.c`、`main/video_player.c`、`main/audio_player.c`、`main/audio.c`、`main/image_viewer.c`、`main/mjpeg.c`、`main/avi.c`。

## 0. 总体链路

```mermaid
flowchart LR
    CA51[CA51F352P4 触控/按键 MCU] -->|UART1 RX IO44 文本指令| UART[app_uart.c 指令解析]
    UART --> STATE[main.c 显示状态机]
    STATE --> DISP{视频/图片共用 LCD}
    STATE -.独立状态.-> AUDIO[audio_player.c CPU1 音频服务]
    DISP -->|VPLAY/VID| VPLAY[Flash/SD 视频播放器]
    DISP -->|IMG/FIMG| IMG[image_viewer JPEG 图片]
    DISP -->|SDLIST| SDLIST[SD 目录浏览]
    VPLAY --> LCD[JD9855 双 320x320 圆屏]
    IMG --> LCD
    SDLIST --> LCD
    AUDIO --> ES8311[ES8311 DAC + 功放 GPIO2]
```

- 存储分工：Flash `storage` 分区（FAT 子类型，mmap 索引）存 AVI/JPEG；SD 卡存 MJPEG AVI、PCM/MP3、JPEG。
- CPU 分工：Flash/TF JPEG 解码固定 CPU0；音频服务固定 CPU1，5 ms tick。
- 显示互斥：视频与图片共用 LCD，二者互斥；音频与显示相互独立，`APLAY` 可与 `VID`/`IMG` 同时运行。

## 1. 视频链路

### 1.1 Flash AVI 视频（`VPLAY`，主力）

```mermaid
flowchart TD
    A[VPLAY 指令 / 上电自动播放] --> B[flash_player_start]
    B --> C[flash_media_init: mmap storage 分区]
    C --> D[flash_media_resolve: 从索引选择 AVI]
    D --> E[avi_init: 解析 RIFF/hdrl/strh/strf/movi]
    E --> F[分配资源]
    F --> F1[PSRAM 双帧缓冲 320x320 RGB565]
    F --> F2[内部 SRAM 双 40 行 DMA 条带]
    F --> F3[CPU0 JPEG 解码任务 + 队列/信号量]
    F3 --> G[预读前两帧 JPEG 并提交 F0 解码]
    G --> H[flash_player_tick 每 1ms 推进]
    H --> H1{等待解码完成?}
    H1 -->|未完成| H
    H1 -->|完成| H2[提交下一帧解码 + mmap 预读下一帧]
    H2 --> H3[帧率控制按 SecPerFrame]
    H3 --> H4[LCD 空闲检查 + 17ms 最小间隔/TE]
    H4 --> H5[PSRAM 帧 -> memcpy -> SRAM 40 行条带]
    H5 --> H6[首条 RAMWR 整帧窗口 / 后续 RAMWRC 连续写 DMA 到双屏]
    H6 --> H7{整帧所有条带 DMA 完成?}
    H7 -->|否| H
    H7 -->|是| H8[交换双缓冲并返回完成一帧]
    H8 --> H9{继续播放?}
    H9 -->|循环/下一帧| H
    H9 -->|VSTOP/错误| I[flash_player_stop 释放解码任务、帧/条带缓冲]
```

### 1.2 SD 卡 MJPEG AVI 视频（`VID`）

```mermaid
flowchart TD
    A[UART VID n/fname] --> B[media_catalog_resolve: 根目录 .avi]
    B --> C[FatFS f_open + 32KB DMA 流式读取器]
    C --> D[avi_init: 解析 AVI 头]
    D --> E[校验 <=320x320 且 SecPerFrame>0]
    E --> F[分配资源]
    F --> F1[PSRAM 双帧缓冲 + 双 JPEG 缓冲]
    F --> F2[内部 SRAM 160 行 DMA 条带]
    F --> F3[CPU0 JPEG 解码任务]
    F3 --> G[预读前两帧并提交 F0 解码]
    G --> H[video_player_tick 每 1ms 推进]
    H --> H1{等待解码完成?}
    H1 -->|未完成| H
    H1 -->|完成| H2[提交下一帧解码 + 流式预读下一帧]
    H2 --> H2A[read_one_chunk 自动跳过 AVI 音频块 00wb]
    H2A --> H3[帧率控制]
    H3 --> H4[LCD 空闲 + 17ms 最小间隔/TE]
    H4 --> H5[PSRAM 帧 cache 同步 -> memcpy -> SRAM 160 行条带]
    H5 --> H6[逐条带独立窗口 esp_lcd_panel_draw_bitmap 双屏 DMA]
    H6 --> H7{整帧完成?}
    H7 -->|否| H
    H7 -->|是| H8[交换缓冲并返回完成]
    H8 --> H9{继续播放?}
    H9 -->|循环/下一帧| H
    H9 -->|VSTOP/错误| I[video_player_stop 释放解码任务、文件、缓冲]
```

要点：
- Flash 视频从 mmap 分区零拷贝读取，不占 SPI2/SDMMC 总线。
- SD 视频使用 FatFS + DMA 读取；AVI 内嵌音频块不播放，只跳过。
- 禁止 PSRAM 直接 DMA 到 LCD；必须先 `memcpy` 到内部 SRAM DMA 条带，再提交 LCD。

## 2. 音频链路

```mermaid
flowchart TD
    A[UART APLAY n/fname] --> B[media_catalog_resolve: 根目录 .pcm/.mp3]
    B --> C[audio_player_init: FatFS 打开 SD 文件]
    C --> D{文件格式?}
    D -->|PCM| E[按 2KB 分块读取 16-bit mono/16kHz]
    D -->|MP3| F[4KB 输入缓冲 + micro_mp3 软解]
    F --> G[MP3 帧解码 -> PCM 缓冲]
    G --> H[按源采样率/声道送入 audio_write_pcm]
    E --> H
    H --> I[软件线性重采样到 44.1kHz]
    I --> J[立体声下混为单声道 + 软件/硬件音量]
    J --> K[esp_codec_dev_write -> I2S0 -> ES8311 DAC]
    K --> L[GPIO2 功放使能 -> 喇叭]
    L --> M[audio_player_stop: GPIO2 拉低 + 关闭文件]
```

要点：
- 音频服务在 CPU1 独立 FreeRTOS 任务中运行，5 ms tick；不受主循环 workspace 阻塞。
- ES8311/I2S 固定 44.1 kHz / 16-bit / mono；PCM 16 kHz、MP3 8–48 kHz 均在 CPU1 内线性重采样。
- `AMUTE` 直接拉低 GPIO2 功放使能；`VOL` 默认写 ES8311 I2C 音量寄存器（也可切换为软件 PCM 缩放）。
- 音频与视频/图片无时间轴同步，`APLAY` 可与显示播放并行。

## 3. 图像链路

```mermaid
flowchart TD
    A[UART IMG n/fname 或 FIMG n/fname] --> B[image_viewer_start / image_viewer_start_flash]
    B --> C{数据源}
    C -->|SD 卡| D[image_viewer_start: fopen + fseek 大小校验 <=1MB]
    D --> E[PHASE_READ: 按 32KB 分块读入 PSRAM]
    C -->|Flash| F[image_viewer_start_flash: flash_media mmap 直接引用]
    F --> G[PHASE_VALIDATE]
    E --> G
    G --> H[read_jpeg_dimensions: 校验 Baseline JPEG、<=320x320]
    H --> I[PHASE_DECODE: mjpeg_decoder_decode -> PSRAM RGB565 帧]
    I --> J[小图居中计算 offset_x/offset_y]
    J --> K{是否 320x320 全屏?}
    K -->|否| L[PHASE_CLEAR: 80 行条带清屏]
    K -->|是| M[跳过清屏避免旧帧消失]
    L --> M
    M --> N[PHASE_SUBMIT: 逐 80 行 memcpy 到内部 SRAM 条带]
    N --> O[esp_lcd_panel_draw_bitmap 双屏 DMA]
    O --> P{所有行已提交?}
    P -->|否| N
    P -->|是| Q[释放资源 -> 回复 OK IMG/FIMG name WxH]
```

要点：
- 图片仅支持 Baseline `.jpg/.jpeg`、文件 ≤1 MiB、解码尺寸 ≤320×320；小图居中显示。
- 解码帧在 PSRAM，LCD 只接收内部 SRAM 80 行 DMA 条带。
- `IMG`/`FIMG` 会停止当前视频/图片显示，但不会停止音频。

## 4. 状态机与命令速查

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> FLASH_VIDEO_PLAYING: VPLAY
    IDLE --> SD_VIDEO_PLAYING: VID
    IDLE --> IMAGE_LOADING: IMG/FIMG
    FLASH_VIDEO_PLAYING --> FLASH_VIDEO_PAUSED: VPAUSE
    FLASH_VIDEO_PAUSED --> FLASH_VIDEO_PLAYING: VRESUME
    SD_VIDEO_PLAYING --> SD_VIDEO_PAUSED: VPAUSE
    SD_VIDEO_PAUSED --> SD_VIDEO_PLAYING: VRESUME
    FLASH_VIDEO_PLAYING --> IDLE: VSTOP/错误
    SD_VIDEO_PLAYING --> IDLE: VSTOP/错误
    IMAGE_LOADING --> IDLE: 加载完成/错误
    IDLE --> SLEEP: SLEEP
    SLEEP --> IDLE: WAKE

    state Audio {
        [*] --> STOPPED
        STOPPED --> PLAYING: APLAY
        PLAYING --> STOPPED: ASTOP/错误/播放结束
    }
```

主要命令映射：

| 命令 | 入口函数 | 媒体链路 |
|---|---|---|
| `VPLAY [n/fname]` | `flash_player_start` | Flash AVI 视频 |
| `VID [n/fname]` | `video_player_start` | SD 卡 MJPEG AVI |
| `VPAUSE` / `VRESUME` / `VSTOP` | `g_display_mode` 切换 / player stop | 视频控制 |
| `APLAY [n/fname]` | `audio_player_start` | SD 卡 PCM/MP3 |
| `ALIST` / `ASTOP` / `AMUTE` / `VOL` | `audio_player_*` | 音频控制 |
| `IMG [n/fname]` | `image_viewer_start` | SD 卡 JPEG |
| `FIMG [n/fname]` | `image_viewer_start_flash` | Flash JPEG |
| `SDLIST [page]` | `sd_browser_show_page` | SD 目录浏览 |

## 5. 相关文件

| 模块 | 文件 | 职责 |
|---|---|---|
| 主循环/调度 | `main/main.c` | 1ms tick、5 workspace、显示/音频状态机 |
| UART 控制 | `main/app_uart.c/h` | 接收 CA51 指令、分发播放器调用 |
| Flash 媒体索引 | `main/flash_media.c/h` | mmap storage 分区、AVI/JPEG 条目解析 |
| Flash 视频 | `main/flash_player.c/h` | Flash AVI tick 播放器 |
| SD 视频 | `main/video_player.c/h` | SD MJPEG AVI 双核流水线播放器 |
| 音频 | `main/audio_player.c/h` + `main/audio.c/h` | PCM/MP3 播放、重采样、ES8311 驱动 |
| 图片 | `main/image_viewer.c/h` | SD/Flash JPEG 分块读取与条带显示 |
| 解码 | `main/mjpeg.c/h` | JPEG/MJPEG 解码封装（esp_new_jpeg/esp_jpeg） |
| AVI 解析 | `main/avi.c/h` | RIFF/movi/strh/strf 解析 |
