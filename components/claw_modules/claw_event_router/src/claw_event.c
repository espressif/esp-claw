/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_event.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_event";

esp_err_t claw_event_clone(const claw_event_t *src, claw_event_t *dst)
{
    if (!src || !dst) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, sizeof(*dst));
    dst->text = NULL;
    dst->payload_json = NULL;

    if (src->text) {
        dst->text = strdup(src->text);
        if (!dst->text) {
            ESP_LOGE(TAG, "Failed to clone event text");
            return ESP_ERR_NO_MEM;
        }
    }
    if (src->payload_json) {
        dst->payload_json = strdup(src->payload_json);
        if (!dst->payload_json) {
            free(dst->text);
            dst->text = NULL;
            ESP_LOGE(TAG, "Failed to clone event payload");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void claw_event_free(claw_event_t *event)
{
    if (!event) {
        return;
    }
    free(event->text);
    free(event->payload_json);
    memset(event, 0, sizeof(*event));
}

const char *claw_event_session_policy_to_string(claw_session_policy_t policy)
{
    switch (policy) {
    case CLAW_SESSION_POLICY_CHAT:
        return "chat";
    case CLAW_SESSION_POLICY_TRIGGER:
        return "trigger";
    case CLAW_SESSION_POLICY_GLOBAL:
        return "global";
    case CLAW_SESSION_POLICY_EPHEMERAL:
        return "ephemeral";
    case CLAW_SESSION_POLICY_NOSAVE:
        return "nosave";
    default:
        return "chat";
    }
}
