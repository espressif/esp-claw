/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_agent_reply.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_agent.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_agent_reply";

#define CAP_AGENT_REPLY_FIELD_LEN       96
#define CAP_AGENT_REPLY_OUTPUT_SIZE     256
#define CAP_AGENT_REPLY_TASK_STACK_SIZE 8192
/* Overall budget for a whole turn to complete. */
#define CAP_AGENT_REPLY_TIMEOUT_MS      (10 * 60 * 1000)
/* Per-receive wait: the turn is pulled event-by-event in a loop; each call
 * blocks at most this long before we re-check the overall deadline. */
#define CAP_AGENT_REPLY_RECV_SLICE_MS   5000

typedef struct {
    uint32_t session_id;
    char channel[CAP_AGENT_REPLY_FIELD_LEN];
    char chat_id[CAP_AGENT_REPLY_FIELD_LEN];
    char correlation_id[CAP_AGENT_REPLY_FIELD_LEN];
} cap_agent_reply_task_arg_t;

static bool cap_agent_str_empty(const char *value)
{
    return !value || !value[0];
}

static const char *cap_agent_send_capability(const char *channel)
{
    if (cap_agent_str_empty(channel)) {
        return NULL;
    }
    if (strcmp(channel, "feishu") == 0) {
        return "feishu_send_message";
    }
    if (strcmp(channel, "qq") == 0) {
        return "qq_send_message";
    }
    if (strcmp(channel, "tg") == 0 || strcmp(channel, "telegram") == 0) {
        return "tg_send_message";
    }
    if (strcmp(channel, "wechat") == 0) {
        return "wechat_send_message";
    }
    if (strcmp(channel, "local") == 0 || strcmp(channel, "web") == 0) {
        return "local_send_message";
    }
    return NULL;
}

bool cap_agent_reply_route_supported(const char *channel, const char *chat_id)
{
    return !cap_agent_str_empty(chat_id) && cap_agent_send_capability(channel) != NULL;
}

static char *cap_agent_build_message_payload(const cap_agent_reply_task_arg_t *arg,
                                             const char *message)
{
    cJSON *root = NULL;
    char *payload = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    if (!cJSON_AddStringToObject(root, "channel", arg->channel) ||
            !cJSON_AddStringToObject(root, "chat_id", arg->chat_id) ||
            !cJSON_AddStringToObject(root, "message", message)) {
        cJSON_Delete(root);
        return NULL;
    }
    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static esp_err_t cap_agent_send_reply(const cap_agent_reply_task_arg_t *arg,
                                      const char *message)
{
    const char *cap_name = cap_agent_send_capability(arg->channel);
    char session_id[16];
    char *payload = NULL;
    char *output = NULL;
    claw_cap_call_context_t call_ctx = {0};
    esp_err_t err;

    if (!cap_name || cap_agent_str_empty(message)) {
        return ESP_OK;
    }

    payload = cap_agent_build_message_payload(arg, message);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    output = calloc(1, CAP_AGENT_REPLY_OUTPUT_SIZE);
    if (!output) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }

    snprintf(session_id, sizeof(session_id), "%" PRIu32, arg->session_id);
    call_ctx.session_id = session_id;
    call_ctx.channel = arg->channel;
    call_ctx.chat_id = arg->chat_id;
    call_ctx.target_channel = arg->channel;
    call_ctx.target_chat_id = arg->chat_id;
    call_ctx.source_cap = "cap_agent";
    call_ctx.correlation_id = arg->correlation_id[0] ? arg->correlation_id : NULL;
    call_ctx.caller = CLAW_CAP_CALLER_SYSTEM;

    err = claw_cap_call(cap_name, payload, &call_ctx, output, CAP_AGENT_REPLY_OUTPUT_SIZE);
    ESP_LOGI(TAG,
             "send_reply cap=%s session=%" PRIu32 " err=%s output=%s",
             cap_name,
             arg->session_id,
             esp_err_to_name(err),
             output[0] ? output : "-");

    free(output);
    free(payload);
    return err;
}

/* Append `add` to a growable heap buffer. Returns false on allocation failure
 * (the existing buffer is left intact for the caller to free). */
static bool cap_agent_reply_append(char **buf, size_t *len, const char *add)
{
    if (!add || !add[0]) {
        return true;
    }
    size_t add_len = strlen(add);
    char *grown = realloc(*buf, *len + add_len + 1);
    if (!grown) {
        return false;
    }
    memcpy(grown + *len, add, add_len + 1);
    *len += add_len;
    *buf = grown;
    return true;
}

static void cap_agent_reply_task(void *param)
{
    cap_agent_reply_task_arg_t *arg = (cap_agent_reply_task_arg_t *)param;
    char *message = NULL;
    size_t message_len = 0;
    bool failed = false;
    bool done = false;
    TickType_t start = xTaskGetTickCount();

    /* Consume the turn's event stream one event at a time. Content events arrive
     * while the turn is still running; we coalesce OUTPUT fragments into one
     * coherent IM message and send it once the turn ends (DONE). */
    while (!done) {
        claw_agent_event_t event = {0};
        esp_err_t err = claw_agent_session_receive(arg->session_id,
                                                   &event,
                                                   CAP_AGENT_REPLY_RECV_SLICE_MS);
        if (err == ESP_ERR_TIMEOUT) {
            if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(CAP_AGENT_REPLY_TIMEOUT_MS)) {
                ESP_LOGW(TAG,
                         "receive overall timeout session=%" PRIu32,
                         arg->session_id);
                break;
            }
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "receive failed session=%" PRIu32 " err=%s",
                     arg->session_id,
                     esp_err_to_name(err));
            break;
        }

        switch (event.kind) {
        case CLAW_AGENT_EVENT_KIND_OUTPUT:
            if (!cap_agent_reply_append(&message, &message_len, event.text)) {
                ESP_LOGW(TAG, "reply buffer alloc failed session=%" PRIu32, arg->session_id);
                failed = true;
                done = true;
            }
            break;
        case CLAW_AGENT_EVENT_KIND_REASONING:
        case CLAW_AGENT_EVENT_KIND_TOOLS:
            ESP_LOGI(TAG,
                     "progress session=%" PRIu32 " kind=%d %s",
                     arg->session_id,
                     (int)event.kind,
                     event.text ? event.text : "-");
            break;
        case CLAW_AGENT_EVENT_KIND_ERROR:
            ESP_LOGW(TAG,
                     "agent error session=%" PRIu32 " error=%s",
                     arg->session_id,
                     event.error_message ? event.error_message : "-");
            failed = true;
            done = true;
            break;
        case CLAW_AGENT_EVENT_KIND_DONE:
        case CLAW_AGENT_EVENT_KIND_CLOSED:
            done = true;
            break;
        default:
            break;
        }

        claw_agent_event_free(&event);
    }

    if (!failed && message) {
        esp_err_t err = cap_agent_send_reply(arg, message);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "reply send failed session=%" PRIu32 " err=%s",
                     arg->session_id,
                     esp_err_to_name(err));
        }
    }

    free(message);
    free(arg);
    vTaskDelete(NULL);
}

esp_err_t cap_agent_reply_start(uint32_t session_id,
                                const char *channel,
                                const char *chat_id,
                                const char *correlation_id)
{
    cap_agent_reply_task_arg_t *arg = NULL;

    if (session_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_agent_reply_route_supported(channel, chat_id)) {
        return ESP_OK;
    }

    arg = calloc(1, sizeof(*arg));
    if (!arg) {
        return ESP_ERR_NO_MEM;
    }
    arg->session_id = session_id;
    strlcpy(arg->channel, channel, sizeof(arg->channel));
    strlcpy(arg->chat_id, chat_id, sizeof(arg->chat_id));
    if (correlation_id) {
        strlcpy(arg->correlation_id, correlation_id, sizeof(arg->correlation_id));
    }

    if (xTaskCreate(cap_agent_reply_task,
                    "cap_agent_reply",
                    CAP_AGENT_REPLY_TASK_STACK_SIZE,
                    arg,
                    tskIDLE_PRIORITY + 1,
                    NULL) != pdPASS) {
        free(arg);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
