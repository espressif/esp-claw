/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_private.h"

#include "esp_check.h"

static void system_ui_overlay_timer_cb(lv_timer_t *timer)
{
    static int32_t x = 0;
    static int32_t dir = 1;

    (void)timer;
    if (!s_ui.overlay_dot) {
        return;
    }
    x += dir * 3;
    if (x > 24) {
        x = 24;
        dir = -1;
    } else if (x < 0) {
        x = 0;
        dir = 1;
    }
    lv_obj_set_x(s_ui.overlay_dot, x);
}

esp_err_t system_ui_create_overlay_locked(void)
{
    s_ui.overlay_root = lv_obj_create(lv_layer_top());
    ESP_RETURN_ON_FALSE(s_ui.overlay_root != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create overlay failed");
    lv_obj_set_size(s_ui.overlay_root, 64, 24);
    lv_obj_align(s_ui.overlay_root, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_radius(s_ui.overlay_root, 0, 0);
    lv_obj_set_style_bg_color(s_ui.overlay_root, system_ui_color(SYSTEM_UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(s_ui.overlay_root, LV_OPA_80, 0);
    lv_obj_set_style_border_color(s_ui.overlay_root, system_ui_color(SYSTEM_UI_COLOR_LINE), 0);
    lv_obj_set_style_border_width(s_ui.overlay_root, 1, 0);
    system_ui_apply_font(s_ui.overlay_root);
    lv_obj_remove_flag(s_ui.overlay_root, LV_OBJ_FLAG_CLICKABLE);

    s_ui.overlay_dot = lv_obj_create(s_ui.overlay_root);
    ESP_RETURN_ON_FALSE(s_ui.overlay_dot != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create overlay dot failed");
    lv_obj_set_size(s_ui.overlay_dot, 10, 10);
    lv_obj_align(s_ui.overlay_dot, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(s_ui.overlay_dot, 0, 0);
    lv_obj_set_style_bg_color(s_ui.overlay_dot, system_ui_color(SYSTEM_UI_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(s_ui.overlay_dot, 0, 0);
    lv_obj_add_flag(s_ui.overlay_root, LV_OBJ_FLAG_HIDDEN);

    s_ui.overlay_timer = lv_timer_create(system_ui_overlay_timer_cb, 80, NULL);
    ESP_RETURN_ON_FALSE(s_ui.overlay_timer != NULL, ESP_ERR_NO_MEM,
                        SYSTEM_UI_TAG, "create overlay timer failed");
    lv_timer_pause(s_ui.overlay_timer);
    return ESP_OK;
}

void system_ui_delete_overlay_locked(void)
{
    if (s_ui.overlay_timer) {
        lv_timer_delete(s_ui.overlay_timer);
        s_ui.overlay_timer = NULL;
    }
    if (s_ui.overlay_root) {
        lv_obj_delete(s_ui.overlay_root);
    }
    s_ui.overlay_root = NULL;
    s_ui.overlay_dot = NULL;
}

esp_err_t system_ui_overlay_set_visible(bool visible)
{
    ESP_RETURN_ON_FALSE(s_ui.started && s_ui.overlay_root, ESP_ERR_INVALID_STATE,
                        SYSTEM_UI_TAG, "overlay not ready");
    ESP_RETURN_ON_ERROR(system_ui_lock(), SYSTEM_UI_TAG, "lock failed");
    if (visible) {
        lv_obj_remove_flag(s_ui.overlay_root, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.overlay_timer) {
            lv_timer_resume(s_ui.overlay_timer);
        }
    } else {
        lv_obj_add_flag(s_ui.overlay_root, LV_OBJ_FLAG_HIDDEN);
        if (s_ui.overlay_timer) {
            lv_timer_pause(s_ui.overlay_timer);
        }
    }
    system_ui_unlock();
    return ESP_OK;
}
