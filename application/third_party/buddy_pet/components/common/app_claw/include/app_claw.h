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

typedef struct claw_core_state *claw_core_handle_t;

typedef struct app_claw_config_t app_claw_config_t;
typedef struct app_claw_storage_paths_t app_claw_storage_paths_t;

typedef esp_err_t (*app_claw_extra_capabilities_cb_t)(const app_claw_config_t *config,
                                                      const app_claw_storage_paths_t *paths);
typedef esp_err_t (*app_claw_extra_lua_modules_cb_t)(const app_claw_config_t *config,
                                                     const app_claw_storage_paths_t *paths);

struct app_claw_config_t {
    char llm_api_key[APP_CLAW_STR_LEN];
    char llm_backend_type[APP_CLAW_SHORT_STR_LEN];
    char llm_model[APP_CLAW_MODEL_LEN];
    char llm_base_url[APP_CLAW_STR_LEN];
    char llm_auth_type[APP_CLAW_SHORT_STR_LEN];
    char llm_timeout_ms[APP_CLAW_TIMEOUT_LEN];
    char llm_max_tokens[APP_CLAW_TIMEOUT_LEN];
    char llm_default_image_max_bytes[APP_CLAW_TIMEOUT_LEN];
    char llm_max_tokens_field[APP_CLAW_SHORT_STR_LEN];
    char llm_supports_tools[8];
    char llm_supports_vision[8];
    char llm_image_remote_url_only[8];
    char qq_app_id[APP_CLAW_SHORT_STR_LEN];
    char qq_app_secret[APP_CLAW_STR_LEN];
    char qq_msg_type[8];
    char feishu_app_id[APP_CLAW_MODEL_LEN];
    char feishu_app_secret[APP_CLAW_STR_LEN];
    char tg_bot_token[APP_CLAW_STR_LEN];
    char wechat_token[APP_CLAW_STR_LEN];
    char wechat_base_url[APP_CLAW_STR_LEN];
    char wechat_cdn_base_url[APP_CLAW_STR_LEN];
    char wechat_account_id[APP_CLAW_SHORT_STR_LEN];
    char search_brave_key[APP_CLAW_STR_LEN];
    char search_tavily_key[APP_CLAW_STR_LEN];
    char search_http_allowlist[APP_CLAW_STR_LEN];
    char enabled_cap_groups[APP_CLAW_STR_LEN];
    char llm_visible_cap_groups[APP_CLAW_STR_LEN];
    char enabled_lua_modules[APP_CLAW_STR_LEN];
    app_claw_extra_lua_modules_cb_t extra_lua_modules_cb;
    app_claw_extra_capabilities_cb_t extra_capabilities_cb;
    bool suppress_startup_event;
};

typedef esp_err_t (*app_claw_save_config_fn)(const app_claw_config_t *config,
                                             void *user_ctx);

esp_err_t app_claw_set_save_config_callback(app_claw_save_config_fn save_config,
                                            void *user_ctx);
esp_err_t app_claw_start(const app_claw_config_t *config);
esp_err_t app_claw_start_with_paths(const app_claw_config_t *config,
                                    const app_claw_storage_paths_t *paths);
esp_err_t app_claw_update_config(const app_claw_config_t *config);
esp_err_t app_claw_get_config(app_claw_config_t *out_config);
esp_err_t app_claw_apply_config(const app_claw_config_t *config);
claw_core_handle_t app_claw_get_core(void);

#ifdef __cplusplus
}
#endif
