/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_capabilities.h"

#include "app_lua_modules.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#if CONFIG_APP_CLAW_CAP_AGENT_MGR
#include "cap_agent_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_FILES
#include "cap_files.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
#include "cap_im_feishu.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_QQ
#include "cap_im_qq.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
#include "cap_im_tg.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
#include "cap_im_local.h"
#endif
#if CONFIG_APP_CLAW_CAP_LLM_INSPECT
#include "cap_llm_inspect.h"
#endif
#include "cap_llm_config.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "cap_lua.h"
#endif
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
#include "cap_mcp_client.h"
#endif
#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
#include "cap_router_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
#include "cap_scheduler.h"
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
#include "cap_session_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
#include "cap_skill_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_SYSTEM
#include "cap_system.h"
#endif
#if CONFIG_APP_CLAW_CAP_HTTP_REQUEST
#include "cap_http_request.h"
#endif
#if CONFIG_APP_CLAW_CAP_WEB_SEARCH
#include "cap_web_search.h"
#endif
#include "claw_cap.h"
#if CONFIG_APP_CLAW_CAP_MEMORY
#include "claw_memory.h"
#endif
#include "claw_paths.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "app_capabilities";

#define APP_IM_ATTACHMENT_MAX_BYTES (2 * 1024 * 1024)

typedef esp_err_t (*app_cap_prepare_fn)(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths);
typedef esp_err_t (*app_cap_register_fn)(const app_claw_config_t *config,
                                         const app_claw_storage_paths_t *paths);

typedef struct {
    const char *group_id;
    const char *display_name;
    const char *label;
    bool llm_visible_by_default;
    app_cap_prepare_fn prepare;
    app_cap_register_fn reg;
} app_capability_group_entry_t;

static bool app_cap_groups_config_empty(const char *value)
{
    size_t i;

    if (!value) {
        return true;
    }

    for (i = 0; value[i]; i++) {
        if (!isspace((unsigned char)value[i])) {
            return false;
        }
    }

    return true;
}

static char *app_cap_trim_token(char *token)
{
    char *end;

    if (!token) {
        return NULL;
    }

    while (*token && isspace((unsigned char)*token)) {
        token++;
    }

    end = token + strlen(token);
    while (end > token && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return token;
}

static int app_cap_find_group_entry(const app_capability_group_entry_t *entries,
                                    size_t count,
                                    const char *group_id)
{
    size_t i;

    if (!entries || !group_id || !group_id[0]) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (entries[i].group_id && strcmp(entries[i].group_id, group_id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static esp_err_t app_cap_build_group_map(const char *configured_groups,
                                         const app_capability_group_entry_t *entries,
                                         size_t entry_count,
                                         bool *selected_map,
                                         bool select_all_if_empty,
                                         bool use_llm_defaults_if_empty)
{
    char *groups_copy = NULL;
    char *saveptr = NULL;
    char *token = NULL;
    size_t i;

    if (!entries || !selected_map) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(selected_map, 0, entry_count * sizeof(selected_map[0]));

    if (app_cap_groups_config_empty(configured_groups)) {
        for (i = 0; i < entry_count; i++) {
            selected_map[i] = select_all_if_empty || (use_llm_defaults_if_empty &&
                                                      entries[i].llm_visible_by_default);
        }
        return ESP_OK;
    }

    groups_copy = malloc(strlen(configured_groups) + 1);
    if (!groups_copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(groups_copy, configured_groups, strlen(configured_groups) + 1);

    for (token = strtok_r(groups_copy, ",", &saveptr);
            token;
            token = strtok_r(NULL, ",", &saveptr)) {
        char *trimmed = app_cap_trim_token(token);
        int index;

        if (!trimmed || !trimmed[0]) {
            continue;
        }

        if (strcmp(trimmed, "__none__") == 0 || strcmp(trimmed, "none") == 0) {
            continue;
        }

        index = app_cap_find_group_entry(entries, entry_count, trimmed);
        if (index < 0) {
            ESP_LOGW(TAG, "Ignoring unknown or unavailable capability group: %s", trimmed);
            continue;
        }

        selected_map[index] = true;
    }

    free(groups_copy);
    return ESP_OK;
}

static esp_err_t app_cap_register_entry(const app_capability_group_entry_t *entry,
                                        const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    claw_cap_list_t cap_list;
    claw_cap_group_list_t group_list;
    esp_err_t err;

    if (!entry || !entry->reg) {
        return ESP_ERR_INVALID_ARG;
    }

    err = entry->reg(config, paths);
    cap_list = claw_cap_list();
    group_list = claw_cap_list_groups();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s (groups=%u, caps=%u)",
                 entry->label, esp_err_to_name(err),
                 (unsigned)group_list.count,
                 (unsigned)cap_list.count);
        return err;
    }

    ESP_LOGI(TAG, "%s ok (groups=%u, caps=%u)",
             entry->label,
             (unsigned)group_list.count,
             (unsigned)cap_list.count);
    return ESP_OK;
}

static void app_cap_copy_app_to_llm_config(const app_claw_config_t *app_config,
                                           cap_llm_config_t *llm_config)
{
    strlcpy(llm_config->api_key, app_config->llm_api_key, sizeof(llm_config->api_key));
    strlcpy(llm_config->backend_type, app_config->llm_backend_type, sizeof(llm_config->backend_type));
    strlcpy(llm_config->model, app_config->llm_model, sizeof(llm_config->model));
    strlcpy(llm_config->base_url, app_config->llm_base_url, sizeof(llm_config->base_url));
    strlcpy(llm_config->auth_type, app_config->llm_auth_type, sizeof(llm_config->auth_type));
    strlcpy(llm_config->timeout_ms, app_config->llm_timeout_ms, sizeof(llm_config->timeout_ms));
    strlcpy(llm_config->max_tokens, app_config->llm_max_tokens, sizeof(llm_config->max_tokens));
    strlcpy(llm_config->default_image_max_bytes,
            app_config->llm_default_image_max_bytes,
            sizeof(llm_config->default_image_max_bytes));
    strlcpy(llm_config->max_tokens_field,
            app_config->llm_max_tokens_field,
            sizeof(llm_config->max_tokens_field));
    strlcpy(llm_config->supports_tools, app_config->llm_supports_tools, sizeof(llm_config->supports_tools));
    strlcpy(llm_config->supports_vision, app_config->llm_supports_vision, sizeof(llm_config->supports_vision));
    strlcpy(llm_config->image_remote_url_only,
            app_config->llm_image_remote_url_only,
            sizeof(llm_config->image_remote_url_only));
}

static void app_cap_copy_llm_to_app_config(const cap_llm_config_t *llm_config,
                                           app_claw_config_t *app_config)
{
    strlcpy(app_config->llm_api_key, llm_config->api_key, sizeof(app_config->llm_api_key));
    strlcpy(app_config->llm_backend_type, llm_config->backend_type, sizeof(app_config->llm_backend_type));
    strlcpy(app_config->llm_model, llm_config->model, sizeof(app_config->llm_model));
    strlcpy(app_config->llm_base_url, llm_config->base_url, sizeof(app_config->llm_base_url));
    strlcpy(app_config->llm_auth_type, llm_config->auth_type, sizeof(app_config->llm_auth_type));
    strlcpy(app_config->llm_timeout_ms, llm_config->timeout_ms, sizeof(app_config->llm_timeout_ms));
    strlcpy(app_config->llm_max_tokens, llm_config->max_tokens, sizeof(app_config->llm_max_tokens));
    strlcpy(app_config->llm_default_image_max_bytes,
            llm_config->default_image_max_bytes,
            sizeof(app_config->llm_default_image_max_bytes));
    strlcpy(app_config->llm_max_tokens_field,
            llm_config->max_tokens_field,
            sizeof(app_config->llm_max_tokens_field));
    strlcpy(app_config->llm_supports_tools, llm_config->supports_tools, sizeof(app_config->llm_supports_tools));
    strlcpy(app_config->llm_supports_vision, llm_config->supports_vision, sizeof(app_config->llm_supports_vision));
    strlcpy(app_config->llm_image_remote_url_only,
            llm_config->image_remote_url_only,
            sizeof(app_config->llm_image_remote_url_only));
}

static esp_err_t app_cap_llm_config_get(cap_llm_config_t *out_config, void *user_ctx)
{
    app_claw_config_t *app_config = NULL;
    esp_err_t err;

    (void)user_ctx;
    if (!out_config) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config = calloc(1, sizeof(*app_config));
    if (!app_config) {
        return ESP_ERR_NO_MEM;
    }

    err = app_claw_get_config(app_config);
    if (err != ESP_OK) {
        free(app_config);
        return err;
    }

    app_cap_copy_app_to_llm_config(app_config, out_config);
    free(app_config);
    return ESP_OK;
}

static esp_err_t app_cap_llm_config_apply(const cap_llm_config_t *config, void *user_ctx)
{
    app_claw_config_t *app_config = NULL;
    esp_err_t err;

    (void)user_ctx;
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config = calloc(1, sizeof(*app_config));
    if (!app_config) {
        return ESP_ERR_NO_MEM;
    }

    err = app_claw_get_config(app_config);
    if (err != ESP_OK) {
        free(app_config);
        return err;
    }

    app_cap_copy_llm_to_app_config(config, app_config);
    err = app_claw_apply_config(app_config);
    free(app_config);
    return err;
}

static esp_err_t app_cap_register_llm_config_command(void)
{
    ESP_RETURN_ON_ERROR(cap_llm_config_set_provider(&(cap_llm_config_provider_t) {
                            .get_config = app_cap_llm_config_get,
                            .apply_config = app_cap_llm_config_apply,
                        }),
                        TAG,
                        "Failed to set LLM config provider");

    return cap_llm_config_register_group();
}

#if CONFIG_APP_CLAW_CAP_FILES
static esp_err_t app_cap_register_files(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_files_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_IM_QQ
static esp_err_t app_cap_prepare_im_qq(const app_claw_config_t *config,
                                       const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return ESP_OK;
}

static esp_err_t app_cap_register_im_qq(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(cap_im_qq_register_group(),
                        TAG,
                        "Failed to register QQ group");

    ESP_RETURN_ON_ERROR(cap_im_qq_set_attachment_config(&(cap_im_qq_attachment_config_t) {
                            .storage_root_dir = paths->im_attachment_root,
                            .max_inbound_file_bytes = APP_IM_ATTACHMENT_MAX_BYTES,
                            .enable_inbound_attachments = true,
                        }),
                        TAG, "Failed to set QQ attachment config");

    if (config->qq_app_id[0] && config->qq_app_secret[0]) {
        ESP_RETURN_ON_ERROR(cap_im_qq_set_credentials(config->qq_app_id, config->qq_app_secret),
                            TAG, "Failed to set QQ credentials");
    }

    int msg_type = 0;
    if (config->qq_msg_type[0]) {
        msg_type = atoi(config->qq_msg_type);
    }
    cap_im_qq_set_msg_type(msg_type);

    return ESP_OK;
}
#endif

#if CONFIG_APP_CLAW_CAP_IM_FEISHU
static esp_err_t app_cap_prepare_im_feishu(const app_claw_config_t *config,
                                           const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return ESP_OK;
}

static esp_err_t app_cap_register_im_feishu(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(cap_im_feishu_register_group(),
                        TAG, "Failed to register Feishu group");

    ESP_RETURN_ON_ERROR(cap_im_feishu_set_attachment_config(&(cap_im_feishu_attachment_config_t) {
                            .storage_root_dir = paths->im_attachment_root,
                            .max_inbound_file_bytes = APP_IM_ATTACHMENT_MAX_BYTES,
                            .enable_inbound_attachments = true,
                        }),
                        TAG, "Failed to set Feishu attachment config");

    if (config->feishu_app_id[0] && config->feishu_app_secret[0]) {
        ESP_RETURN_ON_ERROR(cap_im_feishu_set_credentials(config->feishu_app_id,
                                                          config->feishu_app_secret),
                            TAG, "Failed to set Feishu credentials");
    }

    return ESP_OK;
}
#endif

#if CONFIG_APP_CLAW_CAP_IM_TG
static esp_err_t app_cap_prepare_im_tg(const app_claw_config_t *config,
                                       const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return ESP_OK;
}

static esp_err_t app_cap_register_im_tg(const app_claw_config_t *config,
                                        const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(cap_im_tg_register_group(),
                        TAG, "Failed to register Telegram group");

    ESP_RETURN_ON_ERROR(cap_im_tg_set_attachment_config(&(cap_im_tg_attachment_config_t) {
                            .storage_root_dir = paths->im_attachment_root,
                            .max_inbound_file_bytes = APP_IM_ATTACHMENT_MAX_BYTES,
                            .enable_inbound_attachments = true,
                        }),
                        TAG, "Failed to set Telegram attachment config");

    if (config->tg_bot_token[0]) {
        ESP_RETURN_ON_ERROR(cap_im_tg_set_token(config->tg_bot_token),
                            TAG, "Failed to set Telegram bot token");
    }

    return ESP_OK;
}
#endif

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t app_cap_prepare_im_wechat(const app_claw_config_t *config,
                                           const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(cap_im_wechat_set_attachment_config(&(cap_im_wechat_attachment_config_t) {
                            .storage_root_dir = paths->im_attachment_root,
                            .max_inbound_file_bytes = APP_IM_ATTACHMENT_MAX_BYTES,
                            .enable_inbound_attachments = true,
                        }),
                        TAG, "Failed to set WeChat attachment config");

    if (config->wechat_token[0] && config->wechat_base_url[0]) {
        ESP_RETURN_ON_ERROR(cap_im_wechat_set_client_config(&(cap_im_wechat_client_config_t) {
                                .token = config->wechat_token,
                                .base_url = config->wechat_base_url,
                                .cdn_base_url = config->wechat_cdn_base_url,
                                .account_id = config->wechat_account_id,
                            }),
                            TAG, "Failed to set WeChat client config");
    }

    return ESP_OK;
}

static esp_err_t app_cap_register_im_wechat(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_im_wechat_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_IM_LOCAL
static esp_err_t app_cap_prepare_im_local(const app_claw_config_t *config,
                                          const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_im_local_set_config(&(cap_im_local_config_t) {
        .default_sender_id = "web_user",
        .log_outbound_messages = true,
    });
}

static esp_err_t app_cap_register_im_local(const app_claw_config_t *config,
                                           const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_im_local_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_SCHEDULER
static esp_err_t app_cap_register_scheduler(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_scheduler_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_LUA
static esp_err_t app_cap_prepare_lua(const app_claw_config_t *config,
                                     const app_claw_storage_paths_t *paths)
{
    /* Builtin Lua libraries are baked into the read-only system partition by
     * sync_lua_module_resources.py, so require() must search there — not under
     * paths->lua_root_dir (the writable data root). */
    const char *system_root = claw_paths_get(CLAW_PATH_SYSTEM);
    char path_temp[96];

    ESP_RETURN_ON_FALSE(system_root, ESP_ERR_INVALID_STATE, TAG, "CLAW_PATH_SYSTEM not configured");
    snprintf(path_temp, sizeof(path_temp), "%s/scripts/builtin", system_root);
    cap_lua_add_package_path_dir(path_temp);
    snprintf(path_temp, sizeof(path_temp), "%s/scripts/builtin/lib", system_root);
    cap_lua_add_package_path_dir(path_temp);

    return app_lua_modules_register(config, paths->fatfs_base_path);
}

static esp_err_t app_cap_register_lua(const app_claw_config_t *config,
                                      const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_lua_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
static esp_err_t app_cap_register_mcp_client(const app_claw_config_t *config,
                                             const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_mcp_client_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_SKILL_MGR
static esp_err_t app_cap_register_skill_mgr(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    (void)config;
    return cap_skill_mgr_register_group(paths ? paths->skills_root_dir : NULL);
}
#endif

#if CONFIG_APP_CLAW_CAP_SYSTEM
static esp_err_t app_cap_register_system(const app_claw_config_t *config,
                                         const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_system_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_MEMORY && CONFIG_APP_CLAW_MEMORY_MODE_FULL
static esp_err_t app_cap_register_memory(const app_claw_config_t *config,
                                         const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return claw_memory_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_LLM_INSPECT
static esp_err_t app_cap_register_llm_inspect(const app_claw_config_t *config,
                                              const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_llm_inspect_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_HTTP_REQUEST
static esp_err_t app_cap_prepare_http_request(const app_claw_config_t *config,
                                              const app_claw_storage_paths_t *paths)
{
    (void)paths;

    ESP_RETURN_ON_ERROR(cap_http_request_set_allowlist(config->search_http_allowlist),
                        TAG, "Failed to set HTTP allowlist");
    return ESP_OK;
}

static esp_err_t app_cap_register_http_request(const app_claw_config_t *config,
                                               const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_http_request_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_WEB_SEARCH
static esp_err_t app_cap_prepare_web_search(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    (void)paths;

    if (config->search_brave_key[0]) {
        ESP_RETURN_ON_ERROR(cap_web_search_set_brave_key(config->search_brave_key),
                            TAG, "Failed to set Brave search key");
    }

    if (config->search_tavily_key[0]) {
        ESP_RETURN_ON_ERROR(cap_web_search_set_tavily_key(config->search_tavily_key),
                            TAG, "Failed to set Tavily search key");
    }

    return ESP_OK;
}

static esp_err_t app_cap_register_web_search(const app_claw_config_t *config,
                                             const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_web_search_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
static esp_err_t app_cap_register_router_mgr(const app_claw_config_t *config,
                                             const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_router_mgr_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_SESSION_MGR
static esp_err_t app_cap_register_session_mgr(const app_claw_config_t *config,
                                              const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_session_mgr_register_group();
}
#endif

#if CONFIG_APP_CLAW_CAP_AGENT_MGR
static esp_err_t app_cap_register_agent_mgr(const app_claw_config_t *config,
                                            const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    return cap_agent_mgr_register_group();
}
#endif

static const app_capability_group_entry_t s_capability_group_entries[] = {
#if CONFIG_APP_CLAW_CAP_AGENT_MGR
    { "cap_agent_mgr", "Agent Manager", "Register agent manager cap", true, NULL, app_cap_register_agent_mgr },
#endif
#if CONFIG_APP_CLAW_CAP_IM_QQ
    { "cap_im_qq", "QQ", "Register QQ cap", false, app_cap_prepare_im_qq, app_cap_register_im_qq },
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
    { "cap_im_feishu", "Feishu", "Register Feishu cap", false, app_cap_prepare_im_feishu, app_cap_register_im_feishu },
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
    { "cap_im_tg", "Telegram", "Register Telegram cap", false, app_cap_prepare_im_tg, app_cap_register_im_tg },
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    { "cap_im_wechat", "WeChat", "Register WeChat cap", false, app_cap_prepare_im_wechat, app_cap_register_im_wechat },
#endif
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    { "cap_im_local", "Local IM", "Register local / Web IM cap", false, app_cap_prepare_im_local, app_cap_register_im_local },
#endif
#if CONFIG_APP_CLAW_CAP_FILES
    { "cap_files", "Files", "Register files cap", true, NULL, app_cap_register_files },
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    { "cap_scheduler", "Scheduler", "Register scheduler cap", true, NULL, app_cap_register_scheduler },
#endif
#if CONFIG_APP_CLAW_CAP_LUA
    { "cap_lua", "Lua", "Register Lua cap", true, app_cap_prepare_lua, app_cap_register_lua },
#endif
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    { "cap_mcp_client", "MCP Client", "Register MCP client cap", false, NULL, app_cap_register_mcp_client },
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    { "cap_skill", "Skill Manager", "Register skill cap", true, NULL, app_cap_register_skill_mgr },
#endif
#if CONFIG_APP_CLAW_CAP_SYSTEM
    { "cap_system", "System", "Register system cap", true, NULL, app_cap_register_system },
#endif
#if CONFIG_APP_CLAW_CAP_MEMORY && CONFIG_APP_CLAW_MEMORY_MODE_FULL
    { "claw_memory", "Memory", "Register claw_memory group", true, NULL, app_cap_register_memory },
#endif
#if CONFIG_APP_CLAW_CAP_LLM_INSPECT
    { "cap_llm_inspect", "LLM Inspect", "Register LLM inspect cap", true, NULL, app_cap_register_llm_inspect },
#endif
#if CONFIG_APP_CLAW_CAP_HTTP_REQUEST
    { "cap_http_request", "HTTP Request", "Register HTTP request cap", true, app_cap_prepare_http_request, app_cap_register_http_request },
#endif
#if CONFIG_APP_CLAW_CAP_WEB_SEARCH
    { "cap_web_search", "Web Search", "Register web search cap", true, app_cap_prepare_web_search, app_cap_register_web_search },
#endif
#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
    { "cap_router_mgr", "Router Manager", "Register router manager cap", true, NULL, app_cap_register_router_mgr },
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
    { "cap_session_mgr", "Session Manager", "Register session manager cap", false, NULL, app_cap_register_session_mgr },
#endif
};

static const app_capability_group_info_t s_capability_group_infos[] = {
#if CONFIG_APP_CLAW_CAP_AGENT_MGR
    { "cap_agent_mgr", "Agent Manager", true },
#endif
#if CONFIG_APP_CLAW_CAP_IM_QQ
    { "cap_im_qq", "QQ", false },
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
    { "cap_im_feishu", "Feishu", false },
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
    { "cap_im_tg", "Telegram", false },
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    { "cap_im_wechat", "WeChat", false },
#endif
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    { "cap_im_local", "Local IM", false },
#endif
#if CONFIG_APP_CLAW_CAP_FILES
    { "cap_files", "Files", true },
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    { "cap_scheduler", "Scheduler", false },
#endif
#if CONFIG_APP_CLAW_CAP_LUA
    { "cap_lua", "Lua", true },
#endif
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    { "cap_mcp_client", "MCP Client", false },
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    { "cap_skill", "Skill Manager", true },
#endif
#if CONFIG_APP_CLAW_CAP_SYSTEM
    { "cap_system", "System", true },
#endif
#if CONFIG_APP_CLAW_CAP_MEMORY && CONFIG_APP_CLAW_MEMORY_MODE_FULL
    { "claw_memory", "Memory", true },
#endif
#if CONFIG_APP_CLAW_CAP_LLM_INSPECT
    { "cap_llm_inspect", "LLM Inspect", false },
#endif
#if CONFIG_APP_CLAW_CAP_HTTP_REQUEST
    { "cap_http_request", "HTTP Request", false },
#endif
#if CONFIG_APP_CLAW_CAP_WEB_SEARCH
    { "cap_web_search", "Web Search", false },
#endif
#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
    { "cap_router_mgr", "Router Manager", false },
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
    { "cap_session_mgr", "Session Manager", false },
#endif
};

esp_err_t app_capabilities_init(const app_claw_config_t *config,
                                const app_claw_storage_paths_t *paths)
{
    const size_t entry_count = sizeof(s_capability_group_entries) / sizeof(s_capability_group_entries[0]);
    bool *enabled_map = NULL;
    bool *llm_visible_map = NULL;
    const char **llm_visible_groups = NULL;
    size_t llm_visible_group_count = 0;
    size_t i;
    esp_err_t ret = ESP_OK;

    if (!config || !paths) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(claw_cap_init(), TAG, "Failed to init claw_cap");
    ESP_GOTO_ON_ERROR(app_cap_register_llm_config_command(),
                      cleanup, TAG, "Failed to register LLM config command");

    enabled_map = calloc(entry_count > 0 ? entry_count : 1, sizeof(enabled_map[0]));
    llm_visible_map = calloc(entry_count > 0 ? entry_count : 1, sizeof(llm_visible_map[0]));
    llm_visible_groups = calloc(entry_count > 0 ? entry_count : 1, sizeof(llm_visible_groups[0]));
    if (!enabled_map || !llm_visible_map || !llm_visible_groups) {
        free(enabled_map);
        free(llm_visible_map);
        free(llm_visible_groups);
        return ESP_ERR_NO_MEM;
    }

    ESP_GOTO_ON_ERROR(app_cap_build_group_map(config->enabled_cap_groups,
                                              s_capability_group_entries,
                                              entry_count,
                                              enabled_map,
                                              true,
                                              false),
                      cleanup, TAG, "Failed to parse capability whitelist");
    ESP_GOTO_ON_ERROR(app_cap_build_group_map(config->llm_visible_cap_groups,
                                              s_capability_group_entries,
                                              entry_count,
                                              llm_visible_map,
                                              false,
                                              true),
                      cleanup, TAG, "Failed to parse LLM-visible capability groups");

    for (i = 0; i < entry_count; i++) {
        const app_capability_group_entry_t *entry = &s_capability_group_entries[i];

        if (!enabled_map[i]) {
            ESP_LOGI(TAG, "Skipping capability group at init: %s", entry->group_id);
            continue;
        }

        if (entry->prepare) {
            ESP_GOTO_ON_ERROR(entry->prepare(config, paths),
                              cleanup, TAG, "Failed to prepare %s", entry->group_id);
        }

        ESP_GOTO_ON_ERROR(app_cap_register_entry(entry, config, paths),
                          cleanup, TAG, "Failed to register %s", entry->group_id);

        if (llm_visible_map[i]) {
            llm_visible_groups[llm_visible_group_count++] = entry->group_id;
        }
    }

    ret = claw_cap_set_llm_visible_groups(llm_visible_groups, llm_visible_group_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LLM-visible capability groups: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = claw_cap_start_all();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start capabilities: %s", esp_err_to_name(ret));
        goto cleanup;
    }

cleanup:
    free(enabled_map);
    free(llm_visible_map);
    free(llm_visible_groups);
    return ret;
}

esp_err_t app_capabilities_get_compiled_groups(const app_capability_group_info_t **groups,
                                               size_t *count)
{
    if (!groups || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *groups = s_capability_group_infos;
    *count = sizeof(s_capability_group_infos) / sizeof(s_capability_group_infos[0]);
    return ESP_OK;
}
