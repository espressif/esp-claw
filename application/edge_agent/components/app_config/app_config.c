/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_config.h"

#include <stddef.h>
#include <string.h>

#include "settings_store.h"

typedef struct {
    const char *key;
    const char *default_value;
    size_t offset;
    size_t size;
} app_config_field_t;

#define APP_CONFIG_FIELD(member, nvs_key, default_literal) \
    { nvs_key, default_literal, offsetof(app_config_t, member), sizeof(((app_config_t *)0)->member) }

#define APP_DEFAULT_LLM_API_KEY           ""
#define APP_DEFAULT_LLM_BACKEND_TYPE      ""
#define APP_DEFAULT_LLM_PROFILE           ""
#define APP_DEFAULT_LLM_MODEL             ""
#define APP_DEFAULT_LLM_BASE_URL          ""
#define APP_DEFAULT_LLM_AUTH_TYPE         ""
#define APP_DEFAULT_LLM_TIMEOUT_MS        "120000"
#define APP_DEFAULT_LLM_MAX_TOKENS        "8192"
#define APP_DEFAULT_QQ_APP_ID             ""
#define APP_DEFAULT_QQ_APP_SECRET         ""
#define APP_DEFAULT_FEISHU_APP_ID         ""
#define APP_DEFAULT_FEISHU_APP_SECRET     ""
#define APP_DEFAULT_TG_BOT_TOKEN          ""
#define APP_DEFAULT_WECHAT_TOKEN          ""
#define APP_DEFAULT_WECHAT_BASE_URL       "https://ilinkai.weixin.qq.com"
#define APP_DEFAULT_WECHAT_CDN_BASE_URL   "https://novac2c.cdn.weixin.qq.com/c2c"
#define APP_DEFAULT_WECHAT_ACCOUNT_ID     "default"
#define APP_DEFAULT_SEARCH_BRAVE_KEY      ""
#define APP_DEFAULT_SEARCH_TAVILY_KEY     ""
#define APP_DEFAULT_ENABLED_CAP_GROUPS    ""
#define APP_DEFAULT_LLM_VISIBLE_CAP_GROUPS ""
#define APP_DEFAULT_ENABLED_LUA_MODULES   ""
#define APP_DEFAULT_TIME_TIMEZONE         "CST-8"

static const app_config_field_t s_fields[] = {
    APP_CONFIG_FIELD(wifi_ssid, "wifi_ssid", APP_WIFI_SSID),
    APP_CONFIG_FIELD(wifi_password, "wifi_password", APP_WIFI_PASSWORD),
    APP_CONFIG_FIELD(llm_api_key, "llm_api_key", APP_DEFAULT_LLM_API_KEY),
    APP_CONFIG_FIELD(llm_backend_type, "llm_backend", APP_DEFAULT_LLM_BACKEND_TYPE),
    APP_CONFIG_FIELD(llm_profile, "llm_profile", APP_DEFAULT_LLM_PROFILE),
    APP_CONFIG_FIELD(llm_model, "llm_model", APP_DEFAULT_LLM_MODEL),
    APP_CONFIG_FIELD(llm_base_url, "llm_base_url", APP_DEFAULT_LLM_BASE_URL),
    APP_CONFIG_FIELD(llm_auth_type, "llm_auth_type", APP_DEFAULT_LLM_AUTH_TYPE),
    APP_CONFIG_FIELD(llm_timeout_ms, "llm_timeout_ms", APP_DEFAULT_LLM_TIMEOUT_MS),
    APP_CONFIG_FIELD(llm_max_tokens, "llm_max_tokens", APP_DEFAULT_LLM_MAX_TOKENS),
    APP_CONFIG_FIELD(qq_app_id, "qq_app_id", APP_DEFAULT_QQ_APP_ID),
    APP_CONFIG_FIELD(qq_app_secret, "qq_app_secret", APP_DEFAULT_QQ_APP_SECRET),
    APP_CONFIG_FIELD(feishu_app_id, "feishu_app_id", APP_DEFAULT_FEISHU_APP_ID),
    APP_CONFIG_FIELD(feishu_app_secret, "feishu_secret", APP_DEFAULT_FEISHU_APP_SECRET),
    APP_CONFIG_FIELD(tg_bot_token, "tg_bot_token", APP_DEFAULT_TG_BOT_TOKEN),
    APP_CONFIG_FIELD(wechat_token, "wechat_token", APP_DEFAULT_WECHAT_TOKEN),
    APP_CONFIG_FIELD(wechat_base_url, "wechat_base_url", APP_DEFAULT_WECHAT_BASE_URL),
    APP_CONFIG_FIELD(wechat_cdn_base_url, "wechat_cdn_url", APP_DEFAULT_WECHAT_CDN_BASE_URL),
    APP_CONFIG_FIELD(wechat_account_id, "wechat_acct_id", APP_DEFAULT_WECHAT_ACCOUNT_ID),
    APP_CONFIG_FIELD(search_brave_key, "brave_key", APP_DEFAULT_SEARCH_BRAVE_KEY),
    APP_CONFIG_FIELD(search_tavily_key, "tavily_key", APP_DEFAULT_SEARCH_TAVILY_KEY),
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

static void app_config_apply_legacy_llm_profile(app_config_t *config)
{
    char legacy_provider[32] = {0};

    if (!config) {
        return;
    }

    if (config->llm_profile[0] != '\0') {
        return;
    }

    if (settings_store_get_string("llm_provider",
                                  legacy_provider,
                                  sizeof(legacy_provider),
                                  "") != ESP_OK ||
        legacy_provider[0] == '\0') {
        return;
    }

    if (strcmp(legacy_provider, "qwen") == 0) {
        strlcpy(config->llm_profile, "qwen_compatible", sizeof(config->llm_profile));
    } else if (strcmp(legacy_provider, "deepseek") == 0) {
        strlcpy(config->llm_profile, "custom_openai_compatible", sizeof(config->llm_profile));
    } else if (strcmp(legacy_provider, "openai") == 0) {
        strlcpy(config->llm_profile, "openai", sizeof(config->llm_profile));
    }
}

esp_err_t app_config_init(void)
{
    return settings_store_init(&(settings_store_config_t) {
        .namespace_name = "app",
    });
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

    app_config_apply_legacy_llm_profile(config);
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

void app_config_to_claw(const app_config_t *config, app_claw_config_t *out)
{
    if (!config || !out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    strlcpy(out->llm_api_key, config->llm_api_key, sizeof(out->llm_api_key));
    strlcpy(out->llm_backend_type, config->llm_backend_type, sizeof(out->llm_backend_type));
    strlcpy(out->llm_profile, config->llm_profile, sizeof(out->llm_profile));
    strlcpy(out->llm_model, config->llm_model, sizeof(out->llm_model));
    strlcpy(out->llm_base_url, config->llm_base_url, sizeof(out->llm_base_url));
    strlcpy(out->llm_auth_type, config->llm_auth_type, sizeof(out->llm_auth_type));
    strlcpy(out->llm_timeout_ms, config->llm_timeout_ms, sizeof(out->llm_timeout_ms));
    strlcpy(out->llm_max_tokens, config->llm_max_tokens, sizeof(out->llm_max_tokens));
    strlcpy(out->qq_app_id, config->qq_app_id, sizeof(out->qq_app_id));
    strlcpy(out->qq_app_secret, config->qq_app_secret, sizeof(out->qq_app_secret));
    strlcpy(out->feishu_app_id, config->feishu_app_id, sizeof(out->feishu_app_id));
    strlcpy(out->feishu_app_secret, config->feishu_app_secret, sizeof(out->feishu_app_secret));
    strlcpy(out->tg_bot_token, config->tg_bot_token, sizeof(out->tg_bot_token));
    strlcpy(out->wechat_token, config->wechat_token, sizeof(out->wechat_token));
    strlcpy(out->wechat_base_url, config->wechat_base_url, sizeof(out->wechat_base_url));
    strlcpy(out->wechat_cdn_base_url, config->wechat_cdn_base_url, sizeof(out->wechat_cdn_base_url));
    strlcpy(out->wechat_account_id, config->wechat_account_id, sizeof(out->wechat_account_id));
    strlcpy(out->search_brave_key, config->search_brave_key, sizeof(out->search_brave_key));
    strlcpy(out->search_tavily_key, config->search_tavily_key, sizeof(out->search_tavily_key));
    strlcpy(out->enabled_cap_groups, config->enabled_cap_groups, sizeof(out->enabled_cap_groups));
    strlcpy(out->llm_visible_cap_groups, config->llm_visible_cap_groups, sizeof(out->llm_visible_cap_groups));
    strlcpy(out->enabled_lua_modules, config->enabled_lua_modules, sizeof(out->enabled_lua_modules));
}

const char *app_config_get_timezone(const app_config_t *config)
{
    return config ? config->time_timezone : NULL;
}
