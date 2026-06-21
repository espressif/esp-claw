/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "pet_buddy.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gfx.h"
#include "match_watch_app.h"
#include "match_watch_internal.h"
#include "match_watch_platform.h"
#include "match_watch_pet.h"
#include "match_data.h"
#include "pet_registry.h"

static const char *const TAG = "match_watch";

static void match_watch_pet_touch_cb(gfx_obj_t *obj, const gfx_touch_event_t *event, void *user_data)
{
    (void)obj;
    (void)user_data;

    if (s_app->runtime.active_page == MATCH_WATCH_PAGE_TEAM) {
        return;
    }
    if (event != NULL) {
        if (event->type == GFX_TOUCH_EVENT_PRESS) {
            s_app->touch.pet_touch_active = true;
            s_app->touch.pet_touch_suppress = true;
            s_app->touch.pressed = false;
        } else if (event->type == GFX_TOUCH_EVENT_RELEASE) {
            s_app->touch.pet_touch_active = false;
        }
    }
    if (s_app->runtime.active_page != MATCH_WATCH_PAGE_TIME_HOME) {
        match_watch_app_note_user_browse();
    }
    match_watch_pet_handle_touch(&s_app->pet.handle, event);
}

static esp_err_t match_watch_app_attach_pet(void)
{
    ESP_RETURN_ON_ERROR(match_watch_platform_lock(), TAG, "lock platform failed");

    gfx_obj_t *pet_obj = match_watch_pet_object(&s_app->pet.handle);
    if (pet_obj != NULL) {
        (void)gfx_obj_set_touch_cb(pet_obj, match_watch_pet_touch_cb, NULL);
    }
    match_watch_platform_unlock();
    return ESP_OK;
}

static esp_err_t match_watch_app_open_pet(void)
{
    esp_err_t ret = match_watch_pet_open(&s_app->pet.handle, disp_default);

    if (ret != ESP_OK && pet_registry_has_selected()) {
        ESP_LOGW(TAG, "selected pet failed, fallback to built-in pet: %s", esp_err_to_name(ret));
        (void)pet_buddy_clear_selected();
        ret = match_watch_pet_open(&s_app->pet.handle, disp_default);
    }
    if (ret == ESP_OK) {
        ret = match_watch_app_attach_pet();
    }
    return ret;
}

static void match_watch_app_hold_initial_data_on_match_page(void)
{
    if (!s_app->runtime.initial_data_hold_done && match_data_has_live_data()) {
        s_app->runtime.initial_data_hold_done = true;
        s_app->runtime.initial_match_until_ms = esp_log_timestamp() + MATCH_WATCH_USER_BROWSE_HOLD_MS;
        ESP_LOGI(TAG, "initial match data ready; keep match page for %u ms",
                 (unsigned)MATCH_WATCH_USER_BROWSE_HOLD_MS);
    }
}

esp_err_t match_watch_app_reload_pet(void)
{
    esp_err_t ret;

    match_watch_pet_close(&s_app->pet.handle);
    match_watch_pet_logic_reset(&s_app->pet.logic);
    ret = match_watch_app_open_pet();
    ESP_RETURN_ON_ERROR(ret, TAG, "pet reload failed");
    match_watch_app_render_page(s_app->runtime.active_page);
    return ESP_OK;
}

void match_watch_app_apply_data_source(match_watch_data_source_t source)
{
    match_data_schedule_item_t match;
    bool hold_active;

    if (source != MATCH_WATCH_DATA_SOURCE_LIVE &&
            source != MATCH_WATCH_DATA_SOURCE_EXTERNAL) {
        return;
    }
    if (s_app->runtime.data_source == source) {
        return;
    }
    s_app->runtime.data_source = source;
    s_app->runtime.net_sync_done = true;
    s_app->notify.match_no = 0;
    s_app->notify.home_score = 0;
    s_app->notify.away_score = 0;
    s_app->notify.last_goal_total = 0;
    s_app->notify.state = MATCH_DATA_MATCH_UPCOMING;
    s_app->notify.kickoff_sent = false;
    s_app->notify.half_time_sent = false;
    s_app->notify.full_time_sent = false;

    ESP_LOGI(TAG, "match data source switched: %s; provider owns refresh",
             source == MATCH_WATCH_DATA_SOURCE_EXTERNAL ? "external" : "live");
    match_watch_app_hold_initial_data_on_match_page();
    hold_active = match_watch_app_user_browse_hold_active();
    if (!hold_active && s_app->selection.host_team_selected &&
            match_watch_app_pick_best_team_match(s_app->selection.host_team, &match)) {
        match_watch_app_select_match(&match, false);
        ESP_LOGI(TAG, "data source preferred match: source=%s team=%s match=%u %s vs %s state=%d",
                 source == MATCH_WATCH_DATA_SOURCE_EXTERNAL ? "external" : "live",
                 s_app->selection.host_team,
                 (unsigned)match.match_no, match.home, match.away, (int)match.state);
    }
    match_watch_app_render_page(s_app->runtime.active_page);
}

static void match_watch_app_handle_data_changed_event(void)
{
    match_data_schedule_item_t match;
    match_data_schedule_item_t best;
    bool has_current;
    bool has_best;
    bool hold_active;

    s_app->runtime.net_sync_done = true;
    if (!match_watch_app_full_time_review_hold_active()) {
        s_app->notify.match_switched_sent = false;
    }
    match_watch_app_refresh_team_options(true);
    match_watch_app_hold_initial_data_on_match_page();
    hold_active = match_watch_app_user_browse_hold_active();
    has_current = match_watch_app_get_current_match(&match);
    if (has_current) {
        match_watch_app_sync_full_time_review(&match);
    }
    has_best = s_app->selection.host_team_selected &&
               match_watch_app_pick_best_team_match(s_app->selection.host_team, &best);
    if (!hold_active && has_best && (!has_current || match_watch_app_should_switch_from_current(&match, &best))) {
        match_watch_app_select_match(&best, false);
    } else if (!hold_active && !has_current) {
        match_watch_app_select_default_host_team();
    }
    match_watch_app_check_notifications();
    match_watch_app_render_page(s_app->runtime.active_page);
}

static void match_watch_app_handle_external_event(const match_watch_app_external_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case MATCH_WATCH_EXTERNAL_EVENT_CLOSE:
        s_app->runtime.should_stop = true;
        break;

    case MATCH_WATCH_EXTERNAL_EVENT_DATA_CHANGED:
        match_watch_app_handle_data_changed_event();
        break;

    case MATCH_WATCH_EXTERNAL_EVENT_PET_RELOAD:
        (void)match_watch_app_reload_pet();
        break;

    case MATCH_WATCH_EXTERNAL_EVENT_FAVORITE_TEAM:
        if (event->text[0] == '\0') {
            match_watch_app_clear_host_team(true);
        } else {
            match_watch_app_set_host_team_with_source(event->text, true, event->host_team_source);
        }
        match_watch_app_render_page(s_app->runtime.active_page);
        break;

    case MATCH_WATCH_EXTERNAL_EVENT_DATA_SOURCE:
        match_watch_app_apply_data_source(event->data_source);
        break;

    case MATCH_WATCH_EXTERNAL_EVENT_REMINDERS:
        s_app->runtime.reminders_enabled = event->enabled;
        ESP_LOGI(TAG, "match reminders %s", event->enabled ? "enabled" : "disabled");
        break;

    default:
        ESP_LOGW(TAG, "unknown external event: type=%d", (int)event->type);
        break;
    }
}

void match_watch_app_process_external_requests(void)
{
    match_watch_app_external_event_t event;

    if (match_watch_app_ensure_external_queue() != ESP_OK) {
        return;
    }

    while (xQueueReceive(s_app->external.queue, &event, 0) == pdTRUE) {
        match_watch_app_handle_external_event(&event);
        if (s_app->runtime.should_stop) {
            break;
        }
    }
}

static void match_watch_app_run(void)
{
    match_watch_platform_set_touch_event_cb(match_watch_touch_event_cb, NULL);

    match_watch_app_load_host_team();

    assert(match_watch_platform_lock() == ESP_OK);
    assert(disp_default != NULL);
    (void)gfx_disp_set_bg_color(disp_default, match_watch_platform_color(MATCH_WATCH_COLOR_BG));
    gfx_disp_refresh_all(disp_default);
    match_watch_platform_unlock();

    assert(match_watch_platform_lock() == ESP_OK);
    match_watch_app_create_ui(disp_default);
    match_watch_platform_unlock();

    esp_err_t pet_ret = match_watch_app_open_pet();
    if (pet_ret != ESP_OK) {
        ESP_LOGW(TAG, "pet open failed, continue without pet: %s", esp_err_to_name(pet_ret));
    }

    match_watch_app_render_page(match_watch_app_should_show_time_home() ?
                                MATCH_WATCH_PAGE_TIME_HOME : MATCH_WATCH_PAGE_TEAM);
    s_app->runtime.net_sync_done = true;

    while (!s_app->runtime.should_stop) {
        vTaskDelay(pdMS_TO_TICKS(MATCH_WATCH_LOOP_DELAY_MS));
        match_watch_app_process_external_requests();
        if (s_app->runtime.should_stop) {
            break;
        }
        if (match_watch_app_select_best_match_if_user_idle()) {
            match_watch_app_render_page(s_app->runtime.active_page);
        }
        if (s_app->runtime.active_page == MATCH_WATCH_PAGE_TIME_HOME &&
                !match_watch_app_should_show_time_home()) {
            match_watch_app_render_page(MATCH_WATCH_PAGE_TEAM);
        } else if (s_app->runtime.active_page != MATCH_WATCH_PAGE_TIME_HOME &&
                   match_watch_app_active_match_is_far() &&
                   !match_watch_app_initial_match_hold_active() &&
                   !match_watch_app_user_browse_hold_active() &&
                   !match_watch_app_full_time_review_hold_active()) {
            match_watch_app_render_page(MATCH_WATCH_PAGE_TIME_HOME);
        }
        if (s_app->runtime.active_page == MATCH_WATCH_PAGE_TEAM) {
            match_watch_app_refresh_home_live_state();
        } else if (s_app->runtime.active_page == MATCH_WATCH_PAGE_TIME_HOME) {
            match_watch_app_refresh_home_live_state();
        }
        match_watch_app_check_notifications();
    }
    match_watch_pet_close(&s_app->pet.handle);
    match_watch_app_destroy_ui();
}

esp_err_t match_watch_app_main(void)
{
    match_watch_platform_session_t session;
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(match_watch_app_ensure_context(), TAG, "create app context failed");
    if (s_app->runtime.running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_app->external.queue != NULL) {
        (void)xQueueReset(s_app->external.queue);
    }
    s_app->runtime.running = true;
    s_app->runtime.should_stop = false;
    s_app->runtime.close_requested = false;
    s_app->runtime.initial_data_hold_done = false;
    s_app->runtime.initial_match_until_ms = 0;
    ret = match_watch_platform_open(&session);
    ESP_GOTO_ON_ERROR(ret, fail_clear_running, TAG, "open platform failed");
    match_watch_app_run();
    match_watch_platform_close(&session);
    s_app->runtime.running = false;
    return ESP_OK;

fail_clear_running:
    s_app->runtime.running = false;
    return ret;
}
