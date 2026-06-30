/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_event.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*claw_event_publish_fn)(const claw_event_t *event);

esp_err_t claw_event_router_publish(const claw_event_t *event);
esp_err_t claw_event_router_publish_message(const char *source_cap,
                                            const char *channel,
                                            const char *chat_id,
                                            const char *text,
                                            const char *sender_id,
                                            const char *message_id);
esp_err_t claw_event_router_publish_trigger(const char *source_cap,
                                            const char *event_type,
                                            const char *event_key,
                                            const char *payload_json);

#ifdef __cplusplus
}
#endif
