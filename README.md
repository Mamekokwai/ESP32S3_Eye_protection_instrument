# ESP32-S3 眼保仪固件

本项目是 YT06 主板（V1.4 为当前硬件）的 ESP-IDF v5.4.4 固件，目标芯片为 ESP32-S3-WROOM-1（16 MiB Flash、8 MiB Octal PSRAM）。设备驱动两块 320×320 JD9855 圆屏、ES8311 音频、TF 卡，并通过 CA51F352P4 发来的 UART 指令控制。

## 当前实现

- 显示：双 JD9855 共用 8-bit i80 数据总线，RGB565；视频与图片共用 LCD，互斥运行。
- Flash 媒体：`storage` FAT 分区保存 AVI/MJPEG 和 JPEG；Flash 视频跳过 AVI 音频块。
- TF 媒体：SDMMC 1-bit 40 MHz，递归扫描全部子目录；支持 MJPEG AVI、PCM/MP3、Baseline JPEG。
- 调度：CPU0 使用 1 ms tick 和 5 个 workspace；视频每 1 ms 服务，图片分阶段处理；音频在 CPU1 独立任务中每 5 ms 服务。
- DMA：媒体帧保存在 PSRAM，提交 LCD 前复制到内部 SRAM 条带。Flash 视频条带为 40 行×2，TF 视频为 160 行×1，图片为 80 行×1。
- 中文显示：FATFS CODEPAGE_936（GBK）；启动提示用内嵌点阵字库（无卡也显示），SDLIST 中文文件名走 TF 卡 `/SYSTEM/FONT/GBK16.FON`。
- 启动显示：无 TF 卡时提示“请插入SD卡”；TF 卡就绪但 Flash 自动视频缺失或索引无效时显示 `READY / NO FLASH VIDEO`，不会停留在黑屏。
- 生产安全：可选的一次性 **通用** SD 授权令牌（不绑定设备，一卡解锁所有设备；P-256 签名验证 + `EYECARE_UNLOCKED` eFuse），配合 Secure Boot V2 和 Release 模式 Flash Encryption；开发构建默认关闭。详见 [SECURITY_PROVISIONING.md](SECURITY_PROVISIONING.md)。

代码与文档的基准事实见 [CURRENT_IMPLEMENTATION.md](CURRENT_IMPLEMENTATION.md)，完整指令见 [UART_COMMANDS.md](UART_COMMANDS.md)，仓库与 Obsidian 的整理范围见 [DOCUMENTATION_ALIGNMENT.md](DOCUMENTATION_ALIGNMENT.md)。

## 目录

```text
main/                         应用状态机、UART、媒体播放与生产解锁
components/BSP/               LCD、SDMMC/SPI fallback、I2C 板级驱动
components/esp_lcd_jd9855/    JD9855 面板驱动和初始化序列
tools/linux/                  视频转换、Flash 媒体写入脚本
tools/security/               生产授权令牌工具（`unlock_token.py` + 一键脚本 `unlock_provision.sh`）
partitions.csv                16 MiB Flash 分区表
sdkconfig.defaults            开发/公共默认配置
sdkconfig.production.defaults 生产安全增量配置
```

## 构建

加载本机 ESP-IDF v5.4.4 环境后：

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Linux 媒体工具：

```bash
./tools/linux/convert_mp4_to_avi.sh input.mp4
./tools/linux/flash_video.sh output.avi
```

Windows PowerShell 媒体烧录：

```powershell
# 使用 tools/windows/flash_video.conf 中的配置
.\tools\windows\flash_video.ps1

# 临时覆盖串口、波特率和媒体文件
.\tools\windows\flash_video.ps1 -Port COM16 -Baud 921600 video.avi image.jpg
```

若 PowerShell 执行策略阻止脚本，可在当前终端运行
`Set-ExecutionPolicy -Scope Process Bypass`。该工具只写入 `storage` 媒体分区，
不会烧录应用固件或修改 eFuse。

生产构建必须使用独立配置文件和构建目录；不要让仓库根目录的开发 `sdkconfig` 覆盖安全默认值：

```bash
idf.py -B build-production \
  -D SDKCONFIG=sdkconfig.production \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.production.defaults' \
  build
```

生产镜像首次启动会执行不可逆的安全 eFuse 操作。先阅读生产指南，并只在备用板上走通全流程；不要直接在开发板执行生产 `flash`。

生产构建完成后可运行 `python tools/security/production_preflight.py --build-dir build-production` 做只读预检。

## 关键引脚

| 功能 | GPIO |
|---|---|
| LCD DB0–DB7 | 6–13 |
| LCD WR / D/C / CS1 / CS2 / RESET | 46 / 38 / 17 / 18 / 3 |
| 背光 PWM（V1.4，GPIO1 → Q3 → LEDK，LEDC 默认 100%） | 1 |
| SDMMC CLK / CMD / D0 | 21 / 47 / 14 |
| ES8311 I2C SDA / SCL | 4 / 5 |
| I2S MCLK / BCLK / WS / DOUT | 45 / 39 / 41 / 42 |
| 功放使能 | 2，高电平开启 |
| UART1 RX（业务输入） | 44 |
| UART0 TX（日志/响应） | 43 |

GPIO0 不参与当前 SDMMC 1-bit 数据传输。GPIO38 是 LCD D/C，不是 UART TX。V1.4 无 TE 引脚（GPIO1 为背光 PWM，非 TE 帧同步）。

## 媒体约束

- TF 视频：`.avi`、MJPEG、最大 320×320；AVI 内音频块被跳过。
- 图片：Baseline `.jpg/.jpeg`、最大 1 MiB、解码尺寸不超过 320×320；不支持 Progressive JPEG。
- TF 音频：`.pcm` 或 `.mp3`；PCM 约定为 16-bit、单声道、16 kHz。
- `VIDLIST`、`IMGLIST`、`ALIST` 会递归扫描子目录，序号按 FAT 遍历顺序生成；也可传相对路径。`SDLIST` 仍只在 LCD 上浏览根目录。

## 验证

当前没有自动化硬件测试。提交前至少运行开发构建；在硬件上验证 `STATUS`、`VPLAY`、`VIDLIST`/`VID`、`IMGLIST`/`IMG`、`ALIST`/`APLAY`，并检查双屏同步、RGB565 色序、撕裂、PSRAM/cache 与 DMA 行为。生产安全还必须完成指南中的错误令牌、掉电、重启和密文回读测试。
