/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_core.h"
#include "llm/claw_llm_runtime.h"

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *auth_type;
    const char *max_tokens_field;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
    bool supports_tools;
    bool supports_vision;
    bool image_remote_url_only;
} claw_core_llm_config_t;

typedef claw_llm_tool_call_t claw_core_llm_tool_call_t;
typedef claw_llm_response_t claw_core_llm_response_t;

esp_err_t claw_core_llm_init(const claw_core_llm_config_t *config,
                             claw_llm_runtime_t **out_runtime,
                             char **out_error_message);
esp_err_t claw_core_llm_chat_messages(claw_core_handle_t core,
                                      const char *system_prompt,
                                      cJSON *messages,
                                      const char *tools_json,
                                      claw_core_llm_response_t *out_response,
                                      char **out_error_message);
esp_err_t claw_core_llm_infer_media(claw_core_handle_t core,
                                    const claw_llm_media_request_t *request,
                                    char **out_text,
                                    char **out_error_message);
esp_err_t claw_core_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration);
void claw_core_llm_response_free(claw_core_llm_response_t *response);
