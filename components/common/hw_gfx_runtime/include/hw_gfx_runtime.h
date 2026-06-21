/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display_arbiter.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hw_gfx_runtime_touch_event_cb_t)(gfx_touch_t *touch,
                                                const gfx_touch_event_t *event,
                                                void *user_data);
typedef void (*hw_gfx_runtime_active_cb_t)(bool active, void *user_data);

esp_err_t hw_gfx_runtime_init(void);
void hw_gfx_runtime_deinit(void);
bool hw_gfx_runtime_is_ready(void);

gfx_handle_t hw_gfx_runtime_handle(void);
gfx_disp_t *hw_gfx_runtime_display(void);
gfx_touch_t *hw_gfx_runtime_touch(void);
esp_lcd_panel_io_handle_t hw_gfx_runtime_panel_io(void);
esp_lcd_panel_handle_t hw_gfx_runtime_panel(void);
uint16_t hw_gfx_runtime_width(void);
uint16_t hw_gfx_runtime_height(void);
bool hw_gfx_runtime_should_swap_color(void);

esp_err_t hw_gfx_runtime_lock(void);
void hw_gfx_runtime_unlock(void);
gfx_timer_handle_t hw_gfx_runtime_timer_create(gfx_timer_cb_t timer_cb, uint32_t period, void *user_data);
void hw_gfx_runtime_set_touch_event_cb(display_arbiter_owner_t owner,
                                       hw_gfx_runtime_touch_event_cb_t cb,
                                       void *user_data);
void hw_gfx_runtime_clear_touch_event_cb(display_arbiter_owner_t owner, void *user_data);
void hw_gfx_runtime_set_active_cb(hw_gfx_runtime_active_cb_t cb, void *user_data);
void hw_gfx_runtime_clear_active_cb(void *user_data);

#ifdef __cplusplus
}
#endif
