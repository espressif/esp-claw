/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_ARBITER_OWNER_NONE = 0,
    DISPLAY_ARBITER_OWNER_LUA,
    DISPLAY_ARBITER_OWNER_EMOTE,
} display_arbiter_owner_t;

typedef void (*display_arbiter_owner_changed_cb_t)(display_arbiter_owner_t owner, void *user_ctx);

esp_err_t display_arbiter_acquire(display_arbiter_owner_t owner);
esp_err_t display_arbiter_release(display_arbiter_owner_t owner);
display_arbiter_owner_t display_arbiter_get_owner(void);
bool display_arbiter_is_owner(display_arbiter_owner_t owner);
esp_err_t display_arbiter_set_owner_changed_callback(display_arbiter_owner_changed_cb_t callback, void *user_ctx);

#ifdef __cplusplus
}
#endif
