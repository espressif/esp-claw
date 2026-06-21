/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdbool.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "match_watch_app.h"
#include "match_watch_ctx.h"
#include "match_data.h"
#include "match_watch_runtime.h"

static const char *const TAG = "match_watch";

const match_watch_pet_policy_t match_watch_app_pet_policy = {
    .limited_ms = MATCH_WATCH_PET_LIMITED_MS,
    .random_min_ms = MATCH_WATCH_PET_RANDOM_MIN_MS,
    .random_span_ms = MATCH_WATCH_PET_RANDOM_SPAN_MS,
    .random_hold_ms = MATCH_WATCH_PET_RANDOM_HOLD_MS,
    .wait_wave_min_ms = MATCH_WATCH_PET_WAIT_WAVE_MIN_MS,
    .wait_wave_span_ms = MATCH_WATCH_PET_WAIT_WAVE_SPAN_MS,
    .wait_wave_hold_ms = MATCH_WATCH_PET_WAIT_WAVE_HOLD_MS,
    .live_run_min_ms = MATCH_WATCH_PET_LIVE_RUN_MIN_MS,
    .live_run_span_ms = MATCH_WATCH_PET_LIVE_RUN_SPAN_MS,
};

const match_data_stage_t match_watch_app_stage_order[MATCH_WATCH_APP_STAGE_ORDER_COUNT] = {
    MATCH_DATA_STAGE_GROUP,
    MATCH_DATA_STAGE_ROUND_OF_32,
    MATCH_DATA_STAGE_ROUND_OF_16,
    MATCH_DATA_STAGE_QUARTER_FINAL,
    MATCH_DATA_STAGE_SEMI_FINAL,
    MATCH_DATA_STAGE_THIRD_PLACE,
    MATCH_DATA_STAGE_FINAL,
};

static match_watch_app_ctx_t *s_app;
static match_watch_runtime_notification_cb_t s_notification_cb;
static void *s_notification_user_data;

match_watch_app_ctx_t *match_watch_app_ctx(void)
{
    return s_app;
}

esp_err_t match_watch_app_ensure_context(void)
{
    if (s_app != NULL) {
        return ESP_OK;
    }

    s_app = heap_caps_calloc(1, sizeof(*s_app), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_app == NULL) {
        s_app = heap_caps_calloc(1, sizeof(*s_app), MALLOC_CAP_8BIT);
    }
    if (s_app == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_app->selection.selected_stage = MATCH_DATA_STAGE_GROUP;
    s_app->timing.phase = MATCH_WATCH_HOME_PHASE_FAR;
    s_app->runtime.data_source = MATCH_WATCH_DATA_SOURCE_LIVE;
    s_app->runtime.reminders_enabled = true;
    return ESP_OK;
}

esp_err_t match_watch_app_ensure_external_queue(void)
{
    ESP_RETURN_ON_ERROR(match_watch_app_ensure_context(), TAG, "create app context failed");

    if (s_app->external.queue == NULL) {
        s_app->external.queue = xQueueCreate(MATCH_WATCH_EXTERNAL_EVENT_QUEUE_LEN,
                                             sizeof(match_watch_app_external_event_t));
    }

    return s_app->external.queue != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t match_watch_ctx_set_notification_callback(match_watch_runtime_notification_cb_t cb, void *user_data)
{
    s_notification_cb = cb;
    s_notification_user_data = user_data;
    return ESP_OK;
}

void match_watch_app_emit_notification(const char *kind,
                                       const match_data_schedule_item_t *match,
                                       const char *message)
{
    match_watch_runtime_notification_t notification = {0};

    if (s_notification_cb == NULL || kind == NULL || match == NULL || message == NULL ||
            message[0] == '\0') {
        return;
    }

    if (s_app->runtime.notify_channel[0] == '\0' || s_app->runtime.notify_chat_id[0] == '\0') {
        ESP_LOGW(TAG, "match watch notify skipped: kind=%s match=%u notify target missing",
                 kind, match != NULL ? (unsigned)match->match_no : 0U);
        return;
    }

    notification.match_no = match->match_no;
    notification.source = s_app->runtime.data_source;
    strlcpy(notification.kind, kind, sizeof(notification.kind));
    strlcpy(notification.message, message, sizeof(notification.message));
    strlcpy(notification.channel, s_app->runtime.notify_channel, sizeof(notification.channel));
    strlcpy(notification.chat_id, s_app->runtime.notify_chat_id, sizeof(notification.chat_id));
    s_notification_cb(&notification, s_notification_user_data);
}
