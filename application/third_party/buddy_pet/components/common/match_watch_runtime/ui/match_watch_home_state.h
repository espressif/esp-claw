/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MATCH_WATCH_HOME_PHASE_FAR = 0,
    MATCH_WATCH_HOME_PHASE_COUNTDOWN,
    MATCH_WATCH_HOME_PHASE_LIVE,
    MATCH_WATCH_HOME_PHASE_FINISHED,
} match_watch_home_phase_t;

typedef struct {
    match_watch_home_phase_t phase;
    int64_t remain_s;
    int64_t elapsed_s;
} match_watch_home_timing_t;

#ifdef __cplusplus
}
#endif
