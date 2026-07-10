/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_event_publisher.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_SCHEDULER_ID_LEN 64
#define CAP_SCHEDULER_EXPR_LEN 64
#define CAP_SCHEDULER_EVENT_TYPE_LEN 32
#define CAP_SCHEDULER_EVENT_KEY_LEN 96
#define CAP_SCHEDULER_CHANNEL_LEN 16
#define CAP_SCHEDULER_CHAT_ID_LEN 96
#define CAP_SCHEDULER_CONTENT_TYPE_LEN 24
#define CAP_SCHEDULER_SESSION_POLICY_LEN 24
#define CAP_SCHEDULER_TEXT_LEN 256
#define CAP_SCHEDULER_PAYLOAD_LEN 512

typedef enum {
    CAP_SCHEDULER_ITEM_ONCE = 0,
    CAP_SCHEDULER_ITEM_INTERVAL = 1,
    CAP_SCHEDULER_ITEM_CRON = 2,
} cap_scheduler_item_kind_t;

typedef enum {
    CAP_SCHEDULER_STATUS_SCHEDULED = 0,
    CAP_SCHEDULER_STATUS_PAUSED = 1,
    CAP_SCHEDULER_STATUS_RUNNING = 2,
    CAP_SCHEDULER_STATUS_COMPLETED = 3,
    CAP_SCHEDULER_STATUS_ERROR = 4,
    CAP_SCHEDULER_STATUS_DISABLED = 5,
} cap_scheduler_status_t;

typedef struct {
    char id[CAP_SCHEDULER_ID_LEN];
    bool enabled;
    cap_scheduler_item_kind_t kind;
    int64_t start_at_ms;
    int64_t end_at_ms;
    int64_t interval_ms;
    char cron_expr[CAP_SCHEDULER_EXPR_LEN];
    char event_type[CAP_SCHEDULER_EVENT_TYPE_LEN];
    char event_key[CAP_SCHEDULER_EVENT_KEY_LEN];
    char source_channel[CAP_SCHEDULER_CHANNEL_LEN];
    char chat_id[CAP_SCHEDULER_CHAT_ID_LEN];
    char content_type[CAP_SCHEDULER_CONTENT_TYPE_LEN];
    char session_policy[CAP_SCHEDULER_SESSION_POLICY_LEN];
    char text[CAP_SCHEDULER_TEXT_LEN];
    char payload_json[CAP_SCHEDULER_PAYLOAD_LEN];
    int max_runs;
} cap_scheduler_item_t;

typedef struct {
    cap_scheduler_item_t item;
    cap_scheduler_status_t status;
    int64_t next_fire_ms;
    int64_t last_fire_ms;
    int64_t last_success_ms;
    int run_count;
    int missed_count;
    esp_err_t last_error_code;
} cap_scheduler_snapshot_t;

typedef claw_event_publish_fn cap_scheduler_publish_fn;

typedef struct {
    const char *schedules_path;
    uint32_t tick_ms;
    uint32_t max_items;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    BaseType_t task_core;
    claw_event_publish_fn publish_event;
    bool persist_after_fire;
} cap_scheduler_config_t;

esp_err_t cap_scheduler_register_group(void);
esp_err_t cap_scheduler_init(const cap_scheduler_config_t *config);
esp_err_t cap_scheduler_start(void);
esp_err_t cap_scheduler_stop(void);
esp_err_t cap_scheduler_reload(void);
esp_err_t cap_scheduler_add(const cap_scheduler_item_t *item);
esp_err_t cap_scheduler_update(const cap_scheduler_item_t *item);
esp_err_t cap_scheduler_remove(const char *id);
esp_err_t cap_scheduler_enable(const char *id, bool enabled);
esp_err_t cap_scheduler_pause(const char *id);
esp_err_t cap_scheduler_resume(const char *id);
esp_err_t cap_scheduler_trigger_now(const char *id);
esp_err_t cap_scheduler_get_snapshot(const char *id, cap_scheduler_snapshot_t *out);
esp_err_t cap_scheduler_list_json(char *buf, size_t size);
esp_err_t cap_scheduler_get_state_json(const char *id, char *buf, size_t size);
esp_err_t cap_scheduler_handle_time_sync(void);

#ifdef __cplusplus
}
#endif
