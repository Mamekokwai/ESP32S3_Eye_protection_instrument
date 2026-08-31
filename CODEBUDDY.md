# Repository assistant context

本项目是 YT06 V1.1 的 ESP-IDF v5.4.4 固件。开发规范见 [AGENTS.md](AGENTS.md)，当前硬件、总线、媒体管线和分区事实以 [CURRENT_IMPLEMENTATION.md](CURRENT_IMPLEMENTATION.md) 为准。

- 构建、引脚和架构：[README.md](README.md)
- UART 完整协议：[UART_COMMANDS.md](UART_COMMANDS.md)
- 生产 Secure Boot、Flash Encryption 与一次性 SD 授权：[SECURITY_PROVISIONING.md](SECURITY_PROVISIONING.md)

旧 QSPI、MAX98357、无 TF 卡方案是历史资料，不得当作当前实现。`VIDLIST`、`IMGLIST`、`ALIST` 递归扫描 TF 子目录，只有 `SDLIST` 仍浏览根目录。普通开发任务不得烧录生产安全配置或写 eFuse。
