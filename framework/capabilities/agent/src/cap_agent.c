/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_agent.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_agent_reply.h"
#include "cJSON.h"
#include "claw_agent.h"
#include "claw_cap.h"
#include "claw_event_router.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CAP_AGENT_MAX_ROUTE_SESSIONS 32
#define CAP_AGENT_ROUTE_KEY_LEN      96

typedef struct {
    bool occupied;
    char channel[CAP_AGENT_ROUTE_KEY_LEN];
    char chat_id[CAP_AGENT_ROUTE_KEY_LEN];
    uint32_t session_id;
} cap_agent_route_session_t;

static cap_agent_route_session_t s_route_sessions[CAP_AGENT_MAX_ROUTE_SESSIONS];
static SemaphoreHandle_t s_route_mutex;

static bool cap_agent_str_empty(const char *value)
{
    return !value || !value[0];
}

static esp_err_t cap_agent_ensure_route_mutex(void)
{
    if (s_route_mutex) {
        return ESP_OK;
    }

    s_route_mutex = xSemaphoreCreateMutex();
    return s_route_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t cap_agent_lock_routes(void)
{
    esp_err_t err = cap_agent_ensure_route_mutex();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_route_mutex, portMAX_DELAY);
    return ESP_OK;
}

static void cap_agent_unlock_routes(void)
{
    xSemaphoreGive(s_route_mutex);
}

static bool cap_agent_parse_u32(const char *value, uint32_t *out)
{
    char *end = NULL;
    unsigned long parsed;

    if (cap_agent_str_empty(value) || !out) {
        return false;
    }

    parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return false;
    }

    *out = (uint32_t)parsed;
    return true;
}

static esp_err_t cap_agent_get_route_session(const char *channel,
                                             const char *chat_id,
                                             uint32_t *out_session_id)
{
    size_t free_slot = CAP_AGENT_MAX_ROUTE_SESSIONS;
    esp_err_t err;

    if (cap_agent_str_empty(channel) || cap_agent_str_empty(chat_id) || !out_session_id) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_agent_lock_routes();
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < CAP_AGENT_MAX_ROUTE_SESSIONS; i++) {
        cap_agent_route_session_t *entry = &s_route_sessions[i];
        if (!entry->occupied) {
            if (free_slot == CAP_AGENT_MAX_ROUTE_SESSIONS) {
                free_slot = i;
            }
            continue;
        }
        if (strcmp(entry->channel, channel) == 0 && strcmp(entry->chat_id, chat_id) == 0) {
            *out_session_id = entry->session_id;
            cap_agent_unlock_routes();
            return ESP_OK;
        }
    }

    if (free_slot == CAP_AGENT_MAX_ROUTE_SESSIONS) {
        cap_agent_unlock_routes();
        return ESP_ERR_NO_MEM;
    }

    err = claw_agent_session_create(out_session_id);
    if (err != ESP_OK) {
        cap_agent_unlock_routes();
        return err;
    }
    err = claw_agent_session_open(*out_session_id);
    if (err != ESP_OK) {
        cap_agent_unlock_routes();
        return err;
    }

    cap_agent_route_session_t *entry = &s_route_sessions[free_slot];
    entry->occupied = true;
    strlcpy(entry->channel, channel, sizeof(entry->channel));
    strlcpy(entry->chat_id, chat_id, sizeof(entry->chat_id));
    entry->session_id = *out_session_id;
    cap_agent_unlock_routes();
    return ESP_OK;
}

static esp_err_t cap_agent_select_session(const claw_cap_call_context_t *ctx,
                                          uint32_t *out_session_id)
{
    const char *channel = NULL;
    const char *chat_id = NULL;

    if (!out_session_id) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx && !cap_agent_str_empty(ctx->session_id)) {
        if (cap_agent_parse_u32(ctx->session_id, out_session_id)) {
            return ESP_OK;
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx) {
        channel = !cap_agent_str_empty(ctx->channel) ? ctx->channel : ctx->target_channel;
        chat_id = !cap_agent_str_empty(ctx->chat_id) ? ctx->chat_id : ctx->target_chat_id;
    }
    if (!cap_agent_str_empty(channel) && !cap_agent_str_empty(chat_id)) {
        return cap_agent_get_route_session(channel, chat_id, out_session_id);
    }

    return ESP_ERR_INVALID_ARG;
}

static const char *cap_agent_text_from_json(const char *input_json, cJSON **out_root)
{
    cJSON *root = NULL;
    const char *text = "";

    if (out_root) {
        *out_root = NULL;
    }
    if (cap_agent_str_empty(input_json)) {
        return text;
    }

    root = cJSON_Parse(input_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }

    text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    if (!text) {
        text = "";
    }
    if (out_root) {
        *out_root = root;
    } else {
        cJSON_Delete(root);
    }
    return text;
}

static esp_err_t cap_agent_execute(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size)
{
    cJSON *root = NULL;
    const char *text = cap_agent_text_from_json(input_json, &root);
    const char *reply_channel = NULL;
    const char *reply_chat_id = NULL;
    const char *correlation_id = NULL;
    uint32_t session_id = 0;
    bool route_reply = false;
    esp_err_t err;

    if (!text) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "invalid input json");
        }
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_agent_select_session(ctx, &session_id);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        if (output && output_size > 0) {
            snprintf(output, output_size, "session selection failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    if (ctx) {
        reply_channel = !cap_agent_str_empty(ctx->target_channel) ? ctx->target_channel : ctx->channel;
        reply_chat_id = !cap_agent_str_empty(ctx->target_chat_id) ? ctx->target_chat_id : ctx->chat_id;
        correlation_id = ctx->correlation_id;
    }
    route_reply = cap_agent_reply_route_supported(reply_channel, reply_chat_id);

    err = claw_agent_session_submit(session_id, text);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        if (output && output_size > 0) {
            snprintf(output, output_size, "agent submit failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    if (route_reply) {
        err = cap_agent_reply_start(session_id,
                                    reply_channel,
                                    reply_chat_id,
                                    correlation_id);
        if (err != ESP_OK) {
            if (output && output_size > 0) {
                snprintf(output, output_size, "reply task failed: %s", esp_err_to_name(err));
            }
            return err;
        }
    }

    if (output && output_size > 0) {
        if (route_reply) {
            snprintf(output, output_size, "session_id=%" PRIu32 " reply_routed", session_id);
        } else {
            snprintf(output, output_size, "session_id=%" PRIu32 " reply_unrouted", session_id);
        }
    }
    return ESP_OK;
}

/* Agent runtime exposed through the claw_cap registry. The descriptor is a
 * system entry point (no CLAW_CAP_FLAG_CALLABLE_BY_LLM) so the event router and
 * other subsystems can invoke it via claw_cap_call without it becoming a tool
 * the model can call recursively. Route-to-session mapping is owned here, not
 * in claw-cabi. */
static const claw_cap_descriptor_t s_agent_descriptors[] = {
    {
        .id = CLAW_EVENT_ROUTER_AGENT_CAP_ID,
        .name = CLAW_EVENT_ROUTER_AGENT_CAP_ID,
        .family = "agent",
        .description = "Submit an inbound message to the agent runtime.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}}}",
        .execute = cap_agent_execute,
    },
};

static const claw_cap_group_t s_agent_group = {
    .group_id = "cap_agent",
    .descriptors = s_agent_descriptors,
    .descriptor_count = sizeof(s_agent_descriptors) / sizeof(s_agent_descriptors[0]),
};

esp_err_t cap_agent_register_group(void)
{
    esp_err_t err = cap_agent_ensure_route_mutex();
    if (err != ESP_OK) {
        return err;
    }
    if (claw_cap_group_exists(s_agent_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_agent_group);
}
