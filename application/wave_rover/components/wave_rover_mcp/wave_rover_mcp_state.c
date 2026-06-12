/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_mcp_state.h"
#include "wave_rover_hal.h"

/* Single bool flag, one writer (MCP tool-dispatch task), one reader (HTTP
 * server task via wr_rover_state_get). volatile is sufficient for a
 * single-word flag on this target — mirrors s_cached_bat_v in
 * wave_rover_mcp_web.c. */
static volatile bool s_nav_busy = false;

void wr_rover_state_set_nav_busy(bool busy)
{
    s_nav_busy = busy;
}

wr_rover_state_t wr_rover_state_get(void)
{
    if (wr_motor_emergency_stop_active()) {
        return WR_ROVER_STATE_ESTOP;
    }
    if (s_nav_busy) {
        return WR_ROVER_STATE_NAV_BUSY;
    }

    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);
    if (ms.left != 0.0f || ms.right != 0.0f) {
        return WR_ROVER_STATE_DRIVING;
    }

    return WR_ROVER_STATE_IDLE;
}

const char *wr_rover_state_name(wr_rover_state_t state)
{
    switch (state) {
    case WR_ROVER_STATE_DRIVING:  return "driving";
    case WR_ROVER_STATE_NAV_BUSY: return "nav_busy";
    case WR_ROVER_STATE_ESTOP:    return "estop";
    case WR_ROVER_STATE_IDLE:
    default:                      return "idle";
    }
}
