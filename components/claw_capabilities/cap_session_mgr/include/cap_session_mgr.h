/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "claw_session_mgr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef claw_session_mgr_delete_session_fn_t cap_session_mgr_delete_session_fn_t;

esp_err_t cap_session_mgr_register_group(void);
esp_err_t cap_session_mgr_set_session_root_dir(const char *session_root_dir);
esp_err_t cap_session_mgr_set_delete_session_handler(cap_session_mgr_delete_session_fn_t fn,
                                                     void *user_ctx);

#ifdef __cplusplus
}
#endif
