/**
 * @file cap_event_bus.h
 * @brief event_bus Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_event_bus_init(void);
esp_err_t cap_event_bus_deinit(void);
#ifdef __cplusplus
}
#endif
