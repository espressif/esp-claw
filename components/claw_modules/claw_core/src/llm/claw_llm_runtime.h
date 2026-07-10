/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "llm/claw_llm_types.h"

typedef struct claw_llm_runtime claw_llm_runtime_t;

typedef struct {
    const char *id;
    esp_err_t (*init)(const claw_llm_runtime_config_t *config,
                      const claw_llm_model_profile_t *profile,
                      void **out_backend_ctx,
                      char **out_error_message);
    esp_err_t (*chat)(void *backend_ctx,
                      const claw_llm_model_profile_t *profile,
                      const claw_llm_chat_request_t *request,
                      claw_llm_response_t *out_response,
                      char **out_error_message);
    esp_err_t (*infer_media)(void *backend_ctx,
                             const claw_llm_model_profile_t *profile,
                             const claw_llm_media_request_t *request,
                             char **out_text,
                             char **out_error_message);
    void (*deinit)(void *backend_ctx);
} claw_llm_backend_vtable_t;

typedef struct {
    const char *auth_type;
    const char *chat_path;
    const char *max_tokens_field;
} claw_llm_backend_defaults_t;

typedef struct {
    const char *id;
    const claw_llm_backend_vtable_t *vtable;
    claw_llm_backend_defaults_t defaults;
} claw_llm_backend_registration_t;

typedef claw_llm_backend_registration_t claw_llm_custom_backend_registration_t;

esp_err_t claw_llm_runtime_init(claw_llm_runtime_t **out_runtime,
                                const claw_llm_runtime_config_t *config,
                                char **out_error_message);
esp_err_t claw_llm_runtime_chat(claw_llm_runtime_t *runtime,
                                const claw_llm_chat_request_t *request,
                                claw_llm_response_t *out_response,
                                char **out_error_message);
esp_err_t claw_llm_runtime_infer_media(claw_llm_runtime_t *runtime,
                                       const claw_llm_media_request_t *request,
                                       char **out_text,
                                       char **out_error_message);
void claw_llm_runtime_deinit(claw_llm_runtime_t *runtime);

esp_err_t claw_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration);
const claw_llm_backend_registration_t *claw_llm_find_backend_registration(const char *id);
const claw_llm_backend_vtable_t *claw_llm_find_custom_backend(const char *id);
void claw_llm_response_free(claw_llm_response_t *response);
