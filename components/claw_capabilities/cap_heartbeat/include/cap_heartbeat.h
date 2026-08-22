/**
 * @file cap_heartbeat.h
 * @brief heartbeat Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_heartbeat_init(void);
esp_err_t cap_heartbeat_deinit(void);
#ifdef __cplusplus
}
#endif
