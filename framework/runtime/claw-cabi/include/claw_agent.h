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

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *persistence_dir;
} claw_agent_config_t;

typedef struct {
    const char *text;
    const char *source_cap;
    const char *source_channel;
    const char *source_chat_id;
    const char *target_channel;
    const char *target_chat_id;
} claw_agent_input_t;

esp_err_t claw_agent_init(const claw_agent_config_t *config);
esp_err_t claw_agent_start(void);
esp_err_t claw_agent_stop(void);
esp_err_t claw_agent_deinit(void);
esp_err_t claw_agent_submit(const claw_agent_input_t *input);

#ifdef __cplusplus
}
#endif
