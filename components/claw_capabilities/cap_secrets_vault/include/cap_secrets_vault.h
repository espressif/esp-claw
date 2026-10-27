/**
 * @file cap_secrets_vault.h
 * @brief secrets_vault Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_secrets_vault_init(void);
esp_err_t cap_secrets_vault_deinit(void);
#ifdef __cplusplus
}
#endif
