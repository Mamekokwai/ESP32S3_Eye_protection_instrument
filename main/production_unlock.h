#pragma once

#include <stdbool.h>
#include "sdkconfig.h"

/*
 * ============================================================
 * 生产加密解锁系统 总开关（编译期宏）
 * ============================================================
 *  EYECARE_ENABLE_ENCRYPTION = 1 : 启用生产加密解锁
 *      Secure Boot V2 + Flash Encryption Release 门检查
 *      → 一次性 SD 通用令牌验签 → 烧写 EYECARE_UNLOCKED eFuse
 *  EYECARE_ENABLE_ENCRYPTION = 0 : 关闭
 *      production_unlock_ensure() 立即返回 true（开发环境直通）
 *
 *  手动开关（一眼可改）: 改下面这一行的 1/0 即可。
 *  若想跟随 Kconfig（生产构建自动 y、开发构建自动 n），
 *  把下面两行注释互换即可。
 * ============================================================
 */

/* ===== 生产加密开关：跟随 Kconfig =====
 * 开发配置默认关闭；生产配置通过 sdkconfig.production.defaults 打开。
 * 不要在这里硬编码为 1，否则开发版会因缺少安全配置而无法编译。 */
#ifndef CONFIG_EYECARE_PRODUCTION_LOCK
#define CONFIG_EYECARE_PRODUCTION_LOCK 0
#endif
#define EYECARE_ENABLE_ENCRYPTION CONFIG_EYECARE_PRODUCTION_LOCK

/**
 * @brief Block until this device is permanently unlocked.
 *
 * With EYECARE_ENABLE_ENCRYPTION = 0 (or CONFIG_EYECARE_PRODUCTION_LOCK=n)
 * this returns immediately. Otherwise only the SD-card driver and the
 * P-256 signature verifier run until a valid shared /eyecare.unlock token
 * is found. Successful verification burns the one-way EYECARE_UNLOCKED
 * eFuse bit and returns true.
 */
bool production_unlock_ensure(void);
