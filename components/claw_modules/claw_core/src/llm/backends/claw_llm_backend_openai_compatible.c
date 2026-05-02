/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/backends/claw_llm_backend_openai_compatible.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "llm/claw_llm_http_transport.h"
#include "llm/media/claw_media_pipeline.h"

typedef struct {
    char *api_key;
    char *model;
    char *base_url;
    char *auth_type;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
} openai_compatible_backend_ctx_t;

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

static esp_err_t dup_tool_call_string(cJSON *json, char **out_value)
{
    if (!out_value || !json || !cJSON_IsString(json) || !json->valuestring) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = strdup(json->valuestring);
    return *out_value ? ESP_OK : ESP_ERR_NO_MEM;
}

static char *join_url(const char *base_url, const char *path)
{
    bool base_has_slash;
    bool path_has_slash;

    if (!base_url || !path) {
        return NULL;
    }

    base_has_slash = base_url[0] && base_url[strlen(base_url) - 1] == '/';
    path_has_slash = path[0] == '/';
    if (base_has_slash && path_has_slash) {
        return dup_printf("%s%s", base_url, path + 1);
    }
    if (!base_has_slash && !path_has_slash) {
        return dup_printf("%s/%s", base_url, path);
    }
    return dup_printf("%s%s", base_url, path);
}

static esp_err_t parse_chat_response(const char *body,
                                     claw_llm_response_t *out_response,
                                     char **out_error_message)
{
    cJSON *root = NULL;
    cJSON *choices;
    cJSON *choice0;
    cJSON *message;
    cJSON *content;
    cJSON *reasoning_content;
    cJSON *tool_calls;
    cJSON *tool_call;
    size_t tool_count = 0;
    size_t tool_index = 0;

    if (!body || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_response, 0, sizeof(*out_response));
    root = cJSON_Parse(body);
    if (!root) {
        *out_error_message = dup_printf("Failed to parse LLM JSON response");
        return ESP_FAIL;
    }

    choices = cJSON_GetObjectItem(root, "choices");
    choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    if (!message || !cJSON_IsObject(message)) {
        cJSON_Delete(root);
        *out_error_message = dup_printf("LLM response missing message");
        return ESP_FAIL;
    }

    content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content) && content->valuestring[0]) {
        out_response->text = strdup(content->valuestring);
        if (!out_response->text) {
            cJSON_Delete(root);
            *out_error_message = dup_printf("Out of memory copying LLM response");
            return ESP_ERR_NO_MEM;
        }
    }

    reasoning_content = cJSON_GetObjectItem(message, "reasoning_content");
    if (reasoning_content && cJSON_IsString(reasoning_content)) {
        out_response->reasoning_content = strdup(reasoning_content->valuestring ?
                                                 reasoning_content->valuestring : "");
        if (!out_response->reasoning_content) {
            cJSON_Delete(root);
            *out_error_message = dup_printf("Out of memory copying LLM reasoning");
            return ESP_ERR_NO_MEM;
        }
    }

    tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        tool_count = (size_t)cJSON_GetArraySize(tool_calls);
        if (tool_count > 0) {
            out_response->tool_calls = calloc(tool_count, sizeof(claw_llm_tool_call_t));
            if (!out_response->tool_calls) {
                cJSON_Delete(root);
                *out_error_message = dup_printf("Out of memory copying tool calls");
                return ESP_ERR_NO_MEM;
            }
            out_response->tool_call_count = tool_count;
        }

        cJSON_ArrayForEach(tool_call, tool_calls) {
            claw_llm_tool_call_t *dst = &out_response->tool_calls[tool_index];
            cJSON *id_json = cJSON_GetObjectItem(tool_call, "id");
            cJSON *function_json = cJSON_GetObjectItem(tool_call, "function");
            cJSON *name_json = function_json ? cJSON_GetObjectItem(function_json, "name") : NULL;
            cJSON *args_json = function_json ? cJSON_GetObjectItem(function_json, "arguments") : NULL;
            esp_err_t err;

            if (!id_json || !function_json || !name_json || !args_json) {
                cJSON_Delete(root);
                *out_error_message = dup_printf("Malformed tool call in LLM response");
                return ESP_FAIL;
            }

            err = dup_tool_call_string(id_json, &dst->id);
            if (err == ESP_OK) {
                err = dup_tool_call_string(name_json, &dst->name);
            }
            if (err == ESP_OK) {
                err = dup_tool_call_string(args_json, &dst->arguments_json);
            }
            if (err != ESP_OK) {
                cJSON_Delete(root);
                *out_error_message = dup_printf("Out of memory copying tool call");
                return err;
            }
            tool_index++;
        }
    }

    if (!out_response->text && out_response->tool_call_count == 0) {
        cJSON_Delete(root);
        *out_error_message = dup_printf("LLM returned empty text response");
        return ESP_FAIL;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t build_chat_body(const openai_compatible_backend_ctx_t *ctx,
                                 const claw_llm_model_profile_t *profile,
                                 const claw_llm_chat_request_t *request,
                                 char **out_post_data,
                                 char **out_error_message)
{
    cJSON *body = NULL;
    cJSON *messages = NULL;
    cJSON *system_msg = NULL;
    cJSON *item;
    char *post_data = NULL;

    body = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    system_msg = cJSON_CreateObject();
    if (!body || !messages || !system_msg) {
        *out_error_message = dup_printf("Out of memory building request");
        cJSON_Delete(body);
        cJSON_Delete(messages);
        cJSON_Delete(system_msg);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "model", ctx->model);
    cJSON_AddNumberToObject(body, profile->max_tokens_field, ctx->max_tokens);

    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", request->system_prompt);
    cJSON_AddItemToArray(messages, system_msg);
    system_msg = NULL;

    cJSON_ArrayForEach(item, request->messages) {
        cJSON *dup = cJSON_Duplicate(item, true);
        if (!dup) {
            *out_error_message = dup_printf("Out of memory copying messages");
            cJSON_Delete(body);
            cJSON_Delete(messages);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    cJSON_AddItemToObject(body, "messages", messages);
    messages = NULL;

    if (request->tools_json && request->tools_json[0]) {
        cJSON *tools = cJSON_Parse(request->tools_json);

        if (!tools || !cJSON_IsArray(tools)) {
            cJSON_Delete(tools);
            cJSON_Delete(body);
            *out_error_message = dup_printf("Invalid tools JSON");
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(body, "tools", tools);
    }

    post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        *out_error_message = dup_printf("Out of memory serializing request");
        return ESP_ERR_NO_MEM;
    }

    *out_post_data = post_data;
    return ESP_OK;
}

static esp_err_t openai_compatible_init(const claw_llm_runtime_config_t *config,
                                        const claw_llm_model_profile_t *profile,
                                        void **out_backend_ctx,
                                        char **out_error_message)
{
    openai_compatible_backend_ctx_t *ctx;
    const char *base_url;
    const char *auth_type;

    if (!config || !profile || !out_backend_ctx || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->api_key || !config->api_key[0]) {
        *out_error_message = dup_printf("LLM API key is empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->model || !config->model[0]) {
        *out_error_message = dup_printf("LLM model is empty");
        return ESP_ERR_INVALID_ARG;
    }

    base_url = (config->base_url && config->base_url[0]) ? config->base_url : profile->default_base_url;
    auth_type = (config->auth_type && config->auth_type[0]) ? config->auth_type : "bearer";
    if (!base_url || !base_url[0]) {
        *out_error_message = dup_printf("LLM base_url is empty");
        return ESP_ERR_INVALID_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        *out_error_message = dup_printf("Out of memory allocating backend context");
        return ESP_ERR_NO_MEM;
    }

    ctx->api_key = strdup(config->api_key);
    ctx->model = strdup(config->model);
    ctx->base_url = strdup(base_url);
    ctx->auth_type = strdup(auth_type);
    ctx->timeout_ms = config->timeout_ms ? config->timeout_ms : profile->default_timeout_ms;
    ctx->max_tokens = config->max_tokens;
    ctx->image_max_bytes = config->image_max_bytes ? config->image_max_bytes : profile->default_image_max_bytes;
    if (!ctx->api_key || !ctx->model || !ctx->base_url || !ctx->auth_type) {
        *out_error_message = dup_printf("Out of memory copying backend config");
        free(ctx->api_key);
        free(ctx->model);
        free(ctx->base_url);
        free(ctx->auth_type);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    *out_backend_ctx = ctx;
    return ESP_OK;
}

static esp_err_t openai_compatible_chat(void *backend_ctx,
                                        const claw_llm_model_profile_t *profile,
                                        const claw_llm_chat_request_t *request,
                                        claw_llm_response_t *out_response,
                                        char **out_error_message)
{
    openai_compatible_backend_ctx_t *ctx = (openai_compatible_backend_ctx_t *)backend_ctx;
    claw_llm_http_json_request_t http_request = {0};
    claw_llm_http_response_t http_response = {0};
    char *url = NULL;
    char *post_data = NULL;
    esp_err_t err;

    if (!ctx || !profile || !request || !request->system_prompt || !request->messages ||
            !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_chat_body(ctx, profile, request, &post_data, out_error_message);
    if (err != ESP_OK) {
        return err;
    }

    url = join_url(ctx->base_url, profile->chat_path);
    if (!url) {
        free(post_data);
        *out_error_message = dup_printf("Out of memory building API URL");
        return ESP_ERR_NO_MEM;
    }

    http_request.url = url;
    http_request.body = post_data;
    http_request.api_key = ctx->api_key;
    http_request.auth_type = ctx->auth_type;
    http_request.timeout_ms = ctx->timeout_ms;

    err = claw_llm_http_post_json(&http_request, &http_response, out_error_message);
    free(url);
    free(post_data);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_chat_response(http_response.body, out_response, out_error_message);
    claw_llm_http_response_free(&http_response);
    return err;
}

static esp_err_t openai_compatible_infer_media(void *backend_ctx,
                                               const claw_llm_model_profile_t *profile,
                                               const claw_llm_media_request_t *request,
                                               char **out_text,
                                               char **out_error_message)
{
    openai_compatible_backend_ctx_t *ctx = (openai_compatible_backend_ctx_t *)backend_ctx;
    claw_media_prepared_t prepared = {0};
    claw_llm_response_t response = {0};
    cJSON *messages = NULL;
    cJSON *system_msg = NULL;
    cJSON *user_msg = NULL;
    cJSON *content = NULL;
    cJSON *text_block = NULL;
    cJSON *image_block = NULL;
    cJSON *image_value = NULL;
    char *url = NULL;
    char *post_data = NULL;
    claw_llm_http_json_request_t http_request = {0};
    claw_llm_http_response_t http_response = {0};
    cJSON *body = NULL;
    esp_err_t err;

    if (out_text) {
        *out_text = NULL;
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!ctx || !profile || !request || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!profile->supports_vision) {
        *out_error_message = dup_printf("Selected profile does not support media inference");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!request->user_prompt || !request->user_prompt[0] || !request->media || request->media_count == 0) {
        *out_error_message = dup_printf("media request is incomplete");
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_media_prepare_asset(&request->media[0],
                                   profile,
                                   ctx->image_max_bytes,
                                   &prepared,
                                   out_error_message);
    if (err != ESP_OK) {
        return err;
    }

    body = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    system_msg = cJSON_CreateObject();
    user_msg = cJSON_CreateObject();
    content = cJSON_CreateArray();
    text_block = cJSON_CreateObject();
    image_block = cJSON_CreateObject();
    image_value = cJSON_CreateObject();
    if (!body || !messages || !system_msg || !user_msg || !content ||
            !text_block || !image_block || !image_value) {
        err = ESP_ERR_NO_MEM;
        *out_error_message = dup_printf("Out of memory building media request");
        goto cleanup;
    }

    cJSON_AddStringToObject(body, "model", ctx->model);
    cJSON_AddNumberToObject(body, profile->max_tokens_field, ctx->max_tokens);

    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", request->system_prompt ? request->system_prompt : "");
    cJSON_AddItemToArray(messages, system_msg);
    system_msg = NULL;

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", request->user_prompt);
    cJSON_AddItemToArray(content, text_block);
    text_block = NULL;

    cJSON_AddStringToObject(image_block, "type", "image_url");
    cJSON_AddStringToObject(image_value, "url", prepared.payload);
    cJSON_AddItemToObject(image_block, "image_url", image_value);
    image_value = NULL;
    cJSON_AddItemToArray(content, image_block);
    image_block = NULL;

    cJSON_AddItemToObject(user_msg, "content", content);
    content = NULL;
    cJSON_AddItemToArray(messages, user_msg);
    user_msg = NULL;
    cJSON_AddItemToObject(body, "messages", messages);
    messages = NULL;

    post_data = cJSON_PrintUnformatted(body);
    if (!post_data) {
        err = ESP_ERR_NO_MEM;
        *out_error_message = dup_printf("Out of memory serializing media request");
        goto cleanup;
    }

    url = join_url(ctx->base_url, profile->chat_path);
    if (!url) {
        err = ESP_ERR_NO_MEM;
        *out_error_message = dup_printf("Out of memory building API URL");
        goto cleanup;
    }

    http_request.url = url;
    http_request.body = post_data;
    http_request.api_key = ctx->api_key;
    http_request.auth_type = ctx->auth_type;
    http_request.timeout_ms = ctx->timeout_ms;

    err = claw_llm_http_post_json(&http_request, &http_response, out_error_message);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = parse_chat_response(http_response.body, &response, out_error_message);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!response.text || !response.text[0]) {
        err = ESP_FAIL;
        *out_error_message = dup_printf("LLM returned empty media response");
        goto cleanup;
    }

    *out_text = response.text;
    response.text = NULL;
    err = ESP_OK;

cleanup:
    free(url);
    free(post_data);
    claw_llm_http_response_free(&http_response);
    claw_llm_response_free(&response);
    claw_media_prepared_free(&prepared);
    cJSON_Delete(body);
    cJSON_Delete(messages);
    cJSON_Delete(system_msg);
    cJSON_Delete(user_msg);
    cJSON_Delete(content);
    cJSON_Delete(text_block);
    cJSON_Delete(image_block);
    cJSON_Delete(image_value);
    return err;
}

static void openai_compatible_deinit(void *backend_ctx)
{
    openai_compatible_backend_ctx_t *ctx = (openai_compatible_backend_ctx_t *)backend_ctx;

    if (!ctx) {
        return;
    }

    free(ctx->api_key);
    free(ctx->model);
    free(ctx->base_url);
    free(ctx->auth_type);
    free(ctx);
}

const claw_llm_backend_vtable_t *claw_llm_backend_openai_compatible_vtable(void)
{
    static const claw_llm_backend_vtable_t vtable = {
        .id = "openai_compatible",
        .init = openai_compatible_init,
        .chat = openai_compatible_chat,
        .infer_media = openai_compatible_infer_media,
        .deinit = openai_compatible_deinit,
    };

    return &vtable;
}
