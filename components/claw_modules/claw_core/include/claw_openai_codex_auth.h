/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool active;
    bool completed;
    char status[32];
    char message[160];
    char user_code[32];
    char verification_url[160];
    uint32_t interval;
} claw_openai_codex_login_status_t;

esp_err_t claw_openai_codex_auth_restore_async(void);
esp_err_t claw_openai_codex_auth_get_session(char **out_access_token, char **out_account_id);
esp_err_t claw_openai_codex_login_start(void);
esp_err_t claw_openai_codex_login_get_status(claw_openai_codex_login_status_t *status);
esp_err_t claw_openai_codex_login_cancel(void);