/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    ROVER_S3_DISPLAY_BOOT,
    ROVER_S3_DISPLAY_IDLE,
    ROVER_S3_DISPLAY_LISTENING,
    ROVER_S3_DISPLAY_THINKING,
    ROVER_S3_DISPLAY_SPEAKING,
    ROVER_S3_DISPLAY_EXECUTING,
    ROVER_S3_DISPLAY_OFFLINE,
} rover_s3_display_state_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rover_s3_board_init(void);
void rover_s3_board_update(void);
bool rover_s3_board_btn_a_pressed(void);
bool rover_s3_board_btn_b_pressed(void);
int  rover_s3_board_get_battery_pct(void);
bool rover_s3_board_is_charging(void);
void rover_s3_board_display_state(rover_s3_display_state_t state,
                                   const char *ip, int batt_pct);

#ifdef __cplusplus
}
#endif
