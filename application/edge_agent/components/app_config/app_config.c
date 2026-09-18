/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "settings_store.h"

// Increment only when stored settings become incompatible.
#define APP_CONFIG_SCHEMA_KEY      "cfg_ver"
#define APP_CONFIG_SCHEMA_VERSION  "1"

static const char *TAG = "app_config";

typedef struct {
    const char *key;
    const char *default_value;
    size_t offset;
    size_t size;
} app_config_field_t;

#define APP_CONFIG_FIELD(member, nvs_key, default_literal) \
    { nvs_key, default_literal, offsetof(app_config_t, member), sizeof(((app_config_t *)0)->member) }

#define APP_DEFAULT_LLM_API_KEY              ""
#define APP_DEFAULT_LLM_BACKEND_TYPE         ""
#define APP_DEFAULT_LLM_MODEL                ""
#define APP_DEFAULT_LLM_BASE_URL             ""
#define APP_DEFAULT_LLM_AUTH_TYPE            ""
#define APP_DEFAULT_LLM_TIMEOUT_MS           "120000"
#define APP_DEFAULT_LLM_MAX_TOKENS           "8192"
#define APP_DEFAULT_LLM_DEFAULT_IMAGE_MAX_BYTES "524288"
#define APP_DEFAULT_LLM_MAX_TOKENS_FIELD     ""
#define APP_DEFAULT_LLM_REASONING_EFFORT     "medium"
#define APP_DEFAULT_LLM_SUPPORTS_TOOLS       "false"
#define APP_DEFAULT_LLM_SUPPORTS_VISION      "false"
#define APP_DEFAULT_LLM_IMAGE_REMOTE_URL_ONLY "false"
#define APP_DEFAULT_QQ_APP_ID                ""
#define APP_DEFAULT_QQ_APP_SECRET            ""
#define APP_DEFAULT_QQ_MSG_TYPE              "0"
#define APP_DEFAULT_FEISHU_APP_ID            ""
#define APP_DEFAULT_FEISHU_APP_SECRET        ""
#define APP_DEFAULT_TG_BOT_TOKEN             ""
#define APP_DEFAULT_WECHAT_TOKEN             ""
#define APP_DEFAULT_WECHAT_BASE_URL          "https://ilinkai.weixin.qq.com"
#define APP_DEFAULT_WECHAT_CDN_BASE_URL      "https://novac2c.cdn.weixin.qq.com/c2c"
#define APP_DEFAULT_WECHAT_ACCOUNT_ID        "default"
#define APP_DEFAULT_SEARCH_BRAVE_KEY         ""
#define APP_DEFAULT_SEARCH_TAVILY_KEY        ""
#define APP_DEFAULT_ENABLED_CAP_GROUPS       ""
#define APP_DEFAULT_LLM_VISIBLE_CAP_GROUPS   ""
#define APP_DEFAULT_ENABLED_LUA_MODULES      ""
#define APP_DEFAULT_TIME_TIMEZONE            "CST-8"

static const app_config_field_t s_fields[] = {
    APP_CONFIG_FIELD(wifi_ssid, "wifi_ssid", APP_WIFI_SSID),
    APP_CONFIG_FIELD(wifi_password, "wifi_password", APP_WIFI_PASSWORD),
    APP_CONFIG_FIELD(ap_ssid, "ap_ssid", ""),
    APP_CONFIG_FIELD(ap_password, "ap_password", ""),
    APP_CONFIG_FIELD(ap_behavior, "ap_behavior", "keep"),
    APP_CONFIG_FIELD(llm_api_key, "llm_api_key", APP_DEFAULT_LLM_API_KEY),
    APP_CONFIG_FIELD(llm_backend_type, "llm_backend", APP_DEFAULT_LLM_BACKEND_TYPE),
    APP_CONFIG_FIELD(llm_model, "llm_model", APP_DEFAULT_LLM_MODEL),
    APP_CONFIG_FIELD(llm_base_url, "llm_base_url", APP_DEFAULT_LLM_BASE_URL),
    APP_CONFIG_FIELD(llm_auth_type, "llm_auth_type", APP_DEFAULT_LLM_AUTH_TYPE),
    APP_CONFIG_FIELD(llm_timeout_ms, "llm_timeout_ms", APP_DEFAULT_LLM_TIMEOUT_MS),
    APP_CONFIG_FIELD(llm_max_tokens, "llm_max_tokens", APP_DEFAULT_LLM_MAX_TOKENS),
    APP_CONFIG_FIELD(llm_default_image_max_bytes, "llm_img_max_b", APP_DEFAULT_LLM_DEFAULT_IMAGE_MAX_BYTES),
    APP_CONFIG_FIELD(llm_max_tokens_field, "llm_max_toks_f", APP_DEFAULT_LLM_MAX_TOKENS_FIELD),
    APP_CONFIG_FIELD(llm_reasoning_effort, "llm_reason_eff", APP_DEFAULT_LLM_REASONING_EFFORT),
    APP_CONFIG_FIELD(llm_supports_tools, "llm_sup_tools", APP_DEFAULT_LLM_SUPPORTS_TOOLS),
    APP_CONFIG_FIELD(llm_supports_vision, "llm_sup_vis", APP_DEFAULT_LLM_SUPPORTS_VISION),
    APP_CONFIG_FIELD(llm_image_remote_url_only, "llm_img_url_o", APP_DEFAULT_LLM_IMAGE_REMOTE_URL_ONLY),
    APP_CONFIG_FIELD(qq_app_id, "qq_app_id", APP_DEFAULT_QQ_APP_ID),
    APP_CONFIG_FIELD(qq_app_secret, "qq_app_secret", APP_DEFAULT_QQ_APP_SECRET),
    APP_CONFIG_FIELD(qq_msg_type, "qq_msg_type", APP_DEFAULT_QQ_MSG_TYPE),
    APP_CONFIG_FIELD(feishu_app_id, "feishu_app_id", APP_DEFAULT_FEISHU_APP_ID),
    APP_CONFIG_FIELD(feishu_app_secret, "feishu_secret", APP_DEFAULT_FEISHU_APP_SECRET),
    APP_CONFIG_FIELD(tg_bot_token, "tg_bot_token", APP_DEFAULT_TG_BOT_TOKEN),
    APP_CONFIG_FIELD(wechat_token, "wechat_token", APP_DEFAULT_WECHAT_TOKEN),
    APP_CONFIG_FIELD(wechat_base_url, "wechat_base_url", APP_DEFAULT_WECHAT_BASE_URL),
    APP_CONFIG_FIELD(wechat_cdn_base_url, "wechat_cdn_url", APP_DEFAULT_WECHAT_CDN_BASE_URL),
    APP_CONFIG_FIELD(wechat_account_id, "wechat_acct_id", APP_DEFAULT_WECHAT_ACCOUNT_ID),
    APP_CONFIG_FIELD(search_brave_key, "brave_key", APP_DEFAULT_SEARCH_BRAVE_KEY),
    APP_CONFIG_FIELD(search_tavily_key, "tavily_key", APP_DEFAULT_SEARCH_TAVILY_KEY),
    APP_CONFIG_FIELD(search_http_allowlist, "http_allow_ls", APP_SEARCH_HTTP_ALLOWLIST),
    APP_CONFIG_FIELD(enabled_cap_groups, "en_cap_groups", APP_DEFAULT_ENABLED_CAP_GROUPS),
    APP_CONFIG_FIELD(llm_visible_cap_groups, "vis_cap_groups", APP_DEFAULT_LLM_VISIBLE_CAP_GROUPS),
    APP_CONFIG_FIELD(enabled_lua_modules, "en_lua_mods", APP_DEFAULT_ENABLED_LUA_MODULES),
    APP_CONFIG_FIELD(time_timezone, "time_timezone", APP_DEFAULT_TIME_TIMEZONE),
};

static inline char *app_config_field_ptr(app_config_t *config, const app_config_field_t *field)
{
    return (char *)config + field->offset;
}

static inline const char *app_config_field_cptr(const app_config_t *config, const app_config_field_t *field)
{
    return (const char *)config + field->offset;
}

static bool app_config_ap_behavior_is_valid(const char *ap_behavior)
{
    return !ap_behavior || ap_behavior[0] == '\0' ||
           strcmp(ap_behavior, "keep") == 0 ||
           strcmp(ap_behavior, "close_on_sta") == 0;
}

esp_err_t app_config_init(void)
{
    char stored_version[8];

    ESP_RETURN_ON_ERROR(settings_store_init(&(settings_store_config_t) {
        .namespace_name = "app",
    }), TAG, "Failed to initialize app settings");

    esp_err_t err = settings_store_get_string(APP_CONFIG_SCHEMA_KEY, stored_version, sizeof(stored_version), "");
    if (err != ESP_OK && err != ESP_ERR_NVS_TYPE_MISMATCH && err != ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "Failed to read config schema version: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_OK && strcmp(stored_version, APP_CONFIG_SCHEMA_VERSION) == 0) {
        ESP_LOGI(TAG, "Using config schema version %s", stored_version);
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_TYPE_MISMATCH || err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Resetting app config: invalid schema version");
    } else if (stored_version[0] == '\0') {
        ESP_LOGW(TAG, "Resetting app config: schema version is missing");
    } else {
        ESP_LOGW(TAG, "Resetting app config: stored schema=%s current=%s",
                 stored_version, APP_CONFIG_SCHEMA_VERSION);
    }

    ESP_RETURN_ON_ERROR(settings_store_erase_all(), TAG, "Failed to reset app config");
    ESP_RETURN_ON_ERROR(settings_store_set_string(APP_CONFIG_SCHEMA_KEY, APP_CONFIG_SCHEMA_VERSION),
                        TAG, "Failed to save config schema version");
    return ESP_OK;
}

void app_config_load_defaults(app_config_t *config)
{
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        strlcpy(app_config_field_ptr(config, &s_fields[i]),
                s_fields[i].default_value ? s_fields[i].default_value : "",
                s_fields[i].size);
    }
}

esp_err_t app_config_load(app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_load_defaults(config);

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        esp_err_t err = settings_store_get_string(s_fields[i].key,
                                                  app_config_field_ptr(config, &s_fields[i]),
                                                  s_fields[i].size,
                                                  s_fields[i].default_value);
        if (err != ESP_OK) {
            return err;
        }
    }

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        bool exists = false;

        if (strcmp(s_fields[i].key, "http_allow_ls") != 0) {
            continue;
        }

        esp_err_t err = settings_store_has_key(s_fields[i].key, &exists);
        if (err != ESP_OK) {
            return err;
        }
        if (exists) {
            continue;
        }

        err = settings_store_set_string(s_fields[i].key,
                                        app_config_field_ptr(config, &s_fields[i]));
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(s_fields) / sizeof(s_fields[0]); ++i) {
        esp_err_t err = settings_store_set_string(s_fields[i].key,
                                                  app_config_field_cptr(config, &s_fields[i]));
        if (err != ESP_OK) {
            return err;
        }
    }

    return settings_store_commit();
}

esp_err_t app_config_validate_wifi(const app_config_t *config, const char **message)
{
    if (message) {
        *message = NULL;
    }
    if (!config) {
        if (message) {
            *message = "Missing Wi-Fi configuration";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_ssid[0] != '\0' && strlen(config->wifi_ssid) >= 32) {
        if (message) {
            *message = "wifi_ssid must be 1-31 characters";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_password[0] != '\0') {
        size_t wifi_password_len = strlen(config->wifi_password);
        if (wifi_password_len < 8 || wifi_password_len > 63) {
            if (message) {
                *message = "wifi_password must be empty or 8-63 characters";
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_password[0] != '\0') {
        size_t ap_password_len = strlen(config->ap_password);
        if (ap_password_len < 8 || ap_password_len > 63) {
            if (message) {
                *message = "ap_password must be empty or 8-63 characters";
            }
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_ssid[0] != '\0' && strlen(config->ap_ssid) > 32) {
        if (message) {
            *message = "ap_ssid must be 1-32 characters";
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_config_ap_behavior_is_valid(config->ap_behavior)) {
        if (message) {
            *message = "ap_behavior must be keep or close_on_sta";
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void app_config_to_claw(const app_config_t *config, app_claw_config_t *out)
{
    if (!config || !out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    strlcpy(out->llm_api_key, config->llm_api_key, sizeof(out->llm_api_key));
    strlcpy(out->llm_backend_type, config->llm_backend_type, sizeof(out->llm_backend_type));
    strlcpy(out->llm_model, config->llm_model, sizeof(out->llm_model));
    strlcpy(out->llm_base_url, config->llm_base_url, sizeof(out->llm_base_url));
    strlcpy(out->llm_auth_type, config->llm_auth_type, sizeof(out->llm_auth_type));
    strlcpy(out->llm_timeout_ms, config->llm_timeout_ms, sizeof(out->llm_timeout_ms));
    strlcpy(out->llm_max_tokens, config->llm_max_tokens, sizeof(out->llm_max_tokens));
    strlcpy(out->llm_default_image_max_bytes,
            config->llm_default_image_max_bytes,
            sizeof(out->llm_default_image_max_bytes));
    strlcpy(out->llm_max_tokens_field, config->llm_max_tokens_field, sizeof(out->llm_max_tokens_field));
    strlcpy(out->llm_reasoning_effort, config->llm_reasoning_effort, sizeof(out->llm_reasoning_effort));
    strlcpy(out->llm_supports_tools, config->llm_supports_tools, sizeof(out->llm_supports_tools));
    strlcpy(out->llm_supports_vision, config->llm_supports_vision, sizeof(out->llm_supports_vision));
    strlcpy(out->llm_image_remote_url_only,
            config->llm_image_remote_url_only,
            sizeof(out->llm_image_remote_url_only));
    strlcpy(out->qq_app_id, config->qq_app_id, sizeof(out->qq_app_id));
    strlcpy(out->qq_app_secret, config->qq_app_secret, sizeof(out->qq_app_secret));
    strlcpy(out->qq_msg_type, config->qq_msg_type, sizeof(out->qq_msg_type));
    strlcpy(out->feishu_app_id, config->feishu_app_id, sizeof(out->feishu_app_id));
    strlcpy(out->feishu_app_secret, config->feishu_app_secret, sizeof(out->feishu_app_secret));
    strlcpy(out->tg_bot_token, config->tg_bot_token, sizeof(out->tg_bot_token));
    strlcpy(out->wechat_token, config->wechat_token, sizeof(out->wechat_token));
    strlcpy(out->wechat_base_url, config->wechat_base_url, sizeof(out->wechat_base_url));
    strlcpy(out->wechat_cdn_base_url, config->wechat_cdn_base_url, sizeof(out->wechat_cdn_base_url));
    strlcpy(out->wechat_account_id, config->wechat_account_id, sizeof(out->wechat_account_id));
    strlcpy(out->search_brave_key, config->search_brave_key, sizeof(out->search_brave_key));
    strlcpy(out->search_tavily_key, config->search_tavily_key, sizeof(out->search_tavily_key));
    strlcpy(out->search_http_allowlist,
            config->search_http_allowlist,
            sizeof(out->search_http_allowlist));
    strlcpy(out->enabled_cap_groups, config->enabled_cap_groups, sizeof(out->enabled_cap_groups));
    strlcpy(out->llm_visible_cap_groups, config->llm_visible_cap_groups, sizeof(out->llm_visible_cap_groups));
    strlcpy(out->enabled_lua_modules, config->enabled_lua_modules, sizeof(out->enabled_lua_modules));
}

const char *app_config_get_timezone(const app_config_t *config)
{
    return config ? config->time_timezone : NULL;
}
