/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_s3_display.h"
#include "rover_s3_wifi.h"
#include "setup_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static volatile rover_s3_display_state_t s_state = ROVER_S3_DISPLAY_BOOT;
static SemaphoreHandle_t s_render_mutex;

static rover_s3_display_state_t validate_state(rover_s3_display_state_t state)
{
    if (state < ROVER_S3_DISPLAY_BOOT || state > ROVER_S3_DISPLAY_OFFLINE) {
        return ROVER_S3_DISPLAY_BOOT;
    }
    return state;
}

void rover_s3_display_init(void)
{
    if (!s_render_mutex) {
        s_render_mutex = xSemaphoreCreateMutex();
    }
}

void rover_s3_display_set_state(rover_s3_display_state_t state)
{
    s_state = state;
    rover_s3_display_refresh();
}

void rover_s3_display_refresh(void)
{
    if (!s_render_mutex) {
        return;
    }
    rover_s3_display_state_t state = validate_state(s_state);
    char ip[20] = {0};
    const char *wifi_ip = rover_s3_wifi_get_ip();
    if (wifi_ip) {
        strlcpy(ip, wifi_ip, sizeof(ip));
    }
    int batt_pct = rover_s3_board_get_battery_pct();

    if (xSemaphoreTake(s_render_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        rover_s3_board_display_state(state, ip, batt_pct);
        xSemaphoreGive(s_render_mutex);
    }
}
