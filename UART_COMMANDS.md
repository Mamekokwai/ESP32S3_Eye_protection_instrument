# ESP32-S3 UART 指令手册

当前通信架构：CA51 使用 UART1（RX=GPIO44、TX=GPIO43）与 ESP32 双向通信；电脑使用原生 USB Serial-JTAG 双向调试。两路独立缓冲、共用命令解析器，响应返回各自来源通道；UART0 不参与业务通信。

USB Serial-JTAG 返回的协议响应统一带有 `JTAG ` 前缀（例如 `JTAG OK VID 1.avi`）；UART1 返回给 CA51 的响应不带此前缀。系统日志仍保持原有 `I/W/E` 格式。

本固件同时通过 UART1 和原生 USB Serial/JTAG 接收指令。指令关键字为 ASCII 文本，大小写不敏感，以 `\n` 或 `\r\n` 结束；参数之间使用空格分隔。响应只返回发出该指令的链路，每条响应以 `\r\n` 结束。

## 通信参数

| 项目 | 配置 |
| --- | --- |
| 电气电平 | 3.3 V TTL UART，空闲状态为高电平；不是 RS-232 电平 |
| 串口格式 | 115200 bit/s、8 数据位、无校验、1 停止位、无流控 |
| ESP32 指令输入 | GPIO44，UART1 RX |
| ESP32 对 CA51 的响应输出 | GPIO43，UART1 TX |
| 原生 USB | GPIO19 D-、GPIO20 D+，Linux `/dev/ttyACM*` |
| 数据方向 | UART1、USB 均双向；响应返回指令来源链路 |
| 默认响应编码 | UART1：GBK；USB Serial-JTAG：UTF-8 |

GPIO19/20 原生 USB Serial/JTAG 在 Linux 上通常为 `/dev/ttyACM0`，可用于烧录、日志监控和输入 `VPLAY`、`BL` 等业务指令。UART1 与 USB 使用独立命令缓冲，可同时接收。

## 响应文本编码

TF 卡使用 FATFS CODEPAGE_936，目录名和文件名在固件内部为 GBK 字节。固件可在每条通信链路输出前将 GBK 中文转换成 UTF-8；ASCII 内容在两种模式下完全相同。

| 指令 | 说明 | 成功响应 |
| --- | --- | --- |
| `ENC` / `ENC?` | 查询当前链路的输出编码 | `OK ENC UTF8` 或 `OK ENC GBK` |
| `ENC UTF8` / `ENC UTF-8` | 将当前链路的协议响应设为 UTF-8 | `OK ENC UTF8` |
| `ENC GBK` | 将当前链路的协议响应设为 GBK | `OK ENC GBK` |

UART1 与 USB 的编码模式相互独立：UART1 默认 GBK，以兼容 CA51；USB Serial-JTAG 默认 UTF-8，方便 VS Code/ESP-IDF Monitor 正确显示中文。配置只在本次运行中有效，ESP32 重启后恢复默认值。`ENC` 只影响协议响应（包括 `VIDLIST`、`IMGLIST`、`ALIST` 中的中文路径），不改变 ESP-IDF 的 `I/W/E` 系统日志编码。

该功能是输出编码转换，不会转换接收到的文件名参数。控制端直接发送中文路径时仍需使用固件内部的 GBK 字节；跨终端调试建议使用 `VID 1`、`IMG 1`、`APLAY 1` 等数字序号。

## 主控芯片接入说明

### 硬件连接

外部主控只负责发送指令时，至少连接主控 TX 和公共 GND；需要读取执行结果时再连接主控 RX：

```text
主控 TX  ──────────────> ESP32 GPIO44（指令 RX）
主控 RX  <────────────── ESP32 GPIO43（响应及日志 TX，可选）
主控 GND ─────────────── ESP32 GND
```

YT06 V1.1 板上已经完成以下连接，编写 CA51F352P4 程序时无需另外飞线：

| CA51F352P4 | ESP32-S3 | 用途 |
| --- | --- | --- |
| U7-19，TX0 | GPIO44 | CA51 向 ESP32 发送指令 |
| U7-18，RX0 | GPIO43 | CA51 接收 ESP32 响应和调试日志 |
| U7-20，GND | GND | 两颗芯片共地 |

两颗芯片均使用 3.3 V 逻辑。主控 TX 必须接 ESP32 RX，不能将两个输出脚直接相连；不要向 GPIO43 驱动信号。GPIO19/20 是原生 USB，不是主控 UART 引脚；GPIO0 是启动配置脚，也不要用于本协议。

### 主控发送流程

1. 将主控 UART 配置为 `115200-8N1`，关闭硬件流控。
2. ESP32 的 UART 接收在外设初始化完成后才启用。两颗芯片同时上电时，建议主控等待约 3 秒再发送第一条指令。
3. 发送一条 ASCII 指令，最后追加换行字节 `0x0A`。也支持以 `0x0D` 或 `0x0D 0x0A` 结束。
4. 不需要发送 C 字符串结尾的 `0x00`。单条指令不要超过 127 个字符。
5. 如需确认执行结果，等待一行 `OK ...` 或 `ERR ...` 后再发送下一条指令。

通用 C 伪代码示例（`uart_send_*` 请替换为主控 SDK 的发送接口）：

```c
static void esp32_send_command(const char *command)
{
    uart_send_bytes((const uint8_t *)command, strlen(command));
    uart_send_byte('\n');
}

esp32_send_command("BL 50");
esp32_send_command("VPLAY 1");
esp32_send_command("APLAY 1");
```

例如 `BL 50\n` 实际发送的十六进制字节为：

```text
42 4C 20 35 30 0A
```

GPIO43 除协议响应外还会输出 ESP-IDF 启动和运行日志。主控接收程序应按换行分帧，并只处理下列前缀；其他行可以忽略：

```text
OK ...
ERR ...
STATUS ...
INFO ...
VLIST / VIDLIST / ALIST / FIMGLIST / IMGLIST
```

若主控不需要反馈，可以不连接 RX，仅发送带换行的指令。

## 视频指令

### Flash 视频

| 指令 | 说明 | 成功响应 |
| --- | --- | --- |
| `VLIST` | 列出 Flash 中的 AVI 视频及序号 | `VLIST` 后跟列表 |
| `VPLAY` | 播放 Flash 中第 1 个 AVI | `OK VPLAY <filename>` |
| `VPLAY <N>` | 播放 Flash 中第 N 个 AVI | `OK VPLAY <filename>` |
| `VPLAY <filename.avi>` | 按文件名播放 Flash AVI | `OK VPLAY <filename>` |

### TF 卡视频

| 指令 | 说明 | 成功响应 |
| --- | --- | --- |
| `VIDLIST` | 列出 TF 卡根目录中的 AVI 视频及序号 | `VIDLIST` 后跟列表 |
| `VID <N>` | 播放 TF 卡中第 N 个 AVI | `OK VID <filename>` |
| `VID <filename.avi>` | 按文件名播放 TF 卡 AVI | `OK VID <filename>` |

### 通用视频控制

| 指令 | 说明 | 成功响应 |
| --- | --- | --- |
| `VPAUSE` | 暂停当前 Flash 或 TF 视频 | `OK VPAUSE` |
| `VRESUME` | 继续当前暂停的视频 | `OK VRESUME` |
| `VSTOP` | 停止当前 Flash 或 TF 视频并回到空闲 | `OK VSTOP` |

Flash 和 TF 视频均要求 AVI/MJPEG，最大分辨率为 320×320。播放器只显示 MJPEG 图像；AVI 内的音频块会被跳过，不输出声音。

## 图片与 TF 卡目录

### Flash 图片

| 指令 | 说明 | 成功响应 |
| --- | --- | --- |
| `FIMGLIST` | 列出 Flash 中的 JPEG 图片及序号 | `FIMGLIST` 后跟列表 |
| `FIMG <N>` | 显示 Flash 中第 N 个 JPEG | `OK FIMG loading` |
| `FIMG <filename.jpg>` | 按文件名显示 Flash JPEG | `OK FIMG loading` |

`FIMG` 加载完成后会再次返回 `OK FIMG name widthxheight`。

### TF 卡目录与图片

| 指令 | 说明 |
| --- | --- |
| `SDLIST [page]` | 在 LCD 上分页显示 TF 卡根目录 |
| `IMGLIST` | 返回根目录中的 `.jpg/.jpeg` 文件及序号 |
| `IMG <N>` | 显示第 N 个 JPEG 文件 |
| `IMG <filename.jpg>` | 按文件名显示 JPEG |

`IMG` 开始时先返回 `OK IMG loading`，完成后返回 `OK IMG name widthxheight`。支持 Baseline JPEG，文件大小不超过 1 MiB，解码尺寸不超过 320×320；小图自动居中，Progressive JPEG 不支持。图片序号按 FAT 目录原始遍历顺序生成。

## 音频指令

| 指令 | 说明 | 成功响应 |
| --- | --- | --- |
| `ALIST` | 列出 `.pcm` 和 `.mp3` 文件 | `ALIST` 后跟列表 |
| `APLAY <N/filename>` | 播放指定音频 | `OK APLAY` |
| `ASTOP` | 停止音频 | `OK ASTOP` |
| `AMUTE` | 切换静音 | `OK AMUTE on/off` |
| `VOL <0-100>` | 设置音量 | `OK VOL <value>` |

音频状态与显示状态独立：`APLAY` 不会停止视频或取消图片加载，`VPLAY`、`IMG`、`SDLIST` 也不会停止音频。视频和图片共用 LCD，因此二者仍互斥。音频运行于 CPU1 独立任务，显示调度运行于 CPU0；各媒体独立播放，不做音画时间轴同步。`SLEEP` 会停止显示和音频。

## 背光指令

| 指令 | 说明 | 成功响应 |
| --- | --- | --- |
| `BL <0-100>` | 设置 LCD 背光亮度百分比；`0` 为关闭 | `OK BL <value>` |
| `BL` / `BL?` | 查询当前设定亮度 | `OK BL <value>` |

背光由 GPIO1 的 20 kHz LEDC 硬件 PWM 驱动。`SLEEP` 会关闭实际 PWM 输出但保留设定值；休眠期间仍可用 `BL` 修改待恢复亮度，`WAKE` 后按该亮度恢复。

## 系统指令

| 指令 | 说明 |
| --- | --- |
| `STATUS` | 查询显示、音频和背光亮度状态 |
| `INFO` | 查询剩余堆内存 |
| `SLEEP` / `WAKE` | 进入/退出休眠画面 |
| `RST` | 重启 ESP32 |

## 工程调试指令

以下指令仅用于硬件诊断，不应写入 CA51F352P4 的量产控制流程：

| 指令 | 作用 | 风险 |
| --- | --- | --- |
| `GPIO5` / `G5` | 翻转 GPIO5 电平 | GPIO5 是 ES8311 I2C SCL，会中断音频控制 |
| `GPIO5 <0/1>` | 强制设置 GPIO5 并回读引脚实际电平 | 会把 I2C SCL 改成普通推挽输出 |
| `GPIO4 <0/1>` | 强制设置 GPIO4 并回读引脚实际电平 | GPIO4 是 ES8311 I2C SDA，会破坏 I2C 通信 |
| `I2CTEST` | 启动或停止约 1 kHz 的 ES8311 I2C 读测试 | 仅供示波器观察，增加总线负载 |
| `I2CFIX` | 对 I2C 总线执行 GPIO 恢复脉冲 | 会暂时接管 SDA/SCL 引脚 |

误发 `GPIO4`、`GPIO5` 或 `I2CFIX` 后，音量控制和音频初始化可能失效；正常业务只使用前述视频、图片、音频、背光和系统指令。

## 错误响应

格式通常为 `ERR <reason>`，例如 `ERR no sd`、`ERR no such file`、`ERR IMG ESP_ERR_INVALID_SIZE`。未知指令返回 `ERR unknown: <command>`。

## 使用示例

```text
VLIST\n
VPLAY 1\n
VIDLIST\n
VID 1\n
FIMGLIST\n
FIMG 1\n
IMGLIST\n
IMG 1\n
APLAY music.mp3\n
STATUS\n
VSTOP\n
ASTOP\n
```
