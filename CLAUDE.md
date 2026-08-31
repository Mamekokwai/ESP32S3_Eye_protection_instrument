# Repository assistant context

This is an ESP-IDF v5.4.4 project for the YT06 V1.4 ESP32-S3 eye-care device. Follow [AGENTS.md](AGENTS.md) for repository rules and use [CURRENT_IMPLEMENTATION.md](CURRENT_IMPLEMENTATION.md) as the current hardware/software fact table.

Important references:

- [README.md](README.md): build, architecture, pinout, and media constraints.
- [UART_COMMANDS.md](UART_COMMANDS.md): complete current UART protocol.
- [SECURITY_PROVISIONING.md](SECURITY_PROVISIONING.md): isolated production build, Secure Boot/Flash Encryption, and one-time shared SD authorization token (not device-bound).

Do not revive historical QSPI/MAX98357/no-TF designs as current behavior. V1.4 hardware has no TE pin: GPIO1 is the backlight PWM (PWM_LED→R17→Q3→LEDK) driven by ESP32 LEDC. TF media lookup is recursive for `VIDLIST`, `IMGLIST`, and `ALIST`; `SDLIST` alone remains a root-directory LCD browser. Never flash production security settings or burn eFuses during an ordinary development task.
