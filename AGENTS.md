# Repository Guidelines

## Project Structure & Module Organization

This is an ESP-IDF v5.4.4 firmware project for an ESP32-S3-WROOM-1 eye-care device with dual 320x320 JD9855 round LCDs, ES8311 audio, SD card storage, and UART control.

- `main/` contains the application state machine, UART command parser, flash/SD video players, audio player, AVI/MJPEG parsing, and embedded test assets.
- `components/BSP/` contains board support drivers for LCD, SD SPI, I2C, keys, LEDs, and legacy expansion hardware.
- `components/esp_lcd_jd9855/` contains the LCD panel driver and initialization sequence.
- `tools/` contains video conversion and flash-writing helpers.
- `partitions.csv` and `sdkconfig.defaults` define the custom 16 MB flash layout and FreeRTOS tick settings.
- `issue/` and `info/` store project notes, board images, and hardware diagnostics.

## Build, Test, and Development Commands

Use the ESP-IDF environment from this machine before building:

```bash
source /home/nywerya/esp/v5.4.4/esp-idf/export.fish
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
./tools/convert.sh video.mp4
./tools/flash_video.sh video.avi
```

`idf.py build` compiles firmware and dependencies. `flash monitor` programs the board and opens serial logs. `convert.sh` creates MJPEG AVI files for SD playback; `flash_video.sh` writes AVI content to the `storage` flash partition after a successful build.

## Coding Style & Naming Conventions

Use C with 4-space indentation and ESP-IDF conventions. Keep module APIs paired as `.c/.h` files, for example `flash_player.c` with `flash_player.h`. Use lower_snake_case for functions and variables, UPPER_SNAKE_CASE for macros, and concise ESP logging tags. Preserve the cooperative 1 ms tick / 5-slot workspace design; avoid blocking loops in player or UART paths.

## Testing Guidelines

No automated test suite is currently configured. Validate changes with `idf.py build`, then test on hardware through serial monitor commands such as `STATUS`, `VPLAY`, `VIDLIST`, `VID 1`, `VPAUSE`, `VSTOP`, `ALIST`, `APLAY <file>`, `IMGLIST`, and `IMG <file.jpg>`. Verify independent playback with `VID 1` + `APLAY 1`, and `IMG 1` + `APLAY 1`. For LCD changes, check both panels, RGB565 color order, tearing behavior, and PSRAM/DMA effects. JPEG input is limited to Baseline `.jpg/.jpeg`, 1 MiB, and 320×320; TF video is MJPEG AVI up to 320×320.

## Commit & Pull Request Guidelines

Recent history uses concise imperative commits, often with prefixes such as `feat:` plus Chinese descriptions, e.g. `feat: 添加 UART 指令系统`. Keep each commit focused on one hardware or firmware behavior. Pull requests should describe the hardware tested, commands run, UART command coverage, linked issues, and screenshots or serial logs when display or playback behavior changes.

## Security & Configuration Tips

Do not commit generated `build/` output, private media, or device-specific secrets. Recheck GPIO assignments before changing LCD, UART, boot, or TE-related code; several pins are shared with hardware-critical functions.
