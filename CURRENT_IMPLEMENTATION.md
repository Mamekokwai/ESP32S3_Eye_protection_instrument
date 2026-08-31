# 当前实现基准（2026-08-31）

本文是代码与说明文档对齐时的事实基准。发生冲突时，以对应源文件和构建结果为最终依据；旧的 QSPI、MAX98357、无 TF 卡方案只作为历史设计保留。

## 硬件与总线

| 子系统 | 当前实现 | 代码依据 |
|---|---|---|
| MCU/存储 | ESP32-S3-WROOM-1，16 MiB Flash，8 MiB Octal PSRAM | `sdkconfig.defaults`、板级资料 |
| LCD | 两块 JD9855 320×320，8-bit i80，共享 DB0–DB7/WR/D-C，CS1=17、CS2=18 | `components/BSP/SPILCD/` |
| LCD 引脚 | DB0–7=6–13、WR=46、D/C=38、RESET=3、TE=1（当前不等待） | `components/BSP/SPILCD/` |
| TF 卡 | 首选 SDMMC 1-bit 40 MHz：CLK=21、CMD=47、D0=14；SPI fallback 最高 20 MHz | `components/BSP/SD_CARD/` |
| 音频 | ES8311；I2C 4/5，I2S MCLK45/BCLK39/WS41/DOUT42，功放 GPIO2 高有效 | `main/audio.c` |
| UART | UART1 只接收 GPIO44；UART0 GPIO43 输出日志/响应 | `main/app_uart.c` |

GPIO0 在当前 SDMMC 1-bit 正常传输中不用；GPIO38 已被 LCD D/C 占用。任何声称“TF 当前为 SPI 20 MHz”“UART TX 为 GPIO38”或“USB 直接输入业务指令”的说明均不适用于当前版本。

## 软件与媒体

| 项目 | 当前实现 |
|---|---|
| 主循环 | 1 ms tick，5 个 cooperative workspace；视频每 1 ms 快速服务 |
| Flash 视频 | AVI/MJPEG，mmap `storage`，PSRAM 双帧，2×40 行内部 SRAM DMA 条带 |
| TF 视频 | MJPEG AVI ≤320×320，32 KiB 流读取，PSRAM 双帧，1×160 行 DMA 条带 |
| 图片 | Baseline JPEG ≤1 MiB、≤320×320，32 KiB 分块读，1×80 行 DMA 条带 |
| 音频 | TF `.pcm/.mp3`，CPU1 独立 5 ms 服务，ES8311 固定输出链路 |
| TF 目录 | `VIDLIST`、`IMGLIST`、`ALIST` 递归扫描；索引是 FAT 遍历顺序；支持 UTF-8 相对路径 |
| 屏幕目录 | `SDLIST` 只浏览根目录，这是独立 UI 功能 |

## Flash 分区

| 分区 | 开发偏移 | 生产偏移 | 大小 | 用途 |
|---|---:|---:|---:|---|
| 分区表 | 0x8000 | 0x10000 | 4 KiB | 生产偏移为签名 bootloader 留空间 |
| `nvs` | 0x9000 | 0x11000 | 24 KiB | NVS |
| `phy_init` | 0xF000 | 0x17000 | 4 KiB | PHY |
| `factory` | 0x10000 | 0x20000 | 1 MiB | 应用 |
| `storage` | 0x110000 | 0x120000 | 14 MiB | Flash FAT 媒体，生产配置下加密 |
| `nvs_keys` | 0xF10000 | 0xF20000 | 4 KiB | NVS 加密密钥，生产配置下加密 |

当前只有 factory 应用分区，没有 OTA 槽。开发默认配置不启用生产锁；生产配置由 `sdkconfig.production.defaults` 叠加启用。

## 文档状态规则

- 本文件、`README.md`、`UART_COMMANDS.md`、`SECURITY_PROVISIONING.md` 描述当前代码。
- `issue/`、Obsidian 的 RCA/ISSUE 文档可以保留当时的配置，但必须明确标记“历史问题/已归档”，不可作为当前接线或构建依据。
- 架构图的 JSON 是可编辑源，HTML 是展示副本；两者的关键文字必须同步。
