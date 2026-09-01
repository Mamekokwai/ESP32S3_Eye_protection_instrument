# 生产安全与一次性解锁

> 当前状态（硬件测试阶段）：`sdkconfig.production.defaults` 已关闭生产解锁、Secure Boot、Flash Encryption 和 NVS 加密。以下内容是后续量产启用安全功能时的流程说明；在未完成评审前不要烧录生产配置，也不要写入 eFuse。

## 目标与边界

生产镜像首次运行时只挂载 TF 卡并反复验证根目录的 `/eyecare.unlock`。令牌验证成功后烧写不可清除的 `EYECARE_UNLOCKED` eFuse 位，再进入 LCD、音频、UART 和媒体主程序。后续上电直接读取该位，不再需要 TF 卡或令牌。令牌**不绑定设备**：同一张卡可用于解锁所有已量产的设备，只要其 P-256 签名能通过固件内嵌公钥验证。

这套“一次授权”只负责控制工厂首启。真正防止用户从 Flash 取得明文、替换固件绕过检查的机制是同时启用：

- Secure Boot V2：只允许运行由生产 RSA-3072 密钥签名的 bootloader/应用。
- Flash Encryption Release：设备首启生成自身 XTS 密钥并保护在 eFuse 中，加密应用及标记为 `encrypted` 的分区。
- Secure ROM Download Mode：限制量产后的 ROM 下载能力。

`storage` 和 `nvs_keys` 已在 `partitions.csv` 标记为加密。当前方案不能隐藏 TF 卡自身的普通媒体；需要保密的内容应在安全首启前写入 Flash `storage`，随后由每台设备的 Flash Encryption 保护。

## 三类密钥不要混用

| 资产 | 算法 | 放置位置 | 用途 |
|---|---|---|---|
| 解锁签名私钥 | ECDSA P-256 OpenSSH | 工厂离线机/HSM；本地忽略路径 `info/HTML/key/eyecare_unlock_ecdsa_p256` | 为所有设备签发同一把通用解锁令牌 |
| 解锁公钥 | P-256 DER | 编译进 `main/unlock_public_key.h` | 设备验证令牌，不是秘密 |
| Secure Boot 私钥 | RSA-3072 PEM | 工厂离线机/HSM；本地忽略路径 `info/HTML/key/secure_boot_signing_key.pem` | 签名 bootloader/应用 |

原有 `info/HTML/key/260604-Embed_EyeCare_ESP32S3_320x320` 是 Ed25519 SSH 密钥。ESP-IDF 5.4.4 自带的 Mbed TLS 不提供本方案所需的 Ed25519 验证实现，因此它没有被修改，也没有放入固件；解锁专用密钥改用 P-256。私钥绝不能复制到 TF 卡、固件、构建产物或版本库。

本次本地生成资产的核对指纹（指纹可记录，私钥不可提交）：

- 原 Ed25519 公钥：`SHA256:eyqHkCD4RKTOLyKfNSRLinyRXML0VZVHXVTZwmN6hpI`（未使用）。
- 解锁 P-256 SSH 公钥：`SHA256:p49XGHPcGxzxww31qsPlvij2BEcKMsw0ZlZiqwHskMg`。
- Secure Boot RSA 公钥 DER SHA-256：`3356759c8f99190cc3c4f31a96cad6e0195ee78419e8c1addd03681887c27da4`。

## 令牌格式

`tools/security/unlock_token.py` 生成固定载荷和 DER ECDSA 签名：

```text
magic(8) | version(1) | key_id(1) | reserved(2) |
random_nonce(16) | signature_length(1) | signature(≤72)
```

签名覆盖前 28 字节。令牌**不包含设备标识**，也不与 MAC 绑定：任何设备只要用固件内嵌的 P-256 公钥验签通过即接受，因此同一张卡可解锁所有设备。随机 nonce 使重复签发不会产生相同文件；永久状态由 eFuse 位决定，令牌无需留在设备中。该方案的信任根是解锁签名私钥本身——私钥泄露即等于解锁卡可被任意伪造。

## 离线构建

开发构建保持原样：

```bash
idf.py build
```

生产构建必须隔离 `sdkconfig`：

```bash
idf.py -B build-production \
  -D SDKCONFIG=sdkconfig.production \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.production.defaults' \
  build
```

生产配置把分区表移至 `0x10000`，为约 40 KiB 的签名 bootloader 留出空间；因此生产应用从 `0x20000`、`storage` 从 `0x120000` 开始。必须使用 `build-production/partition_table/partition-table.bin` 的实际偏移，不能套用开发构建偏移。

构建后核查 `sdkconfig.production` 至少含：

```text
CONFIG_EYECARE_PRODUCTION_LOCK=y
CONFIG_SECURE_BOOT_V2_ENABLED=y
CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME=y
CONFIG_SECURE_FLASH_ENCRYPTION_AES256=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=y
CONFIG_PARTITION_TABLE_OFFSET=0x10000
```

在已激活 ESP-IDF 环境中运行只读预检（不会访问设备）：

```bash
python tools/security/production_preflight.py \
  --build-dir build-production \
  --sdkconfig sdkconfig.production
```

它会核对安全配置、生产分区表、P-256/RSA-3072 类型、公钥嵌入一致性、私钥忽略规则、二进制私钥标记和 bootloader/应用 RSA 签名。实际 HSM 流程应由产线适配同等检查，不能因改用 HSM 而省略验签。

## 签发通用解锁令牌

令牌工具依赖 Python `cryptography`；在隔离的产线虚拟环境安装并固定经审核的版本，本机 ESP-IDF Python 环境已包含 44.0.3。

在离线签名机签发令牌（**不绑定设备**，可复用于所有量产设备）：

```bash
python tools/security/unlock_token.py issue \
  --private-key info/HTML/key/eyecare_unlock_ecdsa_p256 \
  --output eyecare.unlock
```

可在离线机用公钥复核产物：

```bash
python tools/security/unlock_token.py verify \
  --public-key info/HTML/key/eyecare_unlock_ecdsa_p256.pub \
  --token eyecare.unlock
```

只把 `eyecare.unlock` 放到 FAT/FAT32 TF 卡根目录。卡上没有私钥。同一张卡可插入任意设备完成解锁；已解锁设备（eFuse 位已置位）会直接进入主程序，不再读卡。

## 推荐量产顺序

> 以下步骤会永久烧写安全 eFuse。先用专门的报废风险样板验证，不要在日常开发板直接执行。

1. 记录芯片 MAC、工单序列号、固件哈希、公钥/签名密钥版本和空白 eFuse 报告。
2. 在首次复位前一次性写入生产 bootloader、生产分区表、签名应用和需要保护的明文 `storage` 内容。Secure Boot 构建默认不会由普通 `idf.py flash` 自动写 bootloader，必须按 ESP-IDF 输出的 `--after=no_reset` 专用首次烧录命令/产线脚本执行，整个镜像写完前不要让芯片启动。
3. 首次启动：ESP-IDF bootloader 生成设备唯一 Flash Encryption 密钥、加密指定内容并启用 Secure Boot/Release 安全设置。
4. 应用进入锁定循环；插入含通用解锁令牌的 TF 卡。
5. 日志确认签名通过且 `EYECARE_UNLOCKED` 写入成功，主程序才开始初始化。
6. 断电、移除 TF 卡，再上电；应直接进入主程序。
7. 回读 Flash 抽样确认内容是密文，并确认未签名固件、篡改/伪造令牌均不能运行/解锁。

不要在 Flash Encryption Release 首启完成后用现有 `flash_video.sh` 写入明文 `storage`。Release 模式下后续下载与更新策略必须先在样板上验证；本项目当前没有 OTA 分区。

## 必测故障场景

- 无卡、无文件、截断文件：设备保持锁定并继续重试。
- 签名被改、载荷被改、用其他私钥签发、使用 Ed25519 私钥：拒绝解锁。
- 正确令牌：只写一次 eFuse，随后进入主程序。
- 写入前/写入中掉电：重新上电仍应处于可判定状态；不得以日志判断代替 eFuse 回读。
- 解锁后无卡重启：直接运行。
- 未签名/旧签名应用、明文 Flash 回读、ROM 下载：按量产策略被拒绝或只能看到密文。

## 运维风险

- eFuse 和 Release 安全设置不可逆；烧错设备、烧错密钥或丢失 Secure Boot 私钥可能导致设备无法恢复或升级。
- 丢失解锁私钥将无法为尚未解锁的设备签发新令牌；丢失 Secure Boot 私钥将无法发布受信任的新固件。至少保留两份离线加密备份，并记录指纹和访问审计。
- 一位 eFuse 标志不是内容加密密钥；它依赖 Secure Boot 防止攻击者刷入跳过检查的代码。两项安全功能必须作为同一生产配置启用。
- 本实现会完成开发/生产配置编译检查，但实际 eFuse 时序、掉电恢复、产线烧录工具和硬件兼容性仍必须在样板上验证后才能量产。
