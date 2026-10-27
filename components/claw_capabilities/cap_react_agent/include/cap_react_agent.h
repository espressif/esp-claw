/**
 * @file cap_react_agent.h
 * @brief react_agent Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_react_agent_init(void);
esp_err_t cap_react_agent_deinit(void);
#ifdef __cplusplus
}
#endif
