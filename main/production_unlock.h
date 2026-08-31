#pragma once

#include <stdbool.h>

/**
 * @brief Block until this device is permanently unlocked.
 *
 * With CONFIG_EYECARE_PRODUCTION_LOCK disabled this returns immediately.
 * Otherwise only the SD-card driver and signature verifier run until a valid,
 * device-bound /eyecare.unlock token is found. Successful verification burns
 * the EYECARE_UNLOCKED eFuse bit and returns true.
 */
bool production_unlock_ensure(void);
