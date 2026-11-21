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

esp_err_t cap_web_search_register_group(void);
// Configure before registering the group; provider is bocha, tavily, or brave.
esp_err_t cap_web_search_set_provider(const char *provider);
esp_err_t cap_web_search_set_bocha_key(const char *api_key);
esp_err_t cap_web_search_set_brave_key(const char *api_key);
esp_err_t cap_web_search_set_tavily_key(const char *api_key);

#ifdef __cplusplus
}
#endif
