# TODO

## 量产前安全门禁

以下事项全部完成并留存验收记录后，才允许烧录生产镜像、启用安全 eFuse 或进入量产：

- [ ] 建立独立的生产构建目录和配置：使用全新的 `build-production`，显式启用 Secure Boot V2、Release Flash Encryption、NVS 加密和 `EYECARE_PRODUCTION_LOCK`；开发配置不得用于量产。
- [ ] 关闭生产固件的 USB Serial-JTAG、调试控制台和 `GPIO4`/`GPIO5`/`I2CTEST`/`I2CFIX` 等调试指令，仅保留必要的 UART1 业务链路。
- [ ] 重新设计解锁令牌：按设备绑定（或至少按批次隔离），增加有效期/撤销机制，避免一张令牌卡解锁全部设备。
- [ ] 将 Secure Boot RSA 私钥和解锁 ECDSA 私钥迁移至离线签名机或 HSM；禁止明文私钥进入仓库、固件、TF 卡和普通开发机长期存储。
- [ ] 固定并审核批准的 Secure Boot/解锁公钥指纹；预检和产线脚本必须校验指纹，不能只验证“本地私钥与固件公钥相互匹配”。
- [ ] 加强 `production_preflight.py`：拒绝额外、重复和重叠分区，检查分区边界及加密标志，并将预检设为发布流水线硬门禁。
- [ ] 验证生产镜像的 bootloader、应用签名、分区偏移、Flash 加密状态和 Secure ROM Download Mode；确保 `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y` 并通过预检。
- [ ] 在专用报废/样板板上验证首次启动、令牌成功、令牌错误、掉电恢复、重复上电、无卡启动和 eFuse 回读；确认流程不会烧错板或导致不可恢复砖机。
- [ ] 量产烧录工具禁止复用开发版 `flash_video` 流程；验证加密后 `storage` 写入、媒体更新策略和无 OTA 分区的维护方案。
- [ ] 建立密钥、eFuse、设备 MAC/序列号、固件哈希和令牌发放的离线审计记录，并保留至少两份加密备份。

参考：[`SECURITY_PROVISIONING.md`](SECURITY_PROVISIONING.md)。
