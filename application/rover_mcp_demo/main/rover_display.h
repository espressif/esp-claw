/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "setup_device.h"

#ifdef __cplusplus
extern "C" {
#endif

void rover_display_set_state(rover_display_state_t state);
void rover_display_refresh(void);

#ifdef __cplusplus
}
#endif
