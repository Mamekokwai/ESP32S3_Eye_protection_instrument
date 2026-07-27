# ESP32-S3 UART 指令手册

本固件通过 UART1 接收 CA51F352P4 指令。指令为 ASCII 文本，大小写不敏感，以 `\n` 或 `\r\n` 结束；参数之间使用空格分隔。响应从调试串口输出，每条响应以 `\r\n` 结束。

## 通信参数

| 项目 | 配置 |
|---|---|
| 波特率 | 115200 |
| 格式 | 8 数据位、无校验、1 停止位 |
| ESP32 RX | GPIO44 |
| 数据方向 | CA51 → ESP32；ESP32 返回调试/状态文本 |

## 视频指令

| 指令 | 说明 | 成功响应 |
|---|---|---|
| `VPLAY` | 播放 Flash 中的 AVI/MJPEG 视频 | `OK VPLAY` |
| `VPAUSE` | 暂停当前视频 | `OK VPAUSE` |
| `VRESUME` | 继续暂停的视频 | `OK VRESUME` |
| `VSTOP` | 停止视频并回到空闲 | `OK VSTOP` |

视频播放器只显示 MJPEG 图像，AVI 内的音频块会被跳过，不输出声音。

## TF 卡与图片

| 指令 | 说明 |
|---|---|
| `SDLIST [page]` | 在 LCD 上分页显示 TF 卡根目录 |
| `IMGLIST` | 返回根目录中的 `.jpg/.jpeg` 文件及序号 |
| `IMG <N>` | 显示第 N 个 JPEG 文件 |
| `IMG <filename.jpg>` | 按文件名显示 JPEG |

`IMG` 开始时先返回 `OK IMG loading`，完成后返回 `OK IMG name widthxheight`。支持 Baseline JPEG，文件大小不超过 1 MiB，解码尺寸不超过 320×320；小图自动居中，Progressive JPEG 不支持。图片序号按 FAT 目录原始遍历顺序生成。

## 音频指令

| 指令 | 说明 | 成功响应 |
|---|---|---|
| `ALIST` | 列出 `.pcm` 和 `.mp3` 文件 | `ALIST` 后跟列表 |
| `APLAY <N/filename>` | 播放指定音频 | `OK APLAY` |
| `ASTOP` | 停止音频 | `OK ASTOP` |
| `AMUTE` | 切换静音 | `OK AMUTE on/off` |
| `VOL <0-100>` | 设置音量 | `OK VOL <value>` |

音频状态与显示状态独立：`APLAY` 不会停止视频或取消图片加载，`VPLAY`、`IMG`、`SDLIST` 也不会停止音频。视频和图片共用 LCD，因此二者仍互斥。音频运行于 CPU1 独立任务，显示调度运行于 CPU0；各媒体独立播放，不做音画时间轴同步。`SLEEP` 会停止显示和音频。

## 系统指令

| 指令 | 说明 |
|---|---|
| `STATUS` | 分别查询显示状态和当前音频文件 |
| `INFO` | 查询剩余堆内存 |
| `SLEEP` / `WAKE` | 进入/退出休眠画面 |
| `RST` | 重启 ESP32 |

## 错误响应

格式通常为 `ERR <reason>`，例如 `ERR no sd`、`ERR no such file`、`ERR IMG ESP_ERR_INVALID_SIZE`。未知指令返回 `ERR unknown: <command>`。

## 使用示例

```text
IMGLIST\n
IMG 1\n
APLAY music.mp3\n
STATUS\n
VSTOP\n
ASTOP\n
```
