/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include "esp_check.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

#define SYSTEM_UI_JOBS_PANEL_Y_OFFSET_PX 50

static void system_ui_jobs_refresh_locked(void);

void system_ui_jobs_request_refresh_locked(void)
{
    if (s_ui.jobs_refresh_running) {
        s_ui.jobs_refresh_pending = true;
        return;
    }

    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_JOBS_REFRESH,
        .generation = s_ui.generation,
    };

    s_ui.jobs_refresh_running = true;
    s_ui.jobs_refresh_pending = false;
    if (system_ui_post_work_event(&event, 0) != ESP_OK) {
        s_ui.jobs_refresh_running = false;
    }
}

static void system_ui_jobs_start_stop_task_locked(const char *job_id, bool stop_all)
{
    system_ui_job_action_cb_t action_cb = NULL;
    void *action_user_ctx = NULL;
    system_ui_jobs_stop_all_cb_t stop_all_cb = NULL;
    void *stop_all_user_ctx = NULL;

    if (s_ui.jobs_stop_task_running) {
        return;
    }
    if (system_ui_callback_lock() != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "skip jobs action: callback lock failed");
        return;
    }
    action_cb = s_ui.jobs_action_cb;
    action_user_ctx = s_ui.jobs_action_user_ctx;
    stop_all_cb = s_ui.jobs_stop_all_cb;
    stop_all_user_ctx = s_ui.jobs_stop_all_user_ctx;
    system_ui_callback_unlock();

    if (stop_all) {
        if (!stop_all_cb) {
            return;
        }
    } else if (!job_id || !job_id[0] || !action_cb) {
        return;
    }

    system_ui_work_event_t event = {
        .type = SYSTEM_UI_WORK_EVENT_JOBS_ACTION,
        .generation = s_ui.generation,
    };
    event.jobs_action.stop_all = stop_all;
    if (!stop_all) {
        strlcpy(event.jobs_action.job_id, job_id, sizeof(event.jobs_action.job_id));
    }
    event.jobs_action.action_cb = action_cb;
    event.jobs_action.action_user_ctx = action_user_ctx;
    event.jobs_action.stop_all_cb = stop_all_cb;
    event.jobs_action.stop_all_user_ctx = stop_all_user_ctx;

    s_ui.jobs_stop_task_running = true;
    if (system_ui_post_work_event(&event, 0) != ESP_OK) {
        s_ui.jobs_stop_task_running = false;
    }
}

static void system_ui_jobs_close_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    system_ui_jobs_set_visible_locked(false);
}

static void system_ui_jobs_stop_all_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    system_ui_jobs_start_stop_task_locked(NULL, true);
}

static void system_ui_jobs_stop_event_cb(lv_event_t *event)
{
    system_ui_jobs_row_t *row = (system_ui_jobs_row_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !row || !row->job_id[0]) {
        return;
    }
    system_ui_jobs_start_stop_task_locked(row->job_id, false);
}

static esp_err_t system_ui_jobs_create_label(lv_obj_t *parent,
                                              const char *text,
                                              lv_color_t color,
                                              lv_obj_t **ret_label)
{
    lv_obj_t *label = lv_label_create(parent);

    ESP_RETURN_ON_FALSE(label != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create jobs label failed");
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, color, 0);
    system_ui_apply_font(label);
    *ret_label = label;
    return ESP_OK;
}

static void system_ui_jobs_clear_row_slots_locked(void)
{
    memset(s_ui.jobs_rows, 0, sizeof(s_ui.jobs_rows));
}

static esp_err_t system_ui_jobs_create_row_locked(system_ui_jobs_row_t *slot)
{
    lv_obj_t *row = lv_obj_create(s_ui.jobs_list);
    lv_obj_t *stop_button = NULL;
    int32_t short_side = system_ui_short_side_from(s_ui.width, s_ui.height);
    int32_t row_h = system_ui_clamp_i32(short_side / 4, 56, 76);
    int32_t row_pad = system_ui_clamp_i32(short_side / 40, 6, 10);
    int32_t stop_w = system_ui_clamp_i32(short_side / 5, 48, 66);
    int32_t stop_h = system_ui_clamp_i32(row_h / 2, 30, 38);

    ESP_RETURN_ON_FALSE(slot != NULL, ESP_ERR_INVALID_ARG, SYSTEM_UI_TAG, "jobs row slot missing");
    ESP_RETURN_ON_FALSE(row != NULL, ESP_ERR_NO_MEM, SYSTEM_UI_TAG, "create jobs row failed");
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, row_h);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_color(row, system_ui_color(SYSTEM_UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, system_ui_color(SYSTEM_UI_COLOR_LINE), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_all(row, row_pad, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    slot->row = row;

    ESP_RETURN_ON_ERROR(system_ui_jobs_create_label(row, "",
                                                     system_ui_color(SYSTEM_UI_COLOR_TEXT),
                                                     &slot->title),
                        SYSTEM_UI_TAG, "create jobs title failed");
    lv_obj_set_width(slot->title, LV_PCT(56));
    lv_obj_set_height(slot->title, SYSTEM_UI_DEFAULT_FONT_SIZE + 4);
    lv_label_set_long_mode(slot->title, LV_LABEL_LONG_CLIP);
    lv_obj_align(slot->title, LV_ALIGN_TOP_LEFT, 0, 0);

    ESP_RETURN_ON_ERROR(system_ui_jobs_create_label(row, "",
                                                     system_ui_color(SYSTEM_UI_COLOR_MUTED),
                                                     &slot->status),
                        SYSTEM_UI_TAG, "create jobs status failed");
    lv_obj_align(slot->status, LV_ALIGN_TOP_RIGHT, -stop_w - row_pad, row_pad / 2);

    stop_button = lv_obj_create(row);
    ESP_RETURN_ON_FALSE(stop_button != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create jobs stop failed");
    lv_obj_set_size(stop_button, stop_w, stop_h);
    lv_obj_align(stop_button, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(stop_button, 0, 0);
    lv_obj_set_style_bg_color(stop_button, system_ui_color(SYSTEM_UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(stop_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(stop_button, system_ui_color(SYSTEM_UI_COLOR_LINE), 0);
    lv_obj_set_style_border_width(stop_button, 1, 0);
    lv_obj_set_style_pad_all(stop_button, 0, 0);
    lv_obj_add_flag(stop_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(stop_button, LV_OBJ_FLAG_SCROLLABLE);
    system_ui_add_click_feedback(stop_button);
    lv_obj_add_event_cb(stop_button, system_ui_jobs_stop_event_cb, LV_EVENT_CLICKED, slot);
    slot->stop_button = stop_button;

    ESP_RETURN_ON_ERROR(system_ui_jobs_create_label(stop_button, "Stop",
                                                     system_ui_color(SYSTEM_UI_COLOR_TEXT),
                                                     &slot->stop_label),
                        SYSTEM_UI_TAG, "create jobs stop label failed");
    lv_obj_center(slot->stop_label);

    ESP_RETURN_ON_ERROR(system_ui_jobs_create_label(row, "",
                                                     system_ui_color(SYSTEM_UI_COLOR_MUTED),
                                                     &slot->detail),
                        SYSTEM_UI_TAG, "create jobs detail failed");
    lv_obj_set_width(slot->detail, LV_PCT(100));
    lv_label_set_long_mode(slot->detail, LV_LABEL_LONG_DOT);
    lv_obj_align(slot->detail, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return ESP_OK;
}

static esp_err_t system_ui_jobs_create_rows_locked(void)
{
    system_ui_jobs_clear_row_slots_locked();
    for (size_t i = 0; i < SYSTEM_UI_JOBS_MAX_ITEMS; i++) {
        ESP_RETURN_ON_ERROR(system_ui_jobs_create_row_locked(&s_ui.jobs_rows[i]),
                            SYSTEM_UI_TAG, "create jobs row %u failed", (unsigned)i);
    }
    return ESP_OK;
}

static void system_ui_jobs_update_row_locked(system_ui_jobs_row_t *slot,
                                              const system_ui_job_item_t *item)
{
    if (!slot || !slot->row || !item) {
        return;
    }

    strlcpy(slot->job_id, item->id, sizeof(slot->job_id));
    lv_label_set_text(slot->title, item->title);
    lv_label_set_text(slot->status, item->status);
    lv_label_set_text(slot->detail, item->detail);
    lv_obj_remove_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
}

static void system_ui_jobs_hide_row_locked(system_ui_jobs_row_t *slot)
{
    if (!slot || !slot->row) {
        return;
    }
    slot->job_id[0] = '\0';
    lv_obj_add_flag(slot->row, LV_OBJ_FLAG_HIDDEN);
}

static void system_ui_jobs_refresh_locked(void)
{
    if (!s_ui.jobs_root || !s_ui.jobs_list ||
            !lv_obj_is_valid(s_ui.jobs_root) || !lv_obj_is_valid(s_ui.jobs_list)) {
        s_ui.jobs_visible = false;
        return;
    }
    if (s_ui.jobs_empty_label) {
        if (!s_ui.jobs_snapshot_valid || s_ui.jobs_item_count == 0) {
            lv_obj_remove_flag(s_ui.jobs_empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.jobs_empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t i = 0; i < SYSTEM_UI_JOBS_MAX_ITEMS; i++) {
        if (i < s_ui.jobs_item_count) {
            system_ui_jobs_update_row_locked(&s_ui.jobs_rows[i], &s_ui.jobs_items[i]);
        } else {
            system_ui_jobs_hide_row_locked(&s_ui.jobs_rows[i]);
        }
    }
}

void system_ui_jobs_refresh_snapshot_locked(const system_ui_job_item_t *items, size_t count)
{
    if (count > SYSTEM_UI_JOBS_MAX_ITEMS) {
        count = SYSTEM_UI_JOBS_MAX_ITEMS;
    }
    memset(s_ui.jobs_items, 0, sizeof(s_ui.jobs_items));
    if (items != NULL && count > 0) {
        memcpy(s_ui.jobs_items, items, count * sizeof(s_ui.jobs_items[0]));
    }
    s_ui.jobs_item_count = count;
    s_ui.jobs_snapshot_valid = true;

    if (s_ui.jobs_visible) {
        system_ui_jobs_refresh_locked();
    }
}

void system_ui_jobs_set_visible_locked(bool visible)
{
    if (!s_ui.jobs_root || !lv_obj_is_valid(s_ui.jobs_root)) {
        s_ui.jobs_visible = false;
        return;
    }
    if (visible && !system_ui_system_overlay_allowed()) {
        visible = false;
    }
    s_ui.jobs_visible = visible;
    if (visible) {
        system_ui_jobs_refresh_locked();
        lv_obj_remove_flag(s_ui.jobs_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.jobs_root);
        system_ui_dummy_draw_suspend_for_overlay_locked();
    } else {
        lv_obj_add_flag(s_ui.jobs_root, LV_OBJ_FLAG_HIDDEN);
        system_ui_dummy_draw_resume_if_no_overlay_locked();
    }
}

esp_err_t system_ui_create_jobs_locked(void)
{
    int32_t width = (int32_t)s_ui.width;
    int32_t height = (int32_t)s_ui.height;
    int32_t short_side = system_ui_min_i32(width, height);
    int32_t side_pad = system_ui_clamp_i32(short_side / 28, 8, 18);
    int32_t panel_pad = system_ui_clamp_i32(short_side / 24, 10, 18);
    int32_t panel_w = system_ui_min_i32(width - side_pad * 2, 520);
    int32_t panel_h_max = system_ui_min_i32(height - side_pad * 2, 340);
    int32_t panel_h = system_ui_clamp_i32(height * 58 / 100, 150, panel_h_max);
    int32_t header_h = system_ui_clamp_i32(short_side / 8, 34, 46);
    int32_t content_w = system_ui_max_i32(96, panel_w - panel_pad * 2);
    int32_t list_h = system_ui_max_i32(60, panel_h - header_h - panel_pad * 3);
    int32_t stop_all_w = system_ui_clamp_i32(short_side / 3, 86, 112);
    int32_t stop_all_h = system_ui_clamp_i32(header_h - 4, 32, 40);
    lv_obj_t *title = NULL;

    s_ui.jobs_root = NULL;
    s_ui.jobs_panel = NULL;
    s_ui.jobs_list = NULL;
    s_ui.jobs_empty_label = NULL;
    s_ui.jobs_stop_all_button = NULL;
    s_ui.jobs_stop_all_label = NULL;
    s_ui.jobs_stop_all_cb = NULL;
    s_ui.jobs_stop_all_user_ctx = NULL;
    s_ui.jobs_stop_task_running = false;
    s_ui.jobs_refresh_running = false;
    s_ui.jobs_refresh_pending = false;
    s_ui.jobs_snapshot_valid = false;
    s_ui.jobs_item_count = 0;
    s_ui.jobs_visible = false;
    system_ui_jobs_clear_row_slots_locked();

    s_ui.jobs_root = lv_obj_create(lv_layer_top());
    ESP_RETURN_ON_FALSE(s_ui.jobs_root != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create jobs overlay failed");
    lv_obj_set_size(s_ui.jobs_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.jobs_root, system_ui_color(SYSTEM_UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_ui.jobs_root, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_ui.jobs_root, 0, 0);
    lv_obj_set_style_radius(s_ui.jobs_root, 0, 0);
    lv_obj_set_style_pad_all(s_ui.jobs_root, 0, 0);
    lv_obj_add_flag(s_ui.jobs_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ui.jobs_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_ui.jobs_root, system_ui_jobs_close_event_cb, LV_EVENT_CLICKED, NULL);
    system_ui_apply_font(s_ui.jobs_root);

    s_ui.jobs_panel = lv_obj_create(s_ui.jobs_root);
    ESP_RETURN_ON_FALSE(s_ui.jobs_panel != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create jobs panel failed");
    lv_obj_set_size(s_ui.jobs_panel, panel_w, panel_h);
    lv_obj_align(s_ui.jobs_panel, LV_ALIGN_TOP_MID, 0,
                 side_pad / 2 + SYSTEM_UI_JOBS_PANEL_Y_OFFSET_PX);
    lv_obj_set_style_radius(s_ui.jobs_panel, 0, 0);
    lv_obj_set_style_bg_color(s_ui.jobs_panel, system_ui_color(SYSTEM_UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_ui.jobs_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.jobs_panel, system_ui_color(SYSTEM_UI_COLOR_LINE), 0);
    lv_obj_set_style_border_width(s_ui.jobs_panel, 1, 0);
    lv_obj_set_style_pad_all(s_ui.jobs_panel, panel_pad, 0);
    lv_obj_add_flag(s_ui.jobs_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ui.jobs_panel, LV_OBJ_FLAG_SCROLLABLE);

    ESP_RETURN_ON_ERROR(system_ui_jobs_create_label(s_ui.jobs_panel, "Jobs",
                                                     system_ui_color(SYSTEM_UI_COLOR_TEXT), &title),
                        SYSTEM_UI_TAG, "create jobs title failed");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 2);

    s_ui.jobs_stop_all_button = lv_obj_create(s_ui.jobs_panel);
    ESP_RETURN_ON_FALSE(s_ui.jobs_stop_all_button != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create jobs stop all failed");
    lv_obj_set_size(s_ui.jobs_stop_all_button, stop_all_w, stop_all_h);
    lv_obj_align(s_ui.jobs_stop_all_button, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(s_ui.jobs_stop_all_button, 0, 0);
    lv_obj_set_style_bg_color(s_ui.jobs_stop_all_button,
                              system_ui_color(SYSTEM_UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(s_ui.jobs_stop_all_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.jobs_stop_all_button,
                                  system_ui_color(SYSTEM_UI_COLOR_LINE), 0);
    lv_obj_set_style_border_width(s_ui.jobs_stop_all_button, 1, 0);
    lv_obj_add_flag(s_ui.jobs_stop_all_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_ui.jobs_stop_all_button, LV_OBJ_FLAG_SCROLLABLE);
    system_ui_add_click_feedback(s_ui.jobs_stop_all_button);
    lv_obj_add_event_cb(s_ui.jobs_stop_all_button, system_ui_jobs_stop_all_event_cb, LV_EVENT_CLICKED, NULL);

    ESP_RETURN_ON_ERROR(system_ui_jobs_create_label(s_ui.jobs_stop_all_button, "Stop All",
                                                     system_ui_color(SYSTEM_UI_COLOR_TEXT),
                                                     &s_ui.jobs_stop_all_label),
                        SYSTEM_UI_TAG, "create jobs stop all label failed");
    lv_obj_center(s_ui.jobs_stop_all_label);

    s_ui.jobs_list = lv_obj_create(s_ui.jobs_panel);
    ESP_RETURN_ON_FALSE(s_ui.jobs_list != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create jobs list failed");
    lv_obj_set_size(s_ui.jobs_list, content_w, list_h);
    lv_obj_align(s_ui.jobs_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_ui.jobs_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.jobs_list, 0, 0);
    lv_obj_set_style_pad_all(s_ui.jobs_list, 0, 0);
    lv_obj_set_style_pad_row(s_ui.jobs_list, 6, 0);
    lv_obj_set_flex_flow(s_ui.jobs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_ui.jobs_list, LV_SCROLLBAR_MODE_AUTO);
    ESP_RETURN_ON_ERROR(system_ui_jobs_create_rows_locked(), SYSTEM_UI_TAG, "create jobs rows failed");

    ESP_RETURN_ON_ERROR(system_ui_jobs_create_label(s_ui.jobs_panel, "No running jobs",
                                                     system_ui_color(SYSTEM_UI_COLOR_MUTED),
                                                     &s_ui.jobs_empty_label),
                        SYSTEM_UI_TAG, "create jobs empty label failed");
    lv_obj_align(s_ui.jobs_empty_label, LV_ALIGN_CENTER, 0, 14);

    lv_obj_add_flag(s_ui.jobs_root, LV_OBJ_FLAG_HIDDEN);
    return ESP_OK;
}

void system_ui_delete_jobs_locked(void)
{
    if (s_ui.jobs_root) {
        lv_obj_delete(s_ui.jobs_root);
    }
    s_ui.jobs_root = NULL;
    s_ui.jobs_panel = NULL;
    s_ui.jobs_list = NULL;
    s_ui.jobs_empty_label = NULL;
    s_ui.jobs_stop_all_button = NULL;
    s_ui.jobs_stop_all_label = NULL;
    s_ui.jobs_stop_all_cb = NULL;
    s_ui.jobs_stop_all_user_ctx = NULL;
    s_ui.jobs_stop_task_running = false;
    s_ui.jobs_refresh_running = false;
    s_ui.jobs_refresh_pending = false;
    s_ui.jobs_snapshot_valid = false;
    s_ui.jobs_item_count = 0;
    s_ui.jobs_visible = false;
    system_ui_jobs_clear_row_slots_locked();
}

esp_err_t system_ui_jobs_set_provider(system_ui_jobs_provider_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.jobs_root, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "jobs overlay not ready");
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "callback lock failed");
    s_ui.jobs_provider_cb = cb;
    s_ui.jobs_provider_user_ctx = user_ctx;
    system_ui_callback_unlock();

    if (system_ui_lock() != ESP_OK) {
        ESP_LOGW(SYSTEM_UI_TAG, "jobs provider registered without immediate refresh: LVGL lock timeout");
        return ESP_OK;
    }
    if (s_ui.jobs_visible && s_ui.jobs_list && lv_obj_is_valid(s_ui.jobs_list)) {
        system_ui_jobs_request_refresh_locked();
    } else {
        s_ui.jobs_visible = false;
    }
    system_ui_unlock();
    return ESP_OK;
}

esp_err_t system_ui_jobs_set_action_callback(system_ui_job_action_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.jobs_root, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "jobs overlay not ready");
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "callback lock failed");
    s_ui.jobs_action_cb = cb;
    s_ui.jobs_action_user_ctx = user_ctx;
    system_ui_callback_unlock();
    return ESP_OK;
}

esp_err_t system_ui_jobs_set_stop_all_callback(system_ui_jobs_stop_all_cb_t cb, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.jobs_root, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "jobs overlay not ready");
    ESP_RETURN_ON_ERROR(system_ui_callback_lock(), SYSTEM_UI_TAG, "callback lock failed");
    s_ui.jobs_stop_all_cb = cb;
    s_ui.jobs_stop_all_user_ctx = user_ctx;
    system_ui_callback_unlock();
    return ESP_OK;
}

esp_err_t system_ui_jobs_request_refresh(void)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.jobs_root, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "jobs overlay not ready");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    system_ui_jobs_request_refresh_locked();
    system_ui_unlock();
    return ESP_OK;
}

esp_err_t system_ui_jobs_set_visible(bool visible)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.jobs_root, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "jobs overlay not ready");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    system_ui_jobs_set_visible_locked(visible);
    system_ui_unlock();
    return ESP_OK;
}

bool system_ui_jobs_is_visible(void)
{
    bool visible = false;

    if (system_ui_lock() == ESP_OK) {
        visible = s_ui.jobs_visible;
        system_ui_unlock();
    }
    return visible;
}
