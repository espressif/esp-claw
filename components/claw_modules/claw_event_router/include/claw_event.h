/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "claw_session_mgr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char event_id[48];
    char source_cap[32];
    char event_type[32];
    char source_channel[16];
    char target_channel[16];
    char source_endpoint[64];
    char target_endpoint[96];
    char chat_id[96];
    char sender_id[96];
    char message_id[96];
    char correlation_id[96];
    char content_type[24];
    int64_t timestamp_ms;
    claw_session_policy_t session_policy;
    char *text;
    char *payload_json;
} claw_event_t;

esp_err_t claw_event_clone(const claw_event_t *src, claw_event_t *dst);
void claw_event_free(claw_event_t *event);
const char *claw_event_session_policy_to_string(claw_session_policy_t policy);

#ifdef __cplusplus
}
#endif
