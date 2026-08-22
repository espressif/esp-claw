/**
 * @file cap_watchdog.h
 * @brief watchdog Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_watchdog_init(void);
esp_err_t cap_watchdog_deinit(void);
#ifdef __cplusplus
}
#endif
