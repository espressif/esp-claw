/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/claw_llm_runtime.h"

#include <stdlib.h>
#include <string.h>

#include "llm/backends/claw_llm_backend_custom.h"
#include "llm/backends/claw_llm_backend_anthropic.h"
#include "llm/backends/claw_llm_backend_openai_compatible.h"

#define CLAW_LLM_DEFAULT_TIMEOUT_MS (120 * 1000)
#define CLAW_LLM_DEFAULT_MAX_TOKENS 8192
#define CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES (512 * 1024)

struct claw_llm_runtime {
    claw_llm_runtime_config_t config;
    const claw_llm_model_profile_t *profile;
    const claw_llm_backend_vtable_t *backend;
    void *backend_ctx;
};

static const claw_llm_model_profile_t s_profiles[] = {
    {
        .id = "openai",
        .default_backend_type = "openai_compatible",
        .default_base_url = "https://api.openai.com/v1",
        .chat_path = "/chat/completions",
        .max_tokens_field = "max_completion_tokens",
        .default_timeout_ms = CLAW_LLM_DEFAULT_TIMEOUT_MS,
        .default_image_max_bytes = CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES,
        .supports_tools = true,
        .supports_vision = true,
        .image_remote_url_only = false,
    },
    {
        .id = "qwen_compatible",
        .default_backend_type = "openai_compatible",
        .default_base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .chat_path = "/chat/completions",
        .max_tokens_field = "max_tokens",
        .default_timeout_ms = CLAW_LLM_DEFAULT_TIMEOUT_MS,
        .default_image_max_bytes = CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES,
        .supports_tools = true,
        .supports_vision = true,
        .image_remote_url_only = false,
    },
    {
        .id = "custom_openai_compatible",
        .default_backend_type = "openai_compatible",
        .default_base_url = "https://api.openai.com/v1",
        .chat_path = "/chat/completions",
        .max_tokens_field = "max_completion_tokens",
        .default_timeout_ms = CLAW_LLM_DEFAULT_TIMEOUT_MS,
        .default_image_max_bytes = CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES,
        .supports_tools = true,
        .supports_vision = true,
        .image_remote_url_only = false,
    },
    {
        .id = "anthropic",
        .default_backend_type = "anthropic",
        .default_base_url = "https://api.anthropic.com/v1",
        .chat_path = "/messages",
        .max_tokens_field = "max_tokens",
        .default_timeout_ms = CLAW_LLM_DEFAULT_TIMEOUT_MS,
        .default_image_max_bytes = CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES,
        .supports_tools = true,
        .supports_vision = true,
        .image_remote_url_only = false,
    },
    {
        .id = "custom_backend",
        .default_backend_type = "custom",
        .default_base_url = "",
        .chat_path = "",
        .max_tokens_field = "max_completion_tokens",
        .default_timeout_ms = CLAW_LLM_DEFAULT_TIMEOUT_MS,
        .default_image_max_bytes = CLAW_LLM_DEFAULT_IMAGE_MAX_BYTES,
        .supports_tools = true,
        .supports_vision = true,
        .image_remote_url_only = false,
    },
};

static const claw_llm_backend_vtable_t *find_backend(const char *id)
{
    if (!id || strcmp(id, "openai_compatible") == 0) {
        return claw_llm_backend_openai_compatible_vtable();
    }
    if (strcmp(id, "custom") == 0) {
        return claw_llm_backend_custom_vtable();
    }
    if (strcmp(id, "anthropic") == 0) {
        return claw_llm_backend_anthropic_vtable();
    }
    return NULL;
}

static char *dup_or_null(const char *value)
{
    return value ? strdup(value) : NULL;
}

static const char *normalize_profile_id(const char *profile_id)
{
    if (!profile_id || !profile_id[0]) {
        return claw_llm_profile_default()->id;
    }
    if (strcmp(profile_id, "qwen") == 0) {
        return "qwen_compatible";
    }
    if (strcmp(profile_id, "openai") == 0) {
        return "openai";
    }
    if (strcmp(profile_id, "claude") == 0) {
        return "anthropic";
    }
    return profile_id;
}

const claw_llm_model_profile_t *claw_llm_profile_find(const char *profile_id)
{
    size_t i;
    const char *needle = normalize_profile_id(profile_id);

    for (i = 0; i < sizeof(s_profiles) / sizeof(s_profiles[0]); i++) {
        if (strcmp(s_profiles[i].id, needle) == 0) {
            return &s_profiles[i];
        }
    }

    return NULL;
}

const claw_llm_model_profile_t *claw_llm_profile_default(void)
{
    return &s_profiles[0];
}

static void runtime_config_free(claw_llm_runtime_config_t *config)
{
    if (!config) {
        return;
    }

    free((char *)config->api_key);
    free((char *)config->backend_type);
    free((char *)config->profile);
    free((char *)config->model);
    free((char *)config->base_url);
    free((char *)config->auth_type);
    memset(config, 0, sizeof(*config));
}

static esp_err_t runtime_config_copy(claw_llm_runtime_config_t *dst,
                                     const claw_llm_runtime_config_t *src)
{
    dst->api_key = dup_or_null(src->api_key);
    dst->backend_type = dup_or_null(src->backend_type);
    dst->profile = dup_or_null(src->profile);
    dst->model = dup_or_null(src->model);
    dst->base_url = dup_or_null(src->base_url);
    dst->auth_type = dup_or_null(src->auth_type);
    dst->timeout_ms = src->timeout_ms;
    dst->max_tokens = src->max_tokens;
    dst->image_max_bytes = src->image_max_bytes;

    if ((src->api_key && !dst->api_key) ||
            (src->backend_type && !dst->backend_type) ||
            (src->profile && !dst->profile) ||
            (src->model && !dst->model) ||
            (src->base_url && !dst->base_url) ||
            (src->auth_type && !dst->auth_type)) {
        runtime_config_free(dst);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t claw_llm_runtime_init(claw_llm_runtime_t **out_runtime,
                                const claw_llm_runtime_config_t *config,
                                char **out_error_message)
{
    claw_llm_runtime_t *runtime;
    const claw_llm_model_profile_t *profile;
    const claw_llm_backend_vtable_t *backend;
    const char *backend_type;
    esp_err_t err;

    if (out_runtime) {
        *out_runtime = NULL;
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!out_runtime || !config || !config->api_key || !config->model || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    profile = claw_llm_profile_find(config->profile);
    if (!profile) {
        *out_error_message = strdup("Unknown LLM profile");
        return ESP_ERR_NOT_SUPPORTED;
    }

    backend_type = config->backend_type && config->backend_type[0] ?
                   config->backend_type : profile->default_backend_type;
    backend = find_backend(backend_type);
    if (!backend) {
        *out_error_message = strdup("Unknown LLM backend type");
        return ESP_ERR_NOT_SUPPORTED;
    }

    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        *out_error_message = strdup("Out of memory allocating runtime");
        return ESP_ERR_NO_MEM;
    }

    err = runtime_config_copy(&runtime->config, config);
    if (err != ESP_OK) {
        free(runtime);
        *out_error_message = strdup("Out of memory copying runtime config");
        return err;
    }
    if (!runtime->config.profile) {
        runtime->config.profile = strdup(profile->id);
    }
    if (!runtime->config.backend_type) {
        runtime->config.backend_type = strdup(backend_type);
    }
    if (!runtime->config.base_url && profile->default_base_url[0]) {
        runtime->config.base_url = strdup(profile->default_base_url);
    }
    if (!runtime->config.auth_type) {
        runtime->config.auth_type = strdup("bearer");
    }
    if (!runtime->config.timeout_ms) {
        runtime->config.timeout_ms = profile->default_timeout_ms;
    }
    if (!runtime->config.max_tokens) {
        runtime->config.max_tokens = CLAW_LLM_DEFAULT_MAX_TOKENS;
    }
    if (!runtime->config.image_max_bytes) {
        runtime->config.image_max_bytes = profile->default_image_max_bytes;
    }
    if (!runtime->config.profile || !runtime->config.backend_type || !runtime->config.auth_type ||
            (profile->default_base_url[0] && !runtime->config.base_url)) {
        runtime_config_free(&runtime->config);
        free(runtime);
        *out_error_message = strdup("Out of memory finalizing runtime config");
        return ESP_ERR_NO_MEM;
    }

    runtime->profile = profile;
    runtime->backend = backend;

    err = backend->init(&runtime->config, profile, &runtime->backend_ctx, out_error_message);
    if (err != ESP_OK) {
        runtime_config_free(&runtime->config);
        free(runtime);
        return err;
    }

    *out_runtime = runtime;
    return ESP_OK;
}

esp_err_t claw_llm_runtime_chat(claw_llm_runtime_t *runtime,
                                const claw_llm_chat_request_t *request,
                                claw_llm_response_t *out_response,
                                char **out_error_message)
{
    if (!runtime || !runtime->backend || !runtime->backend->chat) {
        return ESP_ERR_INVALID_STATE;
    }

    return runtime->backend->chat(runtime->backend_ctx,
                                  runtime->profile,
                                  request,
                                  out_response,
                                  out_error_message);
}

esp_err_t claw_llm_runtime_infer_media(claw_llm_runtime_t *runtime,
                                       const claw_llm_media_request_t *request,
                                       char **out_text,
                                       char **out_error_message)
{
    if (!runtime || !runtime->backend || !runtime->backend->infer_media) {
        return ESP_ERR_INVALID_STATE;
    }

    return runtime->backend->infer_media(runtime->backend_ctx,
                                         runtime->profile,
                                         request,
                                         out_text,
                                         out_error_message);
}

void claw_llm_runtime_deinit(claw_llm_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }

    if (runtime->backend && runtime->backend->deinit) {
        runtime->backend->deinit(runtime->backend_ctx);
    }
    runtime_config_free(&runtime->config);
    free(runtime);
}

void claw_llm_response_free(claw_llm_response_t *response)
{
    size_t i;

    if (!response) {
        return;
    }

    free(response->text);
    free(response->reasoning_content);
    for (i = 0; i < response->tool_call_count; i++) {
        free(response->tool_calls[i].id);
        free(response->tool_calls[i].name);
        free(response->tool_calls[i].arguments_json);
    }
    free(response->tool_calls);
    memset(response, 0, sizeof(*response));
}
