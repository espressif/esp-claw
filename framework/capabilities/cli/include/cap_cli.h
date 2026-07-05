/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAP_CLI_NAME "run_cli_command"

typedef struct {
    size_t max_commands;
    size_t max_output_bytes;
} cap_cli_config_t;

typedef struct {
    const char *command_name;
    const char *description;
    const char *usage_hint;
} cap_cli_command_t;

esp_err_t cap_cli_init(const cap_cli_config_t *config);
esp_err_t cap_cli_register_command(const cap_cli_command_t *command);
esp_err_t cap_cli_register_group(void);

#ifdef __cplusplus
}
#endif
