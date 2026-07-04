/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct claw_capability_registry claw_capability_registry_t;

/* Register the `web_search` console command. `registry` is the claw-cabi
 * capability registry the command dispatches against; it must outlive the
 * console. */
void register_cap_web_search(claw_capability_registry_t *registry);

#ifdef __cplusplus
}
#endif
