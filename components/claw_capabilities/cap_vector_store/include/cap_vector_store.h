/**
 * @file cap_vector_store.h
 * @brief vector_store Capability for Neon-Claw
 * @author maruf009sultan
 */
#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t cap_vector_store_init(void);
esp_err_t cap_vector_store_deinit(void);
#ifdef __cplusplus
}
#endif
