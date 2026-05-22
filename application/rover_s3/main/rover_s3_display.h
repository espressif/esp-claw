/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "setup_device.h"   /* for rover_s3_display_state_t */

#ifdef __cplusplus
extern "C" {
#endif

void rover_s3_display_init(void);
void rover_s3_display_set_state(rover_s3_display_state_t state);
void rover_s3_display_refresh(void);

#ifdef __cplusplus
}
#endif
