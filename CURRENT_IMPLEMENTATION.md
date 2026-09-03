# 当前实现基准（2026-08-31）

本文是代码与说明文档对齐时的事实基准。发生冲突时，以对应源文件和构建结果为最终依据；旧的 QSPI、MAX98357、无 TF 卡方案只作为历史设计保留。

## 硬件与总线

| 子系统 | 当前实现 | 代码依据 |
|---|---|---|
| MCU/存储 | ESP32-S3-WROOM-1，16 MiB Flash，8 MiB Octal PSRAM | `sdkconfig.defaults`、板级资料 |
| LCD | 两块 JD9855 320×320，8-bit i80，共享 DB0–DB7/WR/D-C，CS1=17、CS2=18 | `components/BSP/SPILCD/` |
| LCD 引脚 | DB0–7=6–13、WR=46、D/C=38、RESET=3；无 TE（V1.4 无 TE 引脚） | `components/BSP/SPILCD/` |
| 背光 | V1.4：GPIO1=PWM_LED，LEDC PWM（1 kHz/8bit）→ Q3 → LEDK；启动时先关闭，读取 NVS 后延迟 1 s 按保存值开启，`spilcd_backlight_set()` 调光 | `components/BSP/SPILCD/spilcd.c`、`main/app_settings.c` |
| TF 卡 | 首选 SDMMC 1-bit 40 MHz：CLK=21、CMD=47、D0=14；SPI fallback 最高 20 MHz | `components/BSP/SD_CARD/` |
| 音频 | ES8311；I2C 4/5，I2S MCLK45/BCLK39/WS41/DOUT42，功放 GPIO2 高有效 | `main/audio.c` |
| UART | UART1（RX GPIO44/TX GPIO43）及原生 USB Serial-JTAG 独立双向通信；响应返回来源链路；UART1 默认 GBK、USB 默认 UTF-8，可用 `ENC`/`ENC?` 独立配置和查询；UART1 完整行异步转发到 JTAG（`CA51 ` 前缀），`DBG ` 调试行不执行且不响应；JTAG 可用 `CA51FWD ON|OFF` 开关并用 `CA51FWD?` 查询；支持背光控制 | `main/app_uart.c` |

GPIO0 在当前 SDMMC 1-bit 正常传输中不用；GPIO38 已被 LCD D/C 占用。任何声称“TF 当前为 SPI 20 MHz”或“UART TX 为 GPIO38”的说明均不适用于当前版本。


## 软件与媒体

UART 业务链路已调整为 UART1（CA51，RX GPIO44/TX GPIO43）和 USB Serial-JTAG（电脑）双通道独立收发；UART0 不参与业务通信。媒体路径在固件内部保持 GBK，协议响应按来源链路独立编码：UART1 默认 GBK、USB 默认 UTF-8，可用 `ENC UTF8|GBK` 设置当前链路并用 `ENC?` 查询，重启后恢复默认值。
UART1 收到的完整文本行会以 `CA51 ` 前缀异步转发到 USB Serial-JTAG，仅供调试观察，不会在 JTAG 侧重复执行；其中 CA51 固件发来的 `DBG ` 行在转发后立即结束解析，不返回 `ERR unknown`。JTAG 可用 `CA51FWD ON|OFF` 控制转发、用 `CA51FWD?` 查询，配置仅在当前运行期间有效，复位后默认开启。转发采用有界队列和非阻塞发送，USB 不可用或队列超限时允许丢弃并记录警告。

SD 视频播放器的 LCD DMA 条带缓冲在播放器生命周期内持久复用，连续执行 `VID 1`、`VID 2` 等切换不会重复申请内部 DMA 内存。

SD 视频流水线由 CPU0 执行主循环和 TF 读取，CPU1 执行 JPEG 解码；JPEG 解码任务优先级 4，低于 CPU1 音频任务优先级 5，使读取与解码并行且音频优先。

ES8311 初始化在 I2C 地址 `0x18` 未应答时每 1 秒重试一次；应答后才继续 I2S、codec 和播放链路初始化。

SD 视频只使用 AVI `SecPerFrame` 做帧率控制，并以 LCD DMA 完成事件作为下一帧提交门控；已移除重复的固定 17 ms LCD 间隔，避免高码率视频额外降帧。

| 项目 | 当前实现 |
|---|---|
| 启动门与热插拔 | `app_main` 先 `spilcd_init` 再 `boot_gate()`：TF 卡座无 CD 检测脚，首次挂载前等待 300 ms，每次挂载最多重试 3 次（间隔 100 ms）；无 SD 卡时显示 Flash 中的 `SDCard.jpg`（不依赖字库）并重试。运行中每 2 s 用 CMD13 探测卡状态，拔卡时先停止 SD 视频/音频并卸载文件系统，重新插卡后自动重挂载；已挂载且在线时不会重复卸载挂载。`IMG` 执行前和读取失败后均复核 SD 状态，确认无卡时统一切换到 Flash `SDCard.jpg`，不显示 `IMAGE ERROR` 字体页。加密开启且未解锁显示`请解密`并尝试解锁，通过后进入正常启动。启动门通过后自动分片显示 Flash `start.jpg`，不再自动播放 Flash 视频；启动图缺失时显示 `READY / NO START IMAGE`。 |
| 用户设置 | NVS 命名空间 `eyecare` 保存 `volume`、`backlight` 两个 5~100 参数；启动最前面读取，音频初始化后应用音量，LCD 初始化后延迟 1 s 应用背光；`VOL`/`BL` 设置成功后立即提交 NVS，掉电后恢复。支持 `VOL±`/`BL±` 步进 1，以及 `VOL±±`/`BL±±` 步进 10，边界自动钳位到各自宏定义的 5~100。 |
| 中文字库 | ① 内嵌 `gbk_embedded_font.h`（解密提示等无卡后续流程），② TF 卡 `/SYSTEM/FONT/GBK16.FON`（完整 GBK16 字库，SDLIST 任意中文文件名）。无 SD 卡启动画面使用 Flash 中的 `SDCard.jpg`，不依赖字库。FATFS 用 CODEPAGE_936 + ANSI/OEM，`d_name` 返回 GBK 双字节 |
| 主循环 | 正常模式为 1 ms tick、5 个 cooperative workspace，视频每 1 ms 快速服务；`SLEEP` 时停止 tick，仅每 20 ms 轮询 UART1/JTAG 命令，`WAKE` 后恢复调度 |
| Flash 视频 | AVI/MJPEG，mmap `storage`，兼容媒体索引 v1/v2，PSRAM 双帧，2×40 行内部 SRAM DMA 条带 |
| TF 视频 | MJPEG AVI ≤320×320，32 KiB 流读取，PSRAM 双帧，1×160 行 DMA 条带 |
| 图片 | Baseline JPEG ≤1 MiB、≤320×320，32 KiB 分块读，1×80 行 DMA 条带 |
| 音频 | TF `.pcm/.mp3`，CPU1 独立 5 ms 服务，ES8311 固定输出链路；`APLAY` 按 `ALIST` 递归索引自动轮播，单曲时循环，`ASTOP` 关闭轮播 |
| TF 目录 | `VIDLIST`、`IMGLIST`、`ALIST` 递归扫描；索引是 FAT 遍历顺序；内部使用 GBK 中文相对路径（FATFS CODEPAGE_936），UART 输出可选 GBK/UTF-8 |
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

当前只有 factory 应用分区，没有 OTA 槽。开发配置和当前生产增量配置均关闭生产锁及 Secure Boot/Flash Encryption；待量产流程确认后，才允许恢复 `sdkconfig.production.defaults` 中的安全选项。

## 文档状态规则

- 本文件、`README.md`、`UART_COMMANDS.md`、`SECURITY_PROVISIONING.md` 描述当前代码。
- `issue/`、Obsidian 的 RCA/ISSUE 文档可以保留当时的配置，但必须明确标记“历史问题/已归档”，不可作为当前接线或构建依据。
- 架构图的 JSON 是可编辑源，HTML 是展示副本；两者的关键文字必须同步。
