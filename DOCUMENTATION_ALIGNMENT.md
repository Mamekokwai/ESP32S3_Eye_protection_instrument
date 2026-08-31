# 文档一致性清单（2026-08-31）

## 仓库当前文档

| 文档 | 状态 | 作用 |
|---|---|---|
| `CURRENT_IMPLEMENTATION.md` | 当前基准 | 硬件、总线、媒体、分区事实表 |
| `README.md` | 已对齐 | 构建、目录、引脚、媒体限制 |
| `UART_COMMANDS.md` | 已对齐 | 全部当前指令、递归路径、并发规则 |
| `SECURITY_PROVISIONING.md` | 新增 | 密钥分工、令牌、生产构建、不可逆烧录与验收 |
| `info/Map/media_pipeline_archify.md` | 已对齐 | 媒体与生产启动链路 |
| `info/Map/sd-resource-rule.architecture.json` + HTML | 已同步 | TF 递归目录及当前工具路径 |
| `CLAUDE.md` / `CODEBUDDY.md` / `AGENTS.md` | 已对齐 | 开发助手入口，指向统一基准 |

## Obsidian 项目文档

目录：`E:\Note\Obsidian\笔记\开发\嵌入式\项目\2026\0604眼保仪_ESP32S3_320x320`

已重写为当前事实：

- `项目简介.md`
- `软件设计/ESP 32-S 3 UART 指令手册.md`
- `软件设计/SDMMC.md`
- `软件设计/TF卡配置要点.md`
- `软件设计/生产安全与一次性解锁.md`
- `环境配置/烧录方法.md`
- `硬件设计/电气引脚V1.0.md`

已补充当前安全构建边界：`环境配置/环境配置检查指南.md`。

以下保留历史内容，但已显式标记为历史/归档，不能作为当前接线或固件依据：

- `开发经验.md`：时间序列调试日志。
- `硬件设计/元器件选择.md`：早期裸片/QSPI/MAX98357 选型。
- `硬件设计/开发板和WA54TE057I-20Z的连接.md`：早期开发板 QSPI 验证。
- `硬件设计/WA54TE057I-20Z脚位连接详细说明.md`：早期 QSPI 原理图教程。
- `ISSUE/ESP32-S3 SPI 模式下 TF 卡只能跑 20 MHz，大于20 MHz 卡初始化失败.md`：仅适用 SPI fallback；当前正常路径是 SDMMC 1-bit 40 MHz。

RCA 文档保留故障发生时的上下文。原空白的 `问题分析/AVI 和 raw 在不同图像大小下的帧率分析.md` 已补成“待实机数据”的测量计划，避免把缺失数据当成结论。

## 已消除的主要冲突

- QSPI/单屏/无 TF/MAX98357 历史方案与当前 i80 双屏/SDMMC/ES8311 混写。
- TF 当前协议写成 SPI 20 MHz、媒体仅根目录。
- LCD WR/TE/RESET、TF CMD/D0、UART TX、USB 引脚角色错误。
- 文档存在不存在的 `BL`、USB 业务输入、`tools/convert.sh`。
- `idf.py flash` 被误写为自动进入 monitor。
- 开发与生产分区偏移混用；生产签名 bootloader 空间不足。
- 把 SD 上的私钥误当作安全解锁方案；现改为离线私钥签发设备绑定令牌。
