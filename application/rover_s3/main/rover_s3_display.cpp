/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_s3_display.h"

#include <atomic>
#include <cstdio>
#include <mutex>

extern "C" {
#include "rover_s3_wifi.h"
#include "setup_device.h"
}

static std::atomic<rover_s3_display_state_t> s_state{ROVER_S3_DISPLAY_BOOT};
static std::mutex s_render_mutex;

static rover_s3_display_state_t validate_state(rover_s3_display_state_t state)
{
    if (state < ROVER_S3_DISPLAY_BOOT || state > ROVER_S3_DISPLAY_OFFLINE) {
        return ROVER_S3_DISPLAY_BOOT;
    }

    return state;
}

extern "C" void rover_s3_display_set_state(rover_s3_display_state_t state)
{
    s_state.store(state, std::memory_order_relaxed);
    rover_s3_display_refresh();
}

extern "C" void rover_s3_display_refresh(void)
{
    rover_s3_display_state_t state = validate_state(s_state.load(std::memory_order_relaxed));
    char ip[46] = {};
    const char *wifi_ip = rover_s3_wifi_get_ip();
    if (wifi_ip != nullptr) {
        std::snprintf(ip, sizeof(ip), "%s", wifi_ip);
    }

    int batt_pct = rover_s3_board_get_battery_pct();

    std::lock_guard<std::mutex> lock(s_render_mutex);
    rover_s3_board_display_state(state, ip, batt_pct);
}
