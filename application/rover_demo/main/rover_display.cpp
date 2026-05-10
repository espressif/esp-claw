/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_display.h"

#include <atomic>

extern "C" {
#include "rover_demo_wifi.h"
#include "setup_device.h"
}

static std::atomic<rover_display_state_t> s_state{ROVER_DISPLAY_STATE_BOOT};

extern "C" void rover_display_set_state(rover_display_state_t state)
{
    s_state.store(state, std::memory_order_relaxed);
}

extern "C" void rover_display_refresh(void)
{
    rover_display_state_t state = s_state.load(std::memory_order_relaxed);
    rover_power_status_t power = {};
    rover_board_get_power_status(&power);
    rover_board_display_render(state, rover_demo_wifi_get_ip(), &power);
}
