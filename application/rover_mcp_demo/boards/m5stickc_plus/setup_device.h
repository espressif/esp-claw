/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rover_board_init(void);
void rover_board_m5_update(void);
bool rover_board_btn_a_pressed(void);
bool rover_board_btn_b_pressed(void);
int rover_board_get_battery_pct(void);
bool rover_board_imu_enabled(void);
esp_err_t rover_board_imu_read(float *ax, float *ay, float *az,
                               float *gx, float *gy, float *gz);

typedef struct {
    int battery_pct;
    bool charging;
    int battery_mv;
} rover_power_status_t;

void rover_board_get_power_status(rover_power_status_t *out);

typedef enum {
    ROVER_DISPLAY_STATE_BOOT = 0,
    ROVER_DISPLAY_STATE_IDLE,
    ROVER_DISPLAY_STATE_THINKING,
    ROVER_DISPLAY_STATE_EXECUTING,
    ROVER_DISPLAY_STATE_OFFLINE,
    ROVER_DISPLAY_STATE_SLEEPING,
} rover_display_state_t;

void rover_board_display_render(rover_display_state_t state,
                                const char *ip,
                                const rover_power_status_t *power);
void rover_board_display_sleep(void);
void rover_board_display_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
