/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "app_claw.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_STR_LEN        320
#define APP_CONFIG_TIMEZONE_LEN   32

#define APP_WIFI_SSID             CONFIG_APP_WIFI_SSID
#define APP_WIFI_PASSWORD         CONFIG_APP_WIFI_PASSWORD

typedef struct {
    char wifi_ssid[APP_CONFIG_STR_LEN];
    char wifi_password[APP_CONFIG_STR_LEN];
    char llm_api_key[APP_CONFIG_STR_LEN];
    char llm_backend_type[32];
    char llm_profile[32];
    char llm_model[64];
    char llm_base_url[APP_CONFIG_STR_LEN];
    char llm_auth_type[32];
    char llm_timeout_ms[16];
    char llm_max_tokens[16];
    char qq_app_id[32];
    char qq_app_secret[APP_CONFIG_STR_LEN];
    char feishu_app_id[64];
    char feishu_app_secret[APP_CONFIG_STR_LEN];
    char tg_bot_token[APP_CONFIG_STR_LEN];
    char wechat_token[APP_CONFIG_STR_LEN];
    char wechat_base_url[APP_CONFIG_STR_LEN];
    char wechat_cdn_base_url[APP_CONFIG_STR_LEN];
    char wechat_account_id[32];
    char search_brave_key[APP_CONFIG_STR_LEN];
    char search_tavily_key[APP_CONFIG_STR_LEN];
    char enabled_cap_groups[APP_CONFIG_STR_LEN];
    char llm_visible_cap_groups[APP_CONFIG_STR_LEN];
    char enabled_lua_modules[APP_CONFIG_STR_LEN];
    char time_timezone[APP_CONFIG_TIMEZONE_LEN];
} app_config_t;

esp_err_t app_config_init(void);
void app_config_load_defaults(app_config_t *config);
esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
void app_config_to_claw(const app_config_t *config, app_claw_config_t *out);
const char *app_config_get_timezone(const app_config_t *config);

#ifdef __cplusplus
}
#endif
