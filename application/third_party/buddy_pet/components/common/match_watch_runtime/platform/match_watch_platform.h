/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file match_watch_platform.h
 * @brief Match Watch display session, assets, fonts, and GFX platform helpers.
 */

#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "gfx.h"
#include "display_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open Match Watch platform resources for one UI session.
 *
 * Holds the display session acquired from the arbiter.
 */
typedef struct {
    display_session_t *display_session;     /**< Display arbiter session handle. */
} match_watch_platform_session_t;

/** @deprecated Use match_watch_platform_session_t (not the component facade). */
typedef match_watch_platform_session_t match_watch_runtime_t;

/**
 * @brief Touch event callback forwarded from the display session.
 *
 * @param touch Touch device handle.
 * @param event Touch event payload.
 * @param user_data Opaque pointer registered with the callback.
 */
typedef void (*match_watch_touch_event_cb_t)(gfx_touch_t *touch, const gfx_touch_event_t *event, void *user_data);

/** @brief Element count helper for static arrays. */
#define MATCH_WATCH_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/** @brief Bogle font, 20 px, 4 bpp. */
extern const lv_font_t font_match_bogle_20_4;
/** @brief Bogle font, 24 px, 4 bpp. */
extern const lv_font_t font_match_bogle_24_4;
/** @brief Bogle font, 32 px, 4 bpp. */
extern const lv_font_t font_match_bogle_32_4;
/** @brief Bogle font, 55 px, 4 bpp. */
extern const lv_font_t font_match_bogle_55_4;
/** @brief Bogle font, 70 px, 4 bpp. */
extern const lv_font_t font_match_bogle_70_4;

/** @brief Shared GFX emote handle (synced from hw_gfx_runtime). */
extern gfx_handle_t emote_handle;
/** @brief Default display from GFX runtime init. */
extern gfx_disp_t *disp_default;
/** @brief Default touch device from GFX runtime init. */
extern gfx_touch_t *touch_default;
/** @brief LCD panel IO handle from GFX runtime. */
extern esp_lcd_panel_io_handle_t io_handle;
/** @brief LCD panel handle from GFX runtime. */
extern esp_lcd_panel_handle_t panel_handle;

/**
 * @brief Open display session.
 *
 * @param session Output session state.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t match_watch_platform_open(match_watch_platform_session_t *session);

/**
 * @brief Close display session and release loaded assets.
 *
 * @param session Session to close; may be partially initialized.
 */
void match_watch_platform_close(match_watch_platform_session_t *session);

/**
 * @brief Lock the active Match Watch display session for drawing.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t match_watch_platform_lock(void);

/**
 * @brief Unlock the active Match Watch display session.
 */
void match_watch_platform_unlock(void);

/**
 * @brief Apply platform byte-order fixup to a GFX color value.
 *
 * @param color Input color.
 * @return Color with swapped bytes when required by the panel.
 */
gfx_color_t match_watch_platform_color(gfx_color_t color);


/**
 * @brief Register touch event callback for Match Watch UI input.
 *
 * @param cb Callback invoked on touch events, or NULL to clear.
 * @param user_data Opaque pointer passed to the callback.
 */
void match_watch_platform_set_touch_event_cb(match_watch_touch_event_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
