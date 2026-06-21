/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "settings_store.h"

typedef struct {
    const char *key;
    const char *default_value;
    size_t offset;
    size_t size;
} app_config_field_t;

typedef struct {
    const char *legacy_id;
    const char *backend_type;
    const char *base_url;
    const char *auth_type;
    const char *max_tokens_field;
    const char *default_image_max_bytes;
    const char *supports_tools;
    const char *supports_vision;
    const char *image_remote_url_only;
} app_config_legacy_llm_preset_t;

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

// for backward compatibility, migrate from old settings to new settings
static const app_config_legacy_llm_preset_t s_legacy_llm_presets[] = {
    {
        .legacy_id = "openai",
        .backend_type = "openai_compatible",
        .base_url = "https://api.openai.com/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "false",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "qwen",
        .backend_type = "openai_compatible",
        .base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "false",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "qwen_compatible",
        .backend_type = "openai_compatible",
        .base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "false",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "deepseek",
        .backend_type = "openai_compatible",
        .base_url = "https://api.deepseek.com",
        .auth_type = "bearer",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "false",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "custom_openai_compatible",
        .backend_type = "openai_compatible",
        .base_url = "https://api.openai.com/v1",
        .auth_type = "bearer",
        .max_tokens_field = "max_completion_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "false",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "anthropic",
        .backend_type = "anthropic_compatible",
        .base_url = "https://api.anthropic.com/v1",
        .auth_type = "none",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "false",
        .image_remote_url_only = "false",
    },
    {
        .legacy_id = "claude",
        .backend_type = "anthropic_compatible",
        .base_url = "https://api.anthropic.com/v1",
        .auth_type = "none",
        .max_tokens_field = "max_tokens",
        .default_image_max_bytes = "524288",
        .supports_tools = "true",
        .supports_vision = "false",
        .image_remote_url_only = "false",
    },
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

static const app_config_legacy_llm_preset_t *app_config_find_legacy_llm_preset(const char *legacy_id)
{
    size_t i;

    if (!legacy_id || !legacy_id[0]) {
        return NULL;
    }

    for (i = 0; i < sizeof(s_legacy_llm_presets) / sizeof(s_legacy_llm_presets[0]); i++) {
        if (strcmp(s_legacy_llm_presets[i].legacy_id, legacy_id) == 0) {
            return &s_legacy_llm_presets[i];
        }
    }

    return NULL;
}

static esp_err_t app_config_write_if_empty(const char *key,
                                           char *current_value,
                                           size_t current_value_size,
                                           const char *fallback_value)
{
    if (!key || !current_value || !fallback_value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (current_value[0] != '\0') {
        return ESP_OK;
    }

    esp_err_t err = settings_store_set_string(key, fallback_value);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(current_value, fallback_value, current_value_size);
    return ESP_OK;
}

static esp_err_t app_config_replace_if_match(const char *key,
                                             char *current_value,
                                             size_t current_value_size,
                                             const char *match_value,
                                             const char *replacement_value)
{
    esp_err_t err;

    if (!key || !current_value || !match_value || !replacement_value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(current_value, match_value) != 0) {
        return ESP_OK;
    }

    err = settings_store_set_string(key, replacement_value);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(current_value, replacement_value, current_value_size);
    return ESP_OK;
}

static esp_err_t app_config_upgrade_legacy_llm_settings(void)
{
    char legacy_profile[32] = {0};
    char legacy_provider[32] = {0};
    char llm_backend_type[32] = {0};
    char llm_base_url[APP_CONFIG_STR_LEN] = {0};
    char llm_auth_type[32] = {0};
    char llm_max_tokens_field[32] = {0};
    char llm_default_image_max_bytes[16] = {0};
    char llm_supports_tools[8] = {0};
    char llm_supports_vision[8] = {0};
    char llm_image_remote_url_only[8] = {0};
    const app_config_legacy_llm_preset_t *preset = NULL;
    const char *legacy_id = NULL;
    esp_err_t err;

    err = settings_store_get_string("llm_profile", legacy_profile, sizeof(legacy_profile), "");
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_get_string("llm_provider", legacy_provider, sizeof(legacy_provider), "");
    if (err != ESP_OK) {
        return err;
    }

    legacy_id = legacy_profile[0] ? legacy_profile : legacy_provider;
    preset = app_config_find_legacy_llm_preset(legacy_id);
    if (preset) {
        err = settings_store_get_string("llm_backend", llm_backend_type, sizeof(llm_backend_type), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_base_url", llm_base_url, sizeof(llm_base_url), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_auth_type", llm_auth_type, sizeof(llm_auth_type), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_max_toks_f", llm_max_tokens_field, sizeof(llm_max_tokens_field), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_img_max_b",
                                        llm_default_image_max_bytes,
                                        sizeof(llm_default_image_max_bytes),
                                        "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_sup_tools", llm_supports_tools, sizeof(llm_supports_tools), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_sup_vis", llm_supports_vision, sizeof(llm_supports_vision), "");
        if (err != ESP_OK) {
            return err;
        }
        err = settings_store_get_string("llm_img_url_o",
                                        llm_image_remote_url_only,
                                        sizeof(llm_image_remote_url_only),
                                        "");
        if (err != ESP_OK) {
            return err;
        }

        err = app_config_write_if_empty("llm_backend",
                                        llm_backend_type,
                                        sizeof(llm_backend_type),
                                        preset->backend_type);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_base_url",
                                        llm_base_url,
                                        sizeof(llm_base_url),
                                        preset->base_url);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_auth_type",
                                        llm_auth_type,
                                        sizeof(llm_auth_type),
                                        preset->auth_type);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_max_toks_f",
                                        llm_max_tokens_field,
                                        sizeof(llm_max_tokens_field),
                                        preset->max_tokens_field);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_img_max_b",
                                        llm_default_image_max_bytes,
                                        sizeof(llm_default_image_max_bytes),
                                        preset->default_image_max_bytes);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_sup_tools",
                                        llm_supports_tools,
                                        sizeof(llm_supports_tools),
                                        preset->supports_tools);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_sup_vis",
                                        llm_supports_vision,
                                        sizeof(llm_supports_vision),
                                        preset->supports_vision);
        if (err != ESP_OK) {
            return err;
        }
        err = app_config_write_if_empty("llm_img_url_o",
                                        llm_image_remote_url_only,
                                        sizeof(llm_image_remote_url_only),
                                        preset->image_remote_url_only);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = settings_store_get_string("llm_backend", llm_backend_type, sizeof(llm_backend_type), "");
    if (err != ESP_OK) {
        return err;
    }
    err = app_config_replace_if_match("llm_backend",
                                      llm_backend_type,
                                      sizeof(llm_backend_type),
                                      "anthropic",
                                      "anthropic_compatible");
    if (err != ESP_OK) {
        return err;
    }

    err = settings_store_erase_key("llm_profile");
    if (err != ESP_OK) {
        return err;
    }
    err = settings_store_erase_key("llm_provider");
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = settings_store_init(&(settings_store_config_t) {
        .namespace_name = "app",
    });
    if (err != ESP_OK) {
        return err;
    }
    return app_config_upgrade_legacy_llm_settings();
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
