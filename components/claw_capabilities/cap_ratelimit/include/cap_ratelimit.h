/**
 * @file cap_ratelimit.h
 * @brief ratelimit Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_ratelimit_init(void);
esp_err_t cap_ratelimit_deinit(void);
#ifdef __cplusplus
}
#endif
