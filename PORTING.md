# ESP32-S3 眼保仪固件移植指南

本文说明如何把当前固件移植到另一块 ESP32-S3 主板、同一产品的新 PCB 版本，或一套新的显示/音频/存储器件上。文档描述的是当前仓库的实际实现；如果目标硬件与 V1.4 不同，必须先完成硬件核对，再修改驱动和配置。

## 1. 适用范围和事实来源

- 目标芯片：ESP32-S3-WROOM-1，当前工程按 16 MiB Flash、8 MiB Octal PSRAM 配置。
- ESP-IDF：v5.4.4。
- 当前硬件基准：`info/YT06-主板1_V1.4.net`。V1.1 网络表与 V1.4 不兼容，不能直接套用旧引脚表。
- 固件事实表：`CURRENT_IMPLEMENTATION.md`。
- UART 协议：`UART_COMMANDS.md` 和 `sample/UART_COMMANDS(1).md`。
- 量产安全流程：`SECURITY_PROVISIONING.md`、`TODO.md`。

移植时以 V1.4 网络表为硬件权威。任何引脚、总线、电平或器件变化，都要同步更新 `CURRENT_IMPLEMENTATION.md` 及相关设计笔记；不要只改本文件或只改代码。

## 2. 移植前准备

确认以下条件后再开始编译：

1. 安装 ESP-IDF v5.4.4、对应 Python 环境和 Ninja，并能在终端运行 `idf.py --version`。
2. 目标芯片确实是 ESP32-S3，Flash 容量不少于当前分区表要求，PSRAM 类型和速度与 `sdkconfig` 一致。
3. 板上 3.3 V 电源、USB 供电和音频功放电源有足够余量。LCD 刷新、SD 读写、JPEG 解码和音频同时工作时会产生明显的瞬时电流，供电不足会触发 Brownout。
4. 量产板必须单独准备安全配置和密钥流程；开发板不要烧 eFuse，也不要使用生产固件进行试验。

## 3. V1.4 引脚和电气要求

下表是当前固件默认映射。改板时应先修改相应驱动配置，再进行示波器和万用表验证。

| 功能 | GPIO | 当前实现和注意事项 |
| --- | ---: | --- |
| LCD DB0~DB7 | 6~13 | 两块 JD9855 共用 8-bit i80 数据总线 |
| LCD WR | 46 | 两块屏共用写时钟 |
| LCD D/C | 38 | 两块屏共用命令/数据选择 |
| LCD CS1 / CS2 | 17 / 18 | 软件手动片选；双屏模式下按面板分别选通 |
| LCD RESET | 3 | 两块屏共用复位 |
| 背光 PWM | 1 | `PWM_LED → R17 → Q3 → LEDK`，LEDC 低速、1 kHz、8-bit；不是 TE 引脚 |
| SDMMC CLK | 21 | 默认 SDMMC 1-bit，初始化目标 40 MHz |
| SDMMC CMD | 47 | 使用内部上拉 |
| SDMMC D0 | 14 | 使用内部上拉 |
| SD SPI CS（备用） | 0 | 仅显式切换到 SPI fallback 时使用；GPIO0 是启动相关脚，不能随意外接强拉电路 |
| ES8311 I2C SDA / SCL | 4 / 5 | I2C 开漏总线，需可靠上拉到 3.3 V；默认地址 0x18 |
| ES8311 I2S MCLK | 45 | 音频主时钟 |
| ES8311 I2S BCLK / WS | 39 / 41 | 位时钟 / 左右声道时钟 |
| ES8311 I2S DOUT / DIN | 42 / 40 | 播放数据输出 / 录音输入 |
| 功放 MUTE/EN | 2 | 高电平打开功放，低电平静音；不能拿来做心跳灯 |
| CA51 UART1 RX / TX | 44 / 43 | 115200 8N1、无硬件流控；外部 TX 必须接 ESP RX，外部 RX 接 ESP TX，并共地 |
| USB Serial-JTAG | 19 / 20 | 电脑调试和命令链路，独立于 UART1 |

V1.4 没有 TE 引脚。不要把 GPIO1 当作 TE，也不要在 GPIO4/5 上使用普通推挽输出。I2C 上拉阻值应按总线电容、线长和速率选择；上电后用示波器确认 SDA/SCL 空闲为高电平、起始/停止条件完整。

## 4. 应修改的代码和配置位置

移植不要在生成的 `build/` 文件中改参数。常用修改点如下：

- `components/BSP/SPILCD/spilcd.h/.c`：LCD GPIO、双屏 CS、分辨率、i80 时序、背光 PWM。
- `components/BSP/SD_CARD/sd_card_config.h`：SDMMC/SPI 模式、GPIO、总线频率。当前默认 SDMMC 1-bit 40 MHz，SPI fallback 20 MHz。
- `main/audio.c`、`main/audio.h`：ES8311 I2C 端口/地址、I2S GPIO、采样率、位宽和功放控制。
- `main/app_uart.c`：UART1 引脚、波特率、行结束符和 USB Serial-JTAG 接收路径。UART0 业务接收当前关闭，不要为了“兼容”重新启用 UART0。
- `partitions.csv`：Flash 容量、应用和 `storage` 媒体分区布局。修改后必须重新确认烧录地址和工具配置。
- `sdkconfig.defaults`：开发版功能、FATFS CODEPAGE_936、PSRAM、USB Serial-JTAG 等默认选项。
- `sdkconfig.production.defaults`：生产安全配置。它和开发版必须分开维护，不能用开发配置替代生产配置。

新增或修改引脚后，同时更新网络表对应的硬件笔记、`CURRENT_IMPLEMENTATION.md`、`README.md` 和相关 UART/安全文档。

## 5. 启动和任务结构

当前 `app_main` 的主要顺序是：

1. 初始化双 LCD 和背光。
2. 初始化 SD 卡并执行启动门控/授权检查（生产安全流程启用时）。
3. 初始化 GBK 字库；SD 卡中的 `/SYSTEM/FONT/GBK16.FON` 用于中文文件名显示，启动提示使用内嵌字库兜底。
4. 初始化 ES8311 和 I2S，启动音频服务任务（CPU1）。
5. 初始化 UART1 与 USB Serial-JTAG，两条链路共用一套命令解析代码、独立接收和独立响应。
6. 若 Flash 媒体可用，自动启动默认 Flash 视频。

主循环由 1 ms 软件 tick 驱动，采用协作式 5-slot 工作区。视频显示、图片状态机和 UART 不应加入长时间阻塞循环；音频服务按约 5 ms 周期运行。更换 CPU 频率、PSRAM 或 DMA 内存策略后，应重点复测 LCD、JPEG 和音频并发。

## 6. 媒体和文件系统约束

- Flash 分区名为 `storage`，用于预置 AVI/MJPEG/JPEG 媒体和索引。
- Flash 指令：`VLIST`、`VPLAY`、`FIMGLIST`、`FIMG`。
- SD 卡递归媒体指令：`VIDLIST`、`VID`、`IMGLIST`、`IMG`、`ALIST`、`APLAY`；`SDLIST` 只浏览 SD 根目录并显示到 LCD。
- JPEG 必须是 Baseline `.jpg/.jpeg`，最大 1 MiB，最大 320×320；不支持 Progressive JPEG。
- 视频必须是最大 320×320 的 MJPEG AVI。当前 AVI 播放器只使用视频帧，AVI 内的音频块会跳过。
- 音频服务支持 MP3；PCM 输入按当前实现使用 16-bit、mono、16 kHz 约束。ES8311 播放链路默认 44.1 kHz、16-bit。
- 解码帧缓冲优先放 PSRAM；LCD DMA 条带缓冲必须放内部 SRAM。重复执行 `VID` 时若出现 `ESP_ERR_NO_MEM`，先确认上一次播放器和 DMA 缓冲已停止/释放，再检查内部 DMA 内存余量。
- FATFS 使用 CODEPAGE_936，媒体路径内部为 GBK。中文显示乱码时先检查字库文件、FATFS 配置和 UART 输出编码，不要直接改文件名字节。

## 7. UART 和 USB 命令链路

### 电气链路

- UART1 连接 CA51，是产品业务链路；USB Serial-JTAG 连接电脑，是调试/测试链路。
- 两条链路同时在线、互不阻塞，共用同一命令解析器。UART0 不作为业务接口。
- 默认格式为 115200、8 数据位、无校验、1 停止位、无流控。命令以 LF 或 CRLF 结束，正文使用 ASCII 指令和参数。
- USB 响应会加 `JTAG ` 前缀，便于与 UART1 输出区分；UART1 响应不加该前缀。
- UART1 默认响应 GBK，USB 默认响应 UTF-8。`ENC UTF8`、`ENC GBK` 只切换当前链路，`ENC?` 查询当前链路编码。

### 建议移植验收命令

```text
STATUS
VLIST
VIDLIST
VID 1
VSTOP
IMGLIST
IMG 1
ALIST
APLAY 1
ASTOP
BL 50
ENC?
RST
```

应分别从 USB 和 CA51 UART1 发送并确认响应来源。`VIDLIST/IMGLIST/ALIST` 会递归扫描 SD 子目录，输出较多时要确认电脑端监视器使用正确波特率和 UTF-8/GBK 解码。

## 8. 编译、烧录和媒体写入

### 开发版

在 ESP-IDF v5.4.4 环境中执行：

```bash
idf.py build
idf.py -p COMx flash monitor
```

Linux 下端口通常为 `/dev/ttyUSB0` 或 `/dev/ttyACM0`；Windows 下使用设备管理器确认实际 COM 号。若工程已有不同生成器的 `build/`，请使用新的构建目录（例如 `-B build-porting`），不要混用旧的 CMake 缓存。

### Flash 媒体

Linux：

```bash
./tools/linux/convert_mp4_to_avi.sh input.mp4
./tools/linux/flash_video.sh output.avi
```

Windows：

```powershell
.\tools\windows\flash_video.ps1
```

Windows 脚本读取 `tools/windows/flash_video.conf`，只写入 `storage` 媒体分区，不负责烧录应用、分区表或 eFuse。确认 `FILE/FILES` 没有重复列出同一文件，并在脚本输出中核对实际烧录地址和大小。

### 生产版

生产构建必须使用独立目录和配置，例如：

```bash
idf.py -B build-production \
  -D SDKCONFIG=sdkconfig.production \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.production.defaults' \
  build
```

生产安全配置、签名密钥、Flash Encryption、Secure Boot V2 和一次性 SD 授权流程请严格按 `SECURITY_PROVISIONING.md` 和 `TODO.md` 执行。当前开发配置关闭了这些保护，不能把开发固件当作量产固件；在没有明确硬件和密钥审批前，禁止烧写 eFuse。

## 9. 移植验收清单

- [ ] 核对 V1.4 网络表和原理图，确认所有 GPIO、电平、上拉和电源域。
- [ ] 两块 LCD 均能初始化、显示纯色和图片；确认 RGB565 色序、方向和无 TE 设计。
- [ ] 背光 `BL 0/50/100` 能调节，GPIO1 不被其他外设占用。
- [ ] SD 卡在上电前插入、上电后插入、无卡启动三种场景均符合预期；40 MHz 下连续读写稳定。
- [ ] I2C 扫描能发现 ES8311 0x18；示波器确认 SDA/SCL 高低电平和时序；播放音频时功放 GPIO2 正确打开/静音。
- [ ] USB 和 UART1 可同时接收命令，响应不会串链路，UART0 不产生业务响应。
- [ ] `VID`、`IMG`、`APLAY` 重复切换后无 OOM、死锁或黑屏；检查 `esp_reset_reason()` 日志。
- [ ] 长时间播放视频并改变背光亮度，确认无 Brownout、看门狗复位或 USB 断连。
- [ ] 中文文件名在 SD、Flash 索引和两种 UART 编码下均可验证。
- [ ] 生产构建通过安全预检后再进入密钥/eFuse 工序，开发板和量产板严格分开。

## 10. 常见问题定位

### 开机黑屏或插卡后黑屏

先检查 LCD RESET、CS1/CS2、D/C、WR 和背光 GPIO1，再确认 SD 初始化是否阻塞启动流程。插入 SD 卡后黑屏通常不是“文件名乱码”本身，而是 SD 供电、引脚复用、初始化超时或启动门控路径异常；应同时查看 SD、LCD 和 `app` 的启动日志。

### UART 无响应或监视器提示写超时

确认端口没有被烧录器/其他串口程序占用，波特率为 115200，命令末尾确实发送 LF/CRLF。USB 端发送到 Serial-JTAG，CA51 端发送到 UART1；不要把 UART0 当成业务口。大量 `VIDLIST` 输出时，电脑端解码器可能显示乱码或丢行，但不代表设备没有执行。

### ES8311 未检测到

确认 SDA=GPIO4、SCL=GPIO5、3.3 V 上拉、共地、地址 0x18 和芯片复位/供电。先看 I2C 探测日志，再看 I2S；没有通过 I2C 探测时，后续音频初始化失败是预期结果。SCL 被强制拉低只能用于硬件排查，测试结束要恢复开漏 I2C 配置。

### Brownout 或随机重启

启动最早阶段记录 `esp_reset_reason()`。若为 Brownout，优先检查 USB/3.3 V 电源压降、功放和背光负载、SD 与 LCD 并发电流及地回路；不要只通过降低 BL 掩盖供电问题。若为看门狗或异常崩溃，再分析任务栈、水位和完整 panic 回溯。

### 重复 VID 报 `ESP_ERR_NO_MEM`

确认播放器停止流程已完成，JPEG 解码任务已退出，LCD DMA 回调不再引用条带缓冲；然后检查内部 SRAM/DMA 堆，而不是只看 PSRAM 总堆。必要时降低条带高度或缓冲数量，但要重新验证帧率和撕裂。

## 11. 交付记录

完成移植后，记录目标板版本、实际 GPIO 表、Flash/PSRAM 型号、ESP-IDF 版本、构建配置、烧录地址、测试命令、复位原因和硬件测量结果。若与 V1.4 有差异，在 `CURRENT_IMPLEMENTATION.md` 和硬件设计笔记中明确标注“固件已适配”或“固件待适配”，避免下一次移植继续使用过时假设。
