/**
 * @file cap_prompt_template.h
 * @brief prompt_template Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_prompt_template_init(void);
esp_err_t cap_prompt_template_deinit(void);
#ifdef __cplusplus
}
#endif
