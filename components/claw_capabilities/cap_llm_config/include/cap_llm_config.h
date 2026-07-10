/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_LLM_CONFIG_STR_LEN       320
#define CAP_LLM_CONFIG_SHORT_STR_LEN 32
#define CAP_LLM_CONFIG_MODEL_LEN     64
#define CAP_LLM_CONFIG_TIMEOUT_LEN   16

typedef struct {
    char api_key[CAP_LLM_CONFIG_STR_LEN];
    char backend_type[CAP_LLM_CONFIG_SHORT_STR_LEN];
    char model[CAP_LLM_CONFIG_MODEL_LEN];
    char base_url[CAP_LLM_CONFIG_STR_LEN];
    char auth_type[CAP_LLM_CONFIG_SHORT_STR_LEN];
    char timeout_ms[CAP_LLM_CONFIG_TIMEOUT_LEN];
    char max_tokens[CAP_LLM_CONFIG_TIMEOUT_LEN];
    char default_image_max_bytes[CAP_LLM_CONFIG_TIMEOUT_LEN];
    char max_tokens_field[CAP_LLM_CONFIG_SHORT_STR_LEN];
    char supports_tools[8];
    char supports_vision[8];
    char image_remote_url_only[8];
} cap_llm_config_t;

typedef esp_err_t (*cap_llm_config_get_fn)(cap_llm_config_t *out_config, void *user_ctx);
typedef esp_err_t (*cap_llm_config_apply_fn)(const cap_llm_config_t *config, void *user_ctx);

typedef struct {
    cap_llm_config_get_fn get_config;
    cap_llm_config_apply_fn apply_config;
    void *user_ctx;
} cap_llm_config_provider_t;

esp_err_t cap_llm_config_set_provider(const cap_llm_config_provider_t *provider);
esp_err_t cap_llm_config_register_group(void);

#ifdef __cplusplus
}
#endif
