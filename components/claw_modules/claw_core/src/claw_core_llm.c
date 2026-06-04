/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_core_llm";

#ifndef CLAW_CORE_LOG_FULL_LLM_REQUEST
#define CLAW_CORE_LOG_FULL_LLM_REQUEST 0
#endif

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

esp_err_t claw_core_llm_init(const claw_core_llm_config_t *config,
                             claw_llm_runtime_t **out_runtime,
                             char **out_error_message)
{
    claw_llm_runtime_config_t runtime_config = {0};
    esp_err_t err;

    if (out_runtime) {
        *out_runtime = NULL;
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!config || !config->api_key || !config->model || !out_runtime || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    runtime_config.api_key = config->api_key;
    runtime_config.backend_type = config->backend_type;
    runtime_config.model = config->model;
    runtime_config.base_url = config->base_url;
    runtime_config.auth_type = config->auth_type;
    runtime_config.max_tokens_field = config->max_tokens_field;
    runtime_config.timeout_ms = config->timeout_ms;
    runtime_config.max_tokens = config->max_tokens;
    runtime_config.image_max_bytes = config->image_max_bytes;
    runtime_config.supports_tools = config->supports_tools;
    runtime_config.supports_vision = config->supports_vision;
    runtime_config.image_remote_url_only = config->image_remote_url_only;
    err = claw_llm_runtime_init(out_runtime, &runtime_config, out_error_message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init: runtime init failed err=0x%x", err);
    }
    return err;
}

esp_err_t claw_core_llm_chat_messages(claw_core_handle_t core,
                                      const char *system_prompt,
                                      cJSON *messages,
                                      const char *tools_json,
                                      claw_core_llm_response_t *out_response,
                                      char **out_error_message)
{
    claw_llm_chat_request_t request = {0};
#if CLAW_CORE_LOG_FULL_LLM_REQUEST
    char *messages_json = NULL;
#endif

    if (!core || !core->llm_runtime) {
        if (out_error_message) {
            *out_error_message = dup_printf("LLM runtime is not initialized");
        }
        ESP_LOGE(TAG, "chat_messages: runtime is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!system_prompt || !messages || !out_response || !out_error_message || !cJSON_IsArray(messages)) {
        return ESP_ERR_INVALID_ARG;
    }

    request.system_prompt = system_prompt;
    request.messages = messages;
    request.tools_json = tools_json;
    request.abort_flag = &core->inflight_abort;

#if CLAW_CORE_LOG_FULL_LLM_REQUEST
    messages_json = cJSON_PrintUnformatted(messages);
    if (messages_json) {
        ESP_LOGI(TAG, "llm_request system_prompt=%s", system_prompt);
        ESP_LOGI(TAG, "llm_request messages=%s", messages_json);
        ESP_LOGI(TAG, "llm_request tools=%s", tools_json ? tools_json : "[]");
        free(messages_json);
    } else {
        ESP_LOGE(TAG, "failed to render full LLM request messages");
    }
#endif

    esp_err_t err = claw_llm_runtime_chat(core->llm_runtime, &request, out_response, out_error_message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chat_messages: runtime chat failed err=0x%x", err);
    }
    return err;
}

esp_err_t claw_core_llm_infer_media(claw_core_handle_t core,
                                    const claw_llm_media_request_t *request,
                                    char **out_text,
                                    char **out_error_message)
{
    esp_err_t err;

    if (!core || !core->llm_runtime) {
        if (out_error_message) {
            *out_error_message = dup_printf("LLM runtime is not initialized");
        }
        ESP_LOGE(TAG, "infer_media: runtime is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!request || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    err = claw_llm_runtime_infer_media(core->llm_runtime, request, out_text, out_error_message);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "infer_media: runtime inference failed err=0x%x media_count=%zu",
                 err, request->media_count);
    }
    return err;
}

esp_err_t claw_core_llm_register_custom_backend(const claw_llm_custom_backend_registration_t *registration)
{
    esp_err_t err = claw_llm_register_custom_backend(registration);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_custom_backend: failed err=0x%x", err);
    }
    return err;
}

void claw_core_llm_response_free(claw_core_llm_response_t *response)
{
    claw_llm_response_free(response);
}
