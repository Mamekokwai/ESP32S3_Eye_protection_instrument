#pragma once

/**
 * @brief  软复位到 UART 下载模式
 *
 * 通过 RTC GPIO hold 功能, 在复位期间将 IO0 拉低,
 * ROM bootloader 检测到 IO0=0 → 进入下载模式。
 *
 * 无需按 BOOT 按键, 无需手动接地。
 * 烧录完成后正常上电即可恢复。
 */
void reboot_to_download(void);
