/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

typedef enum {
    CLAW_MEDIA_ASSET_KIND_LOCAL_PATH = 0,
    CLAW_MEDIA_ASSET_KIND_REMOTE_URL = 1,
    CLAW_MEDIA_ASSET_KIND_INLINE_BYTES = 2,
} claw_media_asset_kind_t;

typedef enum {
    CLAW_MEDIA_PREPARED_KIND_DATA_URL = 0,
    CLAW_MEDIA_PREPARED_KIND_REMOTE_URL = 1,
} claw_media_prepared_kind_t;

typedef struct {
    claw_media_asset_kind_t kind;
    const char *path;
    const char *url;
    const uint8_t *bytes;
    size_t byte_count;
    const char *mime_type;
} claw_media_asset_t;

typedef struct {
    claw_media_prepared_kind_t kind;
    char *payload;
    char mime_type[32];
    size_t original_size;
} claw_media_prepared_t;

typedef struct {
    const char *id;
    const char *default_backend_type;
    const char *default_base_url;
    const char *chat_path;
    const char *max_tokens_field;
    uint32_t default_timeout_ms;
    size_t default_image_max_bytes;
    bool supports_tools;
    bool supports_vision;
    bool image_remote_url_only;
} claw_llm_model_profile_t;

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *profile;
    const char *model;
    const char *base_url;
    const char *auth_type;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
} claw_llm_runtime_config_t;

typedef struct {
    char *id;
    char *name;
    char *arguments_json;
} claw_llm_tool_call_t;

typedef struct {
    char *text;
    char *reasoning_content;
    claw_llm_tool_call_t *tool_calls;
    size_t tool_call_count;
} claw_llm_response_t;

typedef struct {
    const char *system_prompt;
    cJSON *messages;
    const char *tools_json;
} claw_llm_chat_request_t;

typedef struct {
    const char *system_prompt;
    const char *user_prompt;
    const claw_media_asset_t *media;
    size_t media_count;
} claw_llm_media_request_t;

typedef struct {
    const char *name;
    const char *value;
} claw_llm_http_header_t;

typedef struct {
    const char *url;
    const char *body;
    const char *api_key;
    const char *auth_type;
    uint32_t timeout_ms;
    const claw_llm_http_header_t *headers;
    size_t header_count;
} claw_llm_http_json_request_t;

typedef struct {
    char *body;
    int status_code;
} claw_llm_http_response_t;
