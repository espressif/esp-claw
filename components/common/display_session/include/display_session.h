/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "display_arbiter.h"
#include "esp_err.h"
#include "gfx.h"
#include "hw_gfx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct display_session_t display_session_t;

/**
 * @brief Called when the display session becomes active or inactive.
 *
 * A session is active while the display owner is `DISPLAY_ARBITER_OWNER_EMOTE_GFX`.
 * It becomes inactive when another owner, for example Lua, takes over the panel.
 */
typedef void (*display_session_active_cb_t)(bool active, void *user_data);

/**
 * @brief Display session startup configuration.
 *
 * `display_session_start()` acquires the emote-gfx display owner, initializes the
 * shared gfx runtime on demand, and wires optional touch/active callbacks to the
 * current session.
 */
typedef struct {
    /** Optional touch callback. It is invoked only while the session is active. */
    hw_gfx_runtime_touch_event_cb_t touch_cb;
    /** User data passed to `touch_cb`. */
    void *touch_user_data;
    /** Optional callback notified when the session active state changes. */
    display_session_active_cb_t active_cb;
    /** User data passed to `active_cb`. */
    void *active_user_data;
} display_session_config_t;

/**
 * @brief Start a display session for emote-gfx based UI.
 *
 * The session owns the display through `display_arbiter` until it is stopped.
 * When another display owner takes over, the session remains alive but inactive.
 */
esp_err_t display_session_start(display_session_t **out_session, const display_session_config_t *config);

/**
 * @brief Stop a display session and release its display ownership.
 *
 * This also clears the session touch callback and frees the session handle. The
 * input pointer is set to NULL.
 */
void display_session_stop(display_session_t **session);

/**
 * @brief Replace or clear the session touch callback.
 *
 * Passing NULL for `cb` disables touch delivery for this session.
 */
void display_session_set_touch_event_cb(display_session_t *session,
                                        hw_gfx_runtime_touch_event_cb_t cb,
                                        void *user_data);

/**
 * @brief Create a gfx timer associated with the shared gfx runtime.
 */
gfx_timer_handle_t display_session_timer_create(display_session_t *session,
                                                gfx_timer_cb_t timer_cb,
                                                uint32_t period,
                                                void *user_data);

/**
 * @brief Lock the shared gfx runtime before modifying gfx objects.
 */
esp_err_t display_session_lock(display_session_t *session);

/**
 * @brief Unlock the shared gfx runtime after modifying gfx objects.
 */
void display_session_unlock(display_session_t *session);

/**
 * @brief Return true if this session currently owns the emote-gfx display path.
 */
bool display_session_is_active(const display_session_t *session);

/** @brief Return the shared gfx handle. */
gfx_handle_t display_session_gfx_handle(const display_session_t *session);

/** @brief Return the shared gfx display object. */
gfx_disp_t *display_session_display(const display_session_t *session);

/** @brief Return the shared gfx touch object, or NULL if touch is unavailable. */
gfx_touch_t *display_session_touch(const display_session_t *session);

/** @brief Return the underlying LCD panel handle. */
esp_lcd_panel_handle_t display_session_panel(const display_session_t *session);

/** @brief Return the display width in pixels. */
uint32_t display_session_width(const display_session_t *session);

/** @brief Return the display height in pixels. */
uint32_t display_session_height(const display_session_t *session);

/**
 * @brief Return true when RGB565 color bytes should be swapped for this panel.
 */
bool display_session_should_swap_color(const display_session_t *session);

#ifdef __cplusplus
}
#endif
