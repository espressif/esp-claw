/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"
#include "match_watch_app.h"
#include "match_watch_internal.h"

#define MATCH_WATCH_TIME_HOME_TAP_MOVE_PX       4
#define MATCH_WATCH_TIME_HOME_TAP_MIN_MS        80
#define MATCH_WATCH_TIME_HOME_TAP_MAX_MS        700

static int32_t match_watch_abs_i32(int32_t value)
{
    return value >= 0 ? value : -value;
}

static int32_t match_watch_move_i32(int32_t dx, int32_t dy)
{
    int32_t abs_dx = match_watch_abs_i32(dx);
    int32_t abs_dy = match_watch_abs_i32(dy);
    int32_t major = abs_dx > abs_dy ? abs_dx : abs_dy;
    int32_t minor = abs_dx > abs_dy ? abs_dy : abs_dx;

    return major + minor / 2;
}

void match_watch_touch_event_cb(gfx_touch_t *touch, const gfx_touch_event_t *event, void *user_data)
{
    (void)touch;
    (void)user_data;

    if (event == NULL) {
        return;
    }

    if (event->type == GFX_TOUCH_EVENT_PRESS) {
        if (s_app->touch.pet_touch_active || s_app->touch.pet_touch_suppress) {
            s_app->touch.pressed = false;
            return;
        }
        s_app->touch.pressed = true;
        s_app->touch.swipe_seen = false;
        s_app->touch.press_x = event->x;
        s_app->touch.press_y = event->y;
        s_app->touch.max_move = 0;
        s_app->touch.press_time_ms = event->timestamp_ms;
        return;
    }

    if (s_app->touch.pet_touch_active || s_app->touch.pet_touch_suppress) {
        if (event->type == GFX_TOUCH_EVENT_RELEASE) {
            s_app->touch.pet_touch_suppress = false;
            s_app->touch.pressed = false;
        }
        return;
    }

    if (event->type == GFX_TOUCH_EVENT_MOVE && s_app->touch.pressed) {
        int32_t dx = (int32_t)event->x - s_app->touch.press_x;
        int32_t dy = (int32_t)event->y - s_app->touch.press_y;
        int32_t move = match_watch_move_i32(dx, dy);
        if (move > s_app->touch.max_move) {
            s_app->touch.max_move = move;
        }
        if ((match_watch_abs_i32(dx) >= 44 && match_watch_abs_i32(dx) > match_watch_abs_i32(dy) * 2) ||
                (match_watch_abs_i32(dy) >= 44 && match_watch_abs_i32(dy) > match_watch_abs_i32(dx) * 2)) {
            s_app->touch.swipe_seen = true;
        }
        return;
    }

    if (event->type == GFX_TOUCH_EVENT_RELEASE && s_app->touch.pressed) {
        int32_t dx = (int32_t)event->x - s_app->touch.press_x;
        int32_t dy = (int32_t)event->y - s_app->touch.press_y;
        uint32_t press_ms = event->timestamp_ms - s_app->touch.press_time_ms;
        s_app->touch.pressed = false;
        if (s_app->runtime.active_page == MATCH_WATCH_PAGE_TIME_HOME) {
            if (!s_app->touch.swipe_seen &&
                    s_app->touch.max_move <= MATCH_WATCH_TIME_HOME_TAP_MOVE_PX &&
                    press_ms >= MATCH_WATCH_TIME_HOME_TAP_MIN_MS &&
                    press_ms <= MATCH_WATCH_TIME_HOME_TAP_MAX_MS) {
                match_watch_app_note_user_browse();
                match_watch_app_render_page(MATCH_WATCH_PAGE_TEAM);
            }
            return;
        }
        if (match_watch_abs_i32(dx) >= 44 && match_watch_abs_i32(dx) > match_watch_abs_i32(dy) * 2) {
            int offset = dx < 0 ? 1 : -1;
            if (match_watch_app_select_match_by_offset(offset)) {
                match_watch_app_note_user_browse();
                match_watch_app_render_page(s_app->runtime.active_page);
            }
            return;
        }
        if (match_watch_abs_i32(dy) >= 44 && match_watch_abs_i32(dy) > match_watch_abs_i32(dx) * 2) {
            int offset = dy < 0 ? 1 : -1;
            if (match_watch_app_can_select_stage_by_offset(offset)) {
                if (match_watch_app_select_stage_by_offset(offset)) {
                    match_watch_app_note_user_browse();
                    match_watch_app_render_page(s_app->runtime.active_page);
                }
            }
            return;
        }
        if (!s_app->touch.swipe_seen && s_app->touch.max_move <= MATCH_WATCH_LONG_PRESS_MOVE_PX) {
            if (s_app->runtime.active_page == MATCH_WATCH_PAGE_TEAM) {
                match_watch_app_note_user_browse();
                match_watch_app_render_page(MATCH_WATCH_PAGE_DETAIL);
            } else if (s_app->runtime.active_page == MATCH_WATCH_PAGE_DETAIL) {
                match_watch_app_note_user_browse();
                match_watch_app_render_page(MATCH_WATCH_PAGE_TEAM);
            }
            return;
        }
    }
}
