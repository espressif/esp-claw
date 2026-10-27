/**
 * @file cap_context_engine.h
 * @brief context_engine Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_context_engine_init(void);
esp_err_t cap_context_engine_deinit(void);
#ifdef __cplusplus
}
#endif
