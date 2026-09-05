# Repository Guidelines

## Project Structure & Module Organization

This is an ESP-IDF v5.4.4 firmware project for an ESP32-S3-WROOM-1 eye-care device with dual 320x320 JD9855 round LCDs, ES8311 audio, SD card storage, and UART control.

- `main/` contains the application state machine, UART command parser, flash/SD video players, audio player, AVI/MJPEG parsing, and embedded test assets.
- `components/BSP/` contains board support drivers for LCD, SDMMC/SPI-fallback storage, and I2C.
- `components/esp_lcd_jd9855/` contains the LCD panel driver and initialization sequence.
- `tools/` contains video conversion and flash-writing helpers.
- `partitions.csv` and `sdkconfig.defaults` define the custom 16 MB flash layout and FreeRTOS tick settings.
- `doc/` stores the maintained project documentation, architecture maps, hardware reference, and historical RCA; `info/` stores board images and hardware source files.

## Build, Test, and Development Commands

Use the ESP-IDF environment from this machine before building:

```bash
source /home/nywerya/esp/v5.4.4/esp-idf/export.fish
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
./tools/linux/convert_mp4_to_avi.sh video.mp4
./tools/linux/flash_video.sh video.avi
```

`idf.py build` compiles firmware and dependencies. `flash monitor` programs the board and opens serial logs. `convert_mp4_to_avi.sh` creates MJPEG AVI files; `flash_video.sh` writes AVI content to the `storage` flash partition before a production device's first encrypted boot.

## Coding Style & Naming Conventions

Use C with 4-space indentation and ESP-IDF conventions. Keep module APIs paired as `.c/.h` files, for example `flash_player.c` with `flash_player.h`. Use lower_snake_case for functions and variables, UPPER_SNAKE_CASE for macros, and concise ESP logging tags. Preserve the cooperative 1 ms tick / 5-slot workspace design; avoid blocking loops in player or UART paths.

## Testing Guidelines

No automated test suite is currently configured. Validate changes with `idf.py build`, then test on hardware through serial monitor commands such as `STATUS`, `VPLAY`, `VIDLIST`, `VID 1`, `VPAUSE`, `VSTOP`, `ALIST`, `APLAY <file>`, `IMGLIST`, and `IMG <file.jpg>`. Verify independent playback with `VID 1` + `APLAY 1`, and `IMG 1` + `APLAY 1`. For LCD changes, check both panels, RGB565 color order, tearing behavior, and PSRAM/DMA effects. JPEG input is limited to Baseline `.jpg/.jpeg`, 1 MiB, and 320×320; TF video is MJPEG AVI up to 320×320.

## Commit & Pull Request Guidelines

Recent history uses concise imperative commits, often with prefixes such as `feat:` plus Chinese descriptions, e.g. `feat: 添加 UART 指令系统`. Keep each commit focused on one hardware or firmware behavior. Pull requests should describe the hardware tested, commands run, UART command coverage, linked issues, and screenshots or serial logs when display or playback behavior changes.

## Security & Configuration Tips

Do not commit generated `build/` output, private media, or device-specific secrets. Recheck GPIO assignments before changing LCD, audio, UART, boot, or TE-related code. GPIO2 is the active-high audio amplifier MUTE/enable pin and must not be used as a heartbeat LED. The current TF path is SDMMC 1-bit at 40 MHz; GPIO0 is not used for normal card transfer. V1.4 硬件无 TE 引脚：GPIO1 是背光 PWM（PWM_LED→R17→Q3→LEDK），由 ESP32 LEDC 驱动。

中文显示：FATFS 用 CODEPAGE_936（`d_name` 为 GBK 双字节），启动提示走内嵌点阵字库（`gbk_embedded_font.h`），SDLIST 中文文件名走 TF 卡 `/SYSTEM/FONT/GBK16.FON`（`gbk_font.c`）。媒体递归路径在固件内部为 GBK；UART1 响应默认 GBK，USB Serial-JTAG 响应默认 UTF-8，可通过 `ENC UTF8|GBK` 为当前链路独立切换。新增/删除字库或改编码前检查 `sdkconfig.defaults` 的 FATFS 配置。

`VIDLIST`, `IMGLIST`, and `ALIST` recursively scan TF subdirectories; `SDLIST` remains a root-directory LCD browser. Production builds additionally use `sdkconfig.production.defaults`, Secure Boot V2, Release-mode Flash Encryption, and the one-time SD authorization flow in `doc/SECURITY_PROVISIONING.md`. Never flash a production build to a development board or burn eFuses without an explicit hardware provisioning step.

## 文档-固件同步规则（每次改硬件/引脚/固件行为必须执行）

硬件与引脚信息以 `Project/YT06_主板1_V1.4/YT06-主板1_V1.4.net`（V1.4 网络表）为权威；固件落后于硬件时，以网络表为准并适配固件。**每次修改引脚分配、外设接线、驱动行为后，必须同步以下文档，不得只改代码：**

1. **Obsidian 硬件笔记**：`E:\Note\Obsidian\笔记\开发\嵌入式\项目\2026\0604眼保仪_ESP32S3_320x320\硬件设计\关键硬件网络连接.md` —— 更新对应脚位表/链路/差异标注，`updated` 日期同步更新。
2. **仓库事实表**：`doc/CURRENT_IMPLEMENTATION.md` —— 更新硬件与总线表中受影响的行，删除过时的旧表述。
3. **Obsidian 软件/项目笔记**（若涉及）：UART 指令、媒体、解锁等文档同步更新。
4. 检查 `AGENTS.md`/`CLAUDE.md`/`README.md`/`doc/UART_COMMANDS.md` 中是否有与该改动冲突的旧表述，一并修正。

网络表（V1.1 与 V1.4 不兼容）与固件冲突时：以 V1.4 网络表为准，固件需适配；差异要在 `关键硬件网络连接.md` 中标注 ⚠️ 与"固件待适配/已适配"状态。
