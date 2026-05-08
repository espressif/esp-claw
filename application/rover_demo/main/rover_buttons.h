/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_event.h"
#include "esp_err.h"

typedef esp_err_t (*rover_buttons_event_cb)(const claw_event_t *event, void *user_ctx);
typedef void (*rover_buttons_sleep_cb)(void *user_ctx);

typedef struct {
    rover_buttons_event_cb on_btn_a_short;
    void *on_btn_a_short_ctx;
    rover_buttons_sleep_cb on_btn_a_long;
    void *on_btn_a_long_ctx;
} rover_buttons_config_t;

esp_err_t rover_buttons_init(const rover_buttons_config_t *config);
