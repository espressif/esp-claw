/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CLAW_STR_LEN              320
#define APP_CLAW_SHORT_STR_LEN        32
#define APP_CLAW_MODEL_LEN            64
#define APP_CLAW_TIMEOUT_LEN          16
#define APP_CLAW_PATH_LEN             64
#define APP_CLAW_FILE_PATH_LEN        96

typedef struct {
    char llm_api_key[APP_CLAW_STR_LEN];
    char llm_backend_type[APP_CLAW_SHORT_STR_LEN];
    char llm_profile[APP_CLAW_SHORT_STR_LEN];
    char llm_model[APP_CLAW_MODEL_LEN];
    char llm_base_url[APP_CLAW_STR_LEN];
    char llm_auth_type[APP_CLAW_SHORT_STR_LEN];
    char llm_timeout_ms[APP_CLAW_TIMEOUT_LEN];
    char llm_max_tokens[APP_CLAW_TIMEOUT_LEN];
    char qq_app_id[APP_CLAW_SHORT_STR_LEN];
    char qq_app_secret[APP_CLAW_STR_LEN];
    char feishu_app_id[APP_CLAW_MODEL_LEN];
    char feishu_app_secret[APP_CLAW_STR_LEN];
    char tg_bot_token[APP_CLAW_STR_LEN];
    char wechat_token[APP_CLAW_STR_LEN];
    char wechat_base_url[APP_CLAW_STR_LEN];
    char wechat_cdn_base_url[APP_CLAW_STR_LEN];
    char wechat_account_id[APP_CLAW_SHORT_STR_LEN];
    char search_brave_key[APP_CLAW_STR_LEN];
    char search_tavily_key[APP_CLAW_STR_LEN];
    char enabled_cap_groups[APP_CLAW_STR_LEN];
    char llm_visible_cap_groups[APP_CLAW_STR_LEN];
    char enabled_lua_modules[APP_CLAW_STR_LEN];
} app_claw_config_t;

typedef struct {
    char fatfs_base_path[APP_CLAW_PATH_LEN];
    char memory_session_root[APP_CLAW_PATH_LEN];
    char memory_root_dir[APP_CLAW_PATH_LEN];
    char skills_root_dir[APP_CLAW_PATH_LEN];
    char lua_root_dir[APP_CLAW_PATH_LEN];
    char router_rules_path[APP_CLAW_FILE_PATH_LEN];
    char scheduler_rules_path[APP_CLAW_FILE_PATH_LEN];
    char im_attachment_root[APP_CLAW_PATH_LEN];
} app_claw_storage_paths_t;

esp_err_t app_claw_start(const app_claw_config_t *config,
                         const app_claw_storage_paths_t *paths);
esp_err_t app_claw_ui_start(void);
esp_err_t app_claw_set_network_status(bool sta_connected, const char *ap_ssid);

#ifdef __cplusplus
}
#endif
