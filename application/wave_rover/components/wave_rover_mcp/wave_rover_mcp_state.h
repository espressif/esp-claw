/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WR_ROVER_STATE_IDLE = 0,
    WR_ROVER_STATE_DRIVING,
    WR_ROVER_STATE_NAV_BUSY,
    WR_ROVER_STATE_ESTOP,
} wr_rover_state_t;

/* Mark whether a blocking nav command (drive_cm / rotate_deg / nav_to) is
 * currently running. Called only from the MCP tool-dispatch task, around
 * the synchronous wr_nav_* calls. */
void wr_rover_state_set_nav_busy(bool busy);

/* Derives the current rover state from motor state, e-stop, and the
 * nav-busy flag. Safe to call from any task (e.g. the HTTP server task
 * serving /status). */
wr_rover_state_t wr_rover_state_get(void);

/* Lower-case identifier used as the JSON "state" value and as the lookup
 * key in the web UI's color map ("idle", "driving", "nav_busy", "estop"). */
const char *wr_rover_state_name(wr_rover_state_t state);

#ifdef __cplusplus
}
#endif
