/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the agent runtime as a claw_cap group so the event router and other
 * subsystems can invoke it through claw_cap_call. The descriptor dispatches to
 * the claw-cabi entry point (claw_agent_cap_execute); the agent runtime must be
 * initialized/started separately for calls to succeed. */
esp_err_t cap_agent_register_group(void);

#ifdef __cplusplus
}
#endif
