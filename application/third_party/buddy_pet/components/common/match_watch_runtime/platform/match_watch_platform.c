/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "match_watch_platform.h"

#include <stdbool.h>
#include <string.h>

#include "config_runtime.h"
#include "display_session.h"
#include "esp_check.h"
#include "esp_log.h"
#include "hw_gfx_runtime.h"

static const char *const TAG = "match_watch_platform";
static display_session_t *s_display_session;

/* Compatibility globals used by the current Match Watch app layer. */
gfx_handle_t emote_handle = NULL;
gfx_disp_t *disp_default = NULL;
gfx_touch_t *touch_default = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;

static void match_watch_platform_sync_gfx_handles(void)
{
    emote_handle = hw_gfx_runtime_handle();
    disp_default = hw_gfx_runtime_display();
    touch_default = hw_gfx_runtime_touch();
    io_handle = hw_gfx_runtime_panel_io();
    panel_handle = hw_gfx_runtime_panel();
}

void match_watch_platform_set_touch_event_cb(match_watch_touch_event_cb_t cb, void *user_data)
{
    if (s_display_session != NULL) {
        display_session_set_touch_event_cb(s_display_session,
                                           (hw_gfx_runtime_touch_event_cb_t)cb,
                                           user_data);
    } else {
        hw_gfx_runtime_set_touch_event_cb(DISPLAY_ARBITER_OWNER_EMOTE_GFX,
                                          (hw_gfx_runtime_touch_event_cb_t)cb,
                                          user_data);
    }
}

esp_err_t match_watch_platform_open(match_watch_platform_session_t *session)
{
    ESP_RETURN_ON_FALSE(session != NULL, ESP_ERR_INVALID_ARG, TAG, "session is NULL");

    session->display_session = NULL;
    match_watch_platform_set_touch_event_cb(NULL, NULL);
    (void)config_runtime_clear_qr();

    const display_session_config_t session_config = {0};
    ESP_RETURN_ON_ERROR(display_session_start(&session->display_session, &session_config),
                        TAG, "start display session failed");
    s_display_session = session->display_session;
    match_watch_platform_sync_gfx_handles();
    return ESP_OK;
}

void match_watch_platform_close(match_watch_platform_session_t *session)
{
    display_session_t *disp_session;

    if (session == NULL) {
        return;
    }

    match_watch_platform_set_touch_event_cb(NULL, NULL);
    disp_session = session->display_session;
    display_session_stop(&session->display_session);
    if (s_display_session == disp_session) {
        s_display_session = NULL;
    }
}

esp_err_t match_watch_platform_lock(void)
{
    return display_session_lock(s_display_session);
}

void match_watch_platform_unlock(void)
{
    display_session_unlock(s_display_session);
}

gfx_color_t match_watch_platform_color(gfx_color_t color)
{
    if (hw_gfx_runtime_should_swap_color()) {
        color.full = (uint16_t)((color.full << 8) | (color.full >> 8));
    }
    return color;
}
