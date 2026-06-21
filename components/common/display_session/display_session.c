/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_session.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "display_session";

struct display_session_t {
    bool active;
    hw_gfx_runtime_touch_event_cb_t touch_cb;
    void *touch_user_data;
    display_session_active_cb_t active_cb;
    void *active_user_data;
};

static void display_session_touch_event_cb(gfx_touch_t *touch,
                                           const gfx_touch_event_t *event,
                                           void *user_data)
{
    display_session_t *session = (display_session_t *)user_data;

    if (session != NULL && session->active && session->touch_cb != NULL) {
        session->touch_cb(touch, event, session->touch_user_data);
    }
}

static void display_session_set_active(display_session_t *session, bool active)
{
    if (session == NULL) {
        return;
    }

    if (session->active == active) {
        return;
    }
    ESP_LOGI(TAG, "active changed: session=%p active=%d->%d",
             session, session->active, active);
    session->active = active;
    if (session->active_cb != NULL) {
        session->active_cb(active, session->active_user_data);
    }
}

static void display_session_active_changed(bool active, void *user_data)
{
    display_session_set_active((display_session_t *)user_data, active);
}

esp_err_t display_session_start(display_session_t **out_session, const display_session_config_t *config)
{
    display_session_t *session;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(out_session != NULL && config != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    *out_session = NULL;
    session = heap_caps_calloc(1, sizeof(*session), MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(session != NULL, ESP_ERR_NO_MEM, TAG, "alloc session failed");

    session->active_cb = config->active_cb;
    session->active_user_data = config->active_user_data;
    session->touch_cb = config->touch_cb;
    session->touch_user_data = config->touch_user_data;

    ESP_LOGI(TAG, "start session: touch_cb=%p active_cb=%p",
             session->touch_cb, session->active_cb);

    ret = hw_gfx_runtime_init();
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "initialize shared gfx runtime failed");

    if (session->touch_cb != NULL) {
        hw_gfx_runtime_set_touch_event_cb(DISPLAY_ARBITER_OWNER_EMOTE_GFX,
                                          display_session_touch_event_cb, session);
    }
    hw_gfx_runtime_set_active_cb(display_session_active_changed, session);

    ret = display_arbiter_acquire(DISPLAY_ARBITER_OWNER_EMOTE_GFX);
    ESP_GOTO_ON_ERROR(ret, fail_clear_active, TAG, "acquire display owner failed");

    display_session_set_active(session, display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE_GFX));
    *out_session = session;
    return ESP_OK;

fail_clear_active:
    hw_gfx_runtime_clear_active_cb(session);
    hw_gfx_runtime_clear_touch_event_cb(DISPLAY_ARBITER_OWNER_EMOTE_GFX, session);
fail:
    ESP_LOGW(TAG, "start failed: session=%p ret=%s", session, esp_err_to_name(ret));
    free(session);
    return ret;
}

void display_session_stop(display_session_t **session_ptr)
{
    display_session_t *session;

    if (session_ptr == NULL || *session_ptr == NULL) {
        return;
    }
    session = *session_ptr;
    *session_ptr = NULL;

    ESP_LOGI(TAG, "stop session: active=%d touch_cb=%p active_cb=%p",
             session->active, session->touch_cb, session->active_cb);
    if (session->active_cb != NULL && session->active) {
        session->active_cb(false, session->active_user_data);
    }
    hw_gfx_runtime_clear_active_cb(session);
    hw_gfx_runtime_clear_touch_event_cb(DISPLAY_ARBITER_OWNER_EMOTE_GFX, session);
    (void)display_arbiter_release(DISPLAY_ARBITER_OWNER_EMOTE_GFX);
    free(session);
}

void display_session_set_touch_event_cb(display_session_t *session,
                                        hw_gfx_runtime_touch_event_cb_t cb,
                                        void *user_data)
{
    if (session == NULL) {
        return;
    }
    session->touch_cb = cb;
    session->touch_user_data = user_data;
    if (cb != NULL) {
        hw_gfx_runtime_set_touch_event_cb(DISPLAY_ARBITER_OWNER_EMOTE_GFX,
                                          display_session_touch_event_cb, session);
    } else {
        hw_gfx_runtime_clear_touch_event_cb(DISPLAY_ARBITER_OWNER_EMOTE_GFX, session);
    }
}

gfx_timer_handle_t display_session_timer_create(display_session_t *session,
                                                gfx_timer_cb_t timer_cb,
                                                uint32_t period,
                                                void *user_data)
{
    (void)session;
    return hw_gfx_runtime_timer_create(timer_cb, period, user_data);
}

esp_err_t display_session_lock(display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_lock();
}

void display_session_unlock(display_session_t *session)
{
    (void)session;
    hw_gfx_runtime_unlock();
}

bool display_session_is_active(const display_session_t *session)
{
    return session != NULL && display_arbiter_is_owner(DISPLAY_ARBITER_OWNER_EMOTE_GFX);
}

gfx_handle_t display_session_gfx_handle(const display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_handle();
}

gfx_disp_t *display_session_display(const display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_display();
}

gfx_touch_t *display_session_touch(const display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_touch();
}

esp_lcd_panel_handle_t display_session_panel(const display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_panel();
}

uint32_t display_session_width(const display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_width();
}

uint32_t display_session_height(const display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_height();
}

bool display_session_should_swap_color(const display_session_t *session)
{
    (void)session;
    return hw_gfx_runtime_should_swap_color();
}
