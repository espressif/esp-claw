/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

esp_err_t claw_openai_codex_store_save_refresh_token(const char *token);
esp_err_t claw_openai_codex_store_load_refresh_token(char **out_token);
esp_err_t claw_openai_codex_store_save_account_id(const char *account_id);
esp_err_t claw_openai_codex_store_load_account_id(char **out_account_id);
esp_err_t claw_openai_codex_store_erase(void);