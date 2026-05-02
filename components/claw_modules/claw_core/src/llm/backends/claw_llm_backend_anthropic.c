/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/backends/claw_llm_backend_anthropic.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/claw_llm_http_transport.h"
#include "llm/media/claw_media_pipeline.h"

#define CLAW_LLM_ANTHROPIC_VERSION "2023-06-01"

typedef struct {
    char *api_key;
    char *model;
    char *base_url;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
} anthropic_backend_ctx_t;

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

static char *dup_n_string(const char *value, size_t len)
{
    char *copy = NULL;

    if (!value) {
        return NULL;
    }

    copy = calloc(1, len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len);
    copy[len] = '\0';
    return copy;
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

static esp_err_t dup_json_string(cJSON *json, char **out_value)
{
    if (!json || !cJSON_IsString(json) || !json->valuestring || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = strdup(json->valuestring);
    return *out_value ? ESP_OK : ESP_ERR_NO_MEM;
}

static cJSON *anthropic_make_text_block(const char *text)
{
    cJSON *block = NULL;

    if (!text || !text[0]) {
        return NULL;
    }

    block = cJSON_CreateObject();
    if (!block) {
        return NULL;
    }
    cJSON_AddStringToObject(block, "type", "text");
    cJSON_AddStringToObject(block, "text", text);
    return block;
}

static cJSON *anthropic_make_tool_use_block(cJSON *tool_call)
{
    cJSON *block = NULL;
    cJSON *id_json = NULL;
    cJSON *function_json = NULL;
    cJSON *name_json = NULL;
    cJSON *args_json = NULL;
    cJSON *input_json = NULL;

    if (!tool_call || !cJSON_IsObject(tool_call)) {
        return NULL;
    }

    id_json = cJSON_GetObjectItem(tool_call, "id");
    function_json = cJSON_GetObjectItem(tool_call, "function");
    name_json = cJSON_IsObject(function_json) ? cJSON_GetObjectItem(function_json, "name") : NULL;
    args_json = cJSON_IsObject(function_json) ? cJSON_GetObjectItem(function_json, "arguments") : NULL;
    if (!cJSON_IsString(id_json) || !cJSON_IsString(name_json)) {
        return NULL;
    }

    block = cJSON_CreateObject();
    if (!block) {
        return NULL;
    }
    cJSON_AddStringToObject(block, "type", "tool_use");
    cJSON_AddStringToObject(block, "id", id_json->valuestring);
    cJSON_AddStringToObject(block, "name", name_json->valuestring);

    if (cJSON_IsString(args_json) && args_json->valuestring && args_json->valuestring[0]) {
        input_json = cJSON_Parse(args_json->valuestring);
    }
    if (!input_json) {
        input_json = cJSON_CreateObject();
    }
    if (!input_json) {
        cJSON_Delete(block);
        return NULL;
    }
    cJSON_AddItemToObject(block, "input", input_json);
    return block;
}

static cJSON *anthropic_make_tool_result_message(const char *tool_call_id, const char *content)
{
    cJSON *message = NULL;
    cJSON *blocks = NULL;
    cJSON *block = NULL;

    if (!tool_call_id || !tool_call_id[0]) {
        return NULL;
    }

    message = cJSON_CreateObject();
    blocks = cJSON_CreateArray();
    block = cJSON_CreateObject();
    if (!message || !blocks || !block) {
        cJSON_Delete(message);
        cJSON_Delete(blocks);
        cJSON_Delete(block);
        return NULL;
    }

    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(block, "type", "tool_result");
    cJSON_AddStringToObject(block, "tool_use_id", tool_call_id);
    cJSON_AddStringToObject(block, "content", content ? content : "");
    cJSON_AddBoolToObject(block, "is_error", false);
    cJSON_AddItemToArray(blocks, block);
    cJSON_AddItemToObject(message, "content", blocks);
    return message;
}

static cJSON *anthropic_duplicate_supported_block(cJSON *block)
{
    cJSON *type_json = NULL;

    if (!block || !cJSON_IsObject(block)) {
        return NULL;
    }

    type_json = cJSON_GetObjectItem(block, "type");
    if (!cJSON_IsString(type_json) || !type_json->valuestring) {
        return NULL;
    }
    if (strcmp(type_json->valuestring, "text") == 0 ||
            strcmp(type_json->valuestring, "tool_use") == 0 ||
            strcmp(type_json->valuestring, "tool_result") == 0) {
        return cJSON_Duplicate(block, true);
    }
    return NULL;
}

static cJSON *convert_messages_to_anthropic(cJSON *messages)
{
    cJSON *out = NULL;
    cJSON *msg = NULL;

    out = cJSON_CreateArray();
    if (!out) {
        return NULL;
    }
    if (!messages || !cJSON_IsArray(messages)) {
        return out;
    }

    cJSON_ArrayForEach(msg, messages) {
        cJSON *role_json = cJSON_GetObjectItem(msg, "role");
        cJSON *content_json = cJSON_GetObjectItem(msg, "content");
        const char *role = cJSON_IsString(role_json) ? role_json->valuestring : NULL;
        cJSON *out_message = NULL;
        cJSON *blocks = NULL;
        cJSON *tool_calls = NULL;
        cJSON *tool_call = NULL;

        if (!role || !role[0]) {
            continue;
        }

        if (strcmp(role, "tool") == 0) {
            cJSON *tool_call_id = cJSON_GetObjectItem(msg, "tool_call_id");
            cJSON *tool_message = anthropic_make_tool_result_message(
                                      cJSON_IsString(tool_call_id) ? tool_call_id->valuestring : NULL,
                                      cJSON_IsString(content_json) ? content_json->valuestring : "");
            if (!tool_message) {
                cJSON_Delete(out);
                return NULL;
            }
            cJSON_AddItemToArray(out, tool_message);
            continue;
        }

        if (strcmp(role, "assistant") != 0 && strcmp(role, "user") != 0) {
            continue;
        }

        out_message = cJSON_CreateObject();
        blocks = cJSON_CreateArray();
        if (!out_message || !blocks) {
            cJSON_Delete(out_message);
            cJSON_Delete(blocks);
            cJSON_Delete(out);
            return NULL;
        }

        cJSON_AddStringToObject(out_message, "role", role);

        if (cJSON_IsString(content_json) && content_json->valuestring && content_json->valuestring[0]) {
            cJSON *text_block = anthropic_make_text_block(content_json->valuestring);

            if (!text_block) {
                cJSON_Delete(out_message);
                cJSON_Delete(blocks);
                cJSON_Delete(out);
                return NULL;
            }
            cJSON_AddItemToArray(blocks, text_block);
        } else if (cJSON_IsArray(content_json)) {
            cJSON *block = NULL;

            cJSON_ArrayForEach(block, content_json) {
                cJSON *dup = anthropic_duplicate_supported_block(block);

                if (dup) {
                    cJSON_AddItemToArray(blocks, dup);
                }
            }
        }

        if (strcmp(role, "assistant") == 0) {
            tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
            if (cJSON_IsArray(tool_calls)) {
                cJSON_ArrayForEach(tool_call, tool_calls) {
                    cJSON *tool_block = anthropic_make_tool_use_block(tool_call);

                    if (!tool_block) {
                        cJSON_Delete(out_message);
                        cJSON_Delete(blocks);
                        cJSON_Delete(out);
                        return NULL;
                    }
                    cJSON_AddItemToArray(blocks, tool_block);
                }
            }
        }

        if (cJSON_GetArraySize(blocks) == 0) {
            cJSON_Delete(out_message);
            cJSON_Delete(blocks);
            continue;
        }

        cJSON_AddItemToObject(out_message, "content", blocks);
        cJSON_AddItemToArray(out, out_message);
    }

    return out;
}

static cJSON *convert_tools_to_anthropic(const char *tools_json)
{
    cJSON *parsed = NULL;
    cJSON *out = NULL;
    cJSON *item = NULL;

    if (!tools_json || !tools_json[0]) {
        return NULL;
    }

    parsed = cJSON_Parse(tools_json);
    if (!parsed || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return NULL;
    }

    out = cJSON_CreateArray();
    if (!out) {
        cJSON_Delete(parsed);
        return NULL;
    }

    cJSON_ArrayForEach(item, parsed) {
        cJSON *tool = NULL;
        cJSON *function_json = NULL;
        cJSON *name_json = NULL;
        cJSON *desc_json = NULL;
        cJSON *schema_json = NULL;

        if (cJSON_IsObject(item)) {
            cJSON *type_json = cJSON_GetObjectItem(item, "type");

            if (cJSON_IsString(type_json) && type_json->valuestring &&
                    strcmp(type_json->valuestring, "function") == 0) {
                function_json = cJSON_GetObjectItem(item, "function");
                name_json = cJSON_IsObject(function_json) ? cJSON_GetObjectItem(function_json, "name") : NULL;
                desc_json = cJSON_IsObject(function_json) ? cJSON_GetObjectItem(function_json, "description") : NULL;
                schema_json = cJSON_IsObject(function_json) ? cJSON_GetObjectItem(function_json, "parameters") : NULL;
            } else {
                name_json = cJSON_GetObjectItem(item, "name");
                desc_json = cJSON_GetObjectItem(item, "description");
                schema_json = cJSON_GetObjectItem(item, "input_schema");
            }
        }

        if (!cJSON_IsString(name_json) || !name_json->valuestring || !name_json->valuestring[0]) {
            continue;
        }

        tool = cJSON_CreateObject();
        if (!tool) {
            cJSON_Delete(parsed);
            cJSON_Delete(out);
            return NULL;
        }

        cJSON_AddStringToObject(tool, "name", name_json->valuestring);
        if (cJSON_IsString(desc_json) && desc_json->valuestring) {
            cJSON_AddStringToObject(tool, "description", desc_json->valuestring);
        }
        if (schema_json) {
            cJSON_AddItemToObject(tool, "input_schema", cJSON_Duplicate(schema_json, true));
        } else {
            cJSON_AddItemToObject(tool, "input_schema", cJSON_CreateObject());
        }
        cJSON_AddItemToArray(out, tool);
    }

    cJSON_Delete(parsed);
    return out;
}

static esp_err_t parse_data_url(const char *data_url, char **out_mime, char **out_data)
{
    const char *prefix = "data:";
    const char *mime_end = NULL;
    const char *data_start = NULL;

    if (out_mime) {
        *out_mime = NULL;
    }
    if (out_data) {
        *out_data = NULL;
    }
    if (!data_url || strncmp(data_url, prefix, strlen(prefix)) != 0 || !out_mime || !out_data) {
        return ESP_ERR_INVALID_ARG;
    }

    mime_end = strstr(data_url, ";base64,");
    if (!mime_end) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    data_start = mime_end + strlen(";base64,");
    if (!data_start[0]) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_mime = dup_n_string(data_url + strlen(prefix), (size_t)(mime_end - (data_url + strlen(prefix))));
    *out_data = strdup(data_start);
    if (!*out_mime || !*out_data) {
        free(*out_mime);
        free(*out_data);
        *out_mime = NULL;
        *out_data = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t parse_chat_response(const char *body,
                                     claw_llm_response_t *out_response,
                                     char **out_error_message)
{
    cJSON *root = NULL;
    cJSON *content = NULL;
    cJSON *block = NULL;
    size_t tool_count = 0;
    size_t tool_index = 0;
    size_t total_text_len = 0;
    char *text = NULL;

    if (!body || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_response, 0, sizeof(*out_response));
    root = cJSON_Parse(body);
    if (!root) {
        *out_error_message = dup_printf("Failed to parse LLM JSON response");
        return ESP_FAIL;
    }

    content = cJSON_GetObjectItem(root, "content");
    if (!cJSON_IsArray(content)) {
        cJSON_Delete(root);
        *out_error_message = dup_printf("LLM response missing content");
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(block, content) {
        cJSON *type_json = cJSON_GetObjectItem(block, "type");

        if (!cJSON_IsString(type_json) || !type_json->valuestring) {
            continue;
        }
        if (strcmp(type_json->valuestring, "text") == 0) {
            cJSON *text_json = cJSON_GetObjectItem(block, "text");

            if (cJSON_IsString(text_json) && text_json->valuestring) {
                total_text_len += strlen(text_json->valuestring);
            }
        } else if (strcmp(type_json->valuestring, "tool_use") == 0) {
            tool_count++;
        }
    }

    if (total_text_len > 0) {
        size_t offset = 0;

        text = calloc(1, total_text_len + 1);
        if (!text) {
            cJSON_Delete(root);
            *out_error_message = dup_printf("Out of memory copying LLM text");
            return ESP_ERR_NO_MEM;
        }

        cJSON_ArrayForEach(block, content) {
            cJSON *type_json = cJSON_GetObjectItem(block, "type");
            cJSON *text_json = NULL;
            size_t len = 0;

            if (!cJSON_IsString(type_json) || strcmp(type_json->valuestring, "text") != 0) {
                continue;
            }
            text_json = cJSON_GetObjectItem(block, "text");
            if (!cJSON_IsString(text_json) || !text_json->valuestring) {
                continue;
            }
            len = strlen(text_json->valuestring);
            memcpy(text + offset, text_json->valuestring, len);
            offset += len;
        }

        out_response->text = text;
        text = NULL;
    }

    if (tool_count > 0) {
        out_response->tool_calls = calloc(tool_count, sizeof(claw_llm_tool_call_t));
        if (!out_response->tool_calls) {
            cJSON_Delete(root);
            *out_error_message = dup_printf("Out of memory copying tool calls");
            return ESP_ERR_NO_MEM;
        }
        out_response->tool_call_count = tool_count;

        cJSON_ArrayForEach(block, content) {
            cJSON *type_json = cJSON_GetObjectItem(block, "type");
            cJSON *dst_input = NULL;
            char *input_json = NULL;
            claw_llm_tool_call_t *dst = NULL;
            esp_err_t err = ESP_OK;

            if (!cJSON_IsString(type_json) || strcmp(type_json->valuestring, "tool_use") != 0) {
                continue;
            }

            dst = &out_response->tool_calls[tool_index];
            err = dup_json_string(cJSON_GetObjectItem(block, "id"), &dst->id);
            if (err == ESP_OK) {
                err = dup_json_string(cJSON_GetObjectItem(block, "name"), &dst->name);
            }
            if (err == ESP_OK) {
                dst_input = cJSON_GetObjectItem(block, "input");
                if (dst_input) {
                    input_json = cJSON_PrintUnformatted(dst_input);
                } else {
                    input_json = strdup("{}");
                }
                if (!input_json) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    dst->arguments_json = input_json;
                    input_json = NULL;
                }
            }
            if (err != ESP_OK) {
                free(input_json);
                cJSON_Delete(root);
                *out_error_message = dup_printf("Out of memory copying tool call");
                return err;
            }
            tool_index++;
        }
    }

    cJSON_Delete(root);
    if (!out_response->text && out_response->tool_call_count == 0) {
        *out_error_message = dup_printf("LLM returned empty response");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t build_chat_body(const anthropic_backend_ctx_t *ctx,
                                 const claw_llm_chat_request_t *request,
                                 char **out_post_data,
                                 char **out_error_message)
{
    cJSON *body = NULL;
    cJSON *messages = NULL;
    cJSON *tools = NULL;
    char *post_data = NULL;

    body = cJSON_CreateObject();
    if (!body) {
        *out_error_message = dup_printf("Out of memory building request");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "model", ctx->model);
    cJSON_AddNumberToObject(body, "max_tokens", ctx->max_tokens);
    cJSON_AddStringToObject(body, "system", request->system_prompt ? request->system_prompt : "");

    messages = convert_messages_to_anthropic(request->messages);
    if (!messages) {
        cJSON_Delete(body);
        *out_error_message = dup_printf("Out of memory converting messages");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(body, "messages", messages);

    tools = convert_tools_to_anthropic(request->tools_json);
    if (request->tools_json && request->tools_json[0] && !tools) {
        cJSON_Delete(body);
        *out_error_message = dup_printf("Invalid tools JSON");
        return ESP_ERR_INVALID_ARG;
    }
    if (tools && cJSON_GetArraySize(tools) > 0) {
        cJSON *tool_choice = cJSON_CreateObject();

        if (!tool_choice) {
            cJSON_Delete(tools);
            cJSON_Delete(body);
            *out_error_message = dup_printf("Out of memory building tool choice");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(body, "tools", tools);
        cJSON_AddStringToObject(tool_choice, "type", "auto");
        cJSON_AddItemToObject(body, "tool_choice", tool_choice);
    } else {
        cJSON_Delete(tools);
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

static esp_err_t anthropic_init(const claw_llm_runtime_config_t *config,
                                const claw_llm_model_profile_t *profile,
                                void **out_backend_ctx,
                                char **out_error_message)
{
    anthropic_backend_ctx_t *ctx = NULL;
    const char *base_url = NULL;

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
    ctx->timeout_ms = config->timeout_ms ? config->timeout_ms : profile->default_timeout_ms;
    ctx->max_tokens = config->max_tokens;
    ctx->image_max_bytes = config->image_max_bytes ? config->image_max_bytes : profile->default_image_max_bytes;
    if (!ctx->api_key || !ctx->model || !ctx->base_url) {
        free(ctx->api_key);
        free(ctx->model);
        free(ctx->base_url);
        free(ctx);
        *out_error_message = dup_printf("Out of memory copying backend config");
        return ESP_ERR_NO_MEM;
    }

    *out_backend_ctx = ctx;
    return ESP_OK;
}

static esp_err_t anthropic_chat(void *backend_ctx,
                                const claw_llm_model_profile_t *profile,
                                const claw_llm_chat_request_t *request,
                                claw_llm_response_t *out_response,
                                char **out_error_message)
{
    anthropic_backend_ctx_t *ctx = (anthropic_backend_ctx_t *)backend_ctx;
    claw_llm_http_json_request_t http_request = {0};
    claw_llm_http_response_t http_response = {0};
    char *post_data = NULL;
    char *url = NULL;
    esp_err_t err;
    const claw_llm_http_header_t headers[] = {
        { .name = "x-api-key", .value = ctx ? ctx->api_key : NULL },
        { .name = "anthropic-version", .value = CLAW_LLM_ANTHROPIC_VERSION },
    };

    if (!ctx || !profile || !request || !request->messages || !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }

    err = build_chat_body(ctx, request, &post_data, out_error_message);
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
    http_request.auth_type = "none";
    http_request.timeout_ms = ctx->timeout_ms;
    http_request.headers = headers;
    http_request.header_count = sizeof(headers) / sizeof(headers[0]);

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

static esp_err_t anthropic_infer_media(void *backend_ctx,
                                       const claw_llm_model_profile_t *profile,
                                       const claw_llm_media_request_t *request,
                                       char **out_text,
                                       char **out_error_message)
{
    anthropic_backend_ctx_t *ctx = (anthropic_backend_ctx_t *)backend_ctx;
    claw_media_prepared_t prepared = {0};
    claw_llm_response_t response = {0};
    claw_llm_http_json_request_t http_request = {0};
    claw_llm_http_response_t http_response = {0};
    cJSON *body = NULL;
    cJSON *messages = NULL;
    cJSON *user_msg = NULL;
    cJSON *content = NULL;
    cJSON *text_block = NULL;
    cJSON *image_block = NULL;
    cJSON *source = NULL;
    char *mime = NULL;
    char *base64_data = NULL;
    char *post_data = NULL;
    char *url = NULL;
    esp_err_t err;
    const claw_llm_http_header_t headers[] = {
        { .name = "x-api-key", .value = ctx ? ctx->api_key : NULL },
        { .name = "anthropic-version", .value = CLAW_LLM_ANTHROPIC_VERSION },
    };

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
    if (prepared.kind != CLAW_MEDIA_PREPARED_KIND_DATA_URL) {
        err = ESP_ERR_NOT_SUPPORTED;
        *out_error_message = dup_printf("Anthropic backend requires local image data");
        goto cleanup;
    }

    err = parse_data_url(prepared.payload, &mime, &base64_data);
    if (err != ESP_OK) {
        *out_error_message = dup_printf("Failed to prepare Anthropic image payload");
        goto cleanup;
    }

    body = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    user_msg = cJSON_CreateObject();
    content = cJSON_CreateArray();
    text_block = cJSON_CreateObject();
    image_block = cJSON_CreateObject();
    source = cJSON_CreateObject();
    if (!body || !messages || !user_msg || !content || !text_block || !image_block || !source) {
        err = ESP_ERR_NO_MEM;
        *out_error_message = dup_printf("Out of memory building media request");
        goto cleanup;
    }

    cJSON_AddStringToObject(body, "model", ctx->model);
    cJSON_AddNumberToObject(body, "max_tokens", ctx->max_tokens);
    cJSON_AddStringToObject(body, "system", request->system_prompt ? request->system_prompt : "");

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", request->user_prompt);
    cJSON_AddItemToArray(content, text_block);
    text_block = NULL;

    cJSON_AddStringToObject(image_block, "type", "image");
    cJSON_AddStringToObject(source, "type", "base64");
    cJSON_AddStringToObject(source, "media_type", mime);
    cJSON_AddStringToObject(source, "data", base64_data);
    cJSON_AddItemToObject(image_block, "source", source);
    source = NULL;
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
    http_request.auth_type = "none";
    http_request.timeout_ms = ctx->timeout_ms;
    http_request.headers = headers;
    http_request.header_count = sizeof(headers) / sizeof(headers[0]);

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
    free(mime);
    free(base64_data);
    free(post_data);
    free(url);
    cJSON_Delete(body);
    cJSON_Delete(messages);
    cJSON_Delete(user_msg);
    cJSON_Delete(content);
    cJSON_Delete(text_block);
    cJSON_Delete(image_block);
    cJSON_Delete(source);
    claw_llm_http_response_free(&http_response);
    claw_llm_response_free(&response);
    claw_media_prepared_free(&prepared);
    return err;
}

static void anthropic_deinit(void *backend_ctx)
{
    anthropic_backend_ctx_t *ctx = (anthropic_backend_ctx_t *)backend_ctx;

    if (!ctx) {
        return;
    }

    free(ctx->api_key);
    free(ctx->model);
    free(ctx->base_url);
    free(ctx);
}

const claw_llm_backend_vtable_t *claw_llm_backend_anthropic_vtable(void)
{
    static const claw_llm_backend_vtable_t vtable = {
        .id = "anthropic",
        .init = anthropic_init,
        .chat = anthropic_chat,
        .infer_media = anthropic_infer_media,
        .deinit = anthropic_deinit,
    };

    return &vtable;
}
