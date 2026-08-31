# ESP32-S3 UART 指令手册

固件通过 UART0 RX 和 UART1 RX（两者当前均映射 GPIO44）接收 ASCII 指令，115200-8N1，大小写不敏感，以 `\n` 或 `\r\n` 结束。UART0 TX（GPIO43）输出日志和文本响应；可直接在 `idf.py monitor` 中输入命令。UART0 可用时优先处理 UART0，并丢弃 UART1 镜像数据，确保一条命令只执行一次；UART1 仍兼容 CA51F352P4 外部控制器。

接收行缓冲为 544 字节（包含结尾 `\0`）；超长行会被丢弃。响应通常以 `\r\n` 结束。

## 指令表

| 指令 | 作用 |
|---|---|
| `VLIST` | 列出 Flash `storage` 索引中的 AVI |
| `VPLAY [N/filename.avi]` | 播放 Flash AVI；无参数时选默认项 |
| `VIDLIST` | 递归列出 TF 卡中的 `.avi` |
| `VID <N/path.avi>` | 按序号或相对路径播放 TF MJPEG AVI |
| `VPAUSE` / `VRESUME` | 暂停/继续当前 Flash 或 TF 视频 |
| `VSTOP` | 停止当前视频 |
| `FIMGLIST` | 列出 Flash 索引中的 JPEG |
| `FIMG <N/filename.jpg>` | 显示 Flash JPEG |
| `IMGLIST` | 递归列出 TF 卡中的 `.jpg/.jpeg` |
| `IMG <N/path.jpg>` | 按序号或相对路径显示 TF JPEG |
| `ALIST` | 递归列出 TF 卡中的 `.pcm/.mp3` |
| `APLAY <N/path>` | 播放 TF 音频 |
| `ASTOP` | 停止音频 |
| `AMUTE` | 切换静音；功放 GPIO2 随状态切换 |
| `VOL <0-100>` | 设置音量 |
| `BL <0-100>` | 设置 LCD 背光亮度；0 为关闭，100 为全亮 |
| `SDLIST [page]` | 在 LCD 分页浏览 TF 卡根目录，页码从 1 开始 |
| `STATUS` | 查询显示状态和音频状态 |
| `INFO` | 查询剩余堆内存 |
| `SLEEP` / `WAKE` | 进入/退出休眠画面；`SLEEP` 同时停止音频 |
| `RST` | 重启 ESP32 |

诊断指令 `GPIO4 <0/1>`、`GPIO5 [0/1]`（别名 `G5`）、`I2CTEST`、`I2CFIX` 只用于产线/示波器排障，不属于正常产品协议。V1.4 背光由 ESP32 GPIO1（PWM_LED → Q3 → LEDK）控制，默认 100% 亮度；可通过 `BL <0-100>` 调节。

## 媒体选择规则

- `VIDLIST`、`IMGLIST`、`ALIST` 从 TF 卡挂载点递归遍历子目录，忽略 `.` 和 `..`，保存相对路径。
- 数字参数按当前 FAT 遍历顺序选择；增删文件后序号可能变化，产品协议应优先使用稳定的相对路径。
- 路径可以包含中文（GBK；FATFS 用 CODEPAGE_936 + 长文件名）。路径不得逃出挂载点。
- `SDLIST` 是单独的屏幕浏览器，目前只显示 TF 根目录（可用 GBK16 字库显示中文文件名），不等同于递归媒体目录。

## 并发与限制

视频和图片共用 LCD，因此互斥；音频状态独立，可与 `VID`、`VPLAY` 或 `IMG` 并行。系统不做音画时间轴同步。

- TF/Flash 视频仅显示 MJPEG 图像，AVI 音频块被跳过；TF 视频最大 320×320。
- 图片仅支持 Baseline JPEG，文件最大 1 MiB、解码尺寸最大 320×320；小图居中，Progressive JPEG 会失败。
- `IMG`/`FIMG` 先返回 `OK ... loading`，异步完成后再报告文件名和尺寸。

## 响应示例

```text
VIDLIST
VID 1
APLAY 提示音/启动.mp3
STATUS
VPAUSE
VRESUME
VSTOP
ASTOP
```

成功响应以 `OK` 或列表标题开头；失败通常为 `ERR <reason>`，未知指令为 `ERR unknown: <command>`。具体错误字符串以 [main/app_uart.c](main/app_uart.c) 为准。
