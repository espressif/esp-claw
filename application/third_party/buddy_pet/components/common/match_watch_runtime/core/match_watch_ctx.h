/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include "esp_err.h"
#include "match_watch_types.h"
#include "match_data.h"
#include "match_watch_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif


extern const match_watch_pet_policy_t match_watch_app_pet_policy;

#define MATCH_WATCH_APP_STAGE_ORDER_COUNT 7
extern const match_data_stage_t match_watch_app_stage_order[MATCH_WATCH_APP_STAGE_ORDER_COUNT];

match_watch_app_ctx_t *match_watch_app_ctx(void);

esp_err_t match_watch_app_ensure_context(void);
esp_err_t match_watch_app_ensure_external_queue(void);
esp_err_t match_watch_ctx_set_notification_callback(match_watch_runtime_notification_cb_t cb,
                                                    void *user_data);

void match_watch_app_emit_notification(const char *kind,
                                       const match_data_schedule_item_t *match,
                                       const char *message);

#ifdef __cplusplus
}
#endif
