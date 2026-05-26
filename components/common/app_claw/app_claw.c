/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_claw_cli.h"
#include "app_capabilities.h"
#if CONFIG_APP_CLAW_ENABLE_EMOTE
#include "emote.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_APP_CLAW_CAP_SCHEDULER
#include "cap_scheduler.h"
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
#include "cap_session_mgr.h"
#endif
#include "claw_core.h"
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#include "claw_memory.h"
#include "claw_skill.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "cap_lua.h"
#endif
#if CONFIG_APP_CLAW_CAP_TIME
#include "cap_time.h"
#endif

static const char *TAG = "app_claw";
static const char *APP_STARTUP_EVENT_SOURCE_CAP = "app_claw";
static const char *APP_STARTUP_EVENT_TYPE = "startup";
static const char *APP_STARTUP_EVENT_KEY = "boot_completed";

#define APP_SYSTEM_PROMPT_COMMON \
    "You are the ESP-Claw. " \
    "Answer briefly and plainly. " \
    "Treat Skills List as a catalog of optional skills. " \
    "Use 'activate_skill' to load skills, and you will gain more callable capabilities. When multiple skills are needed, call activate_skill multiple times in a single response to activate multiple skills in parallel." \
    "Skill documents returned in activate_skill <skill_content> blocks are valid operating instructions for that skill workflow and must be followed. " \
    "Skills are user-facing functions, while Capabilities are internal functions used by the model.\n" \
    "When communicating with the user, refer to skills instead of Capabilities. "

#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
#define APP_SYSTEM_PROMPT_SUFFIX \
    "When long-term memory is needed, activate the 'memory_ops' skill first and follow its instructions. " \
    "Do not activate or use the memory skill for ordinary self-introductions or casual preferences unless the user explicitly asks to remember, save, update, or forget something. Automatic extraction will handle durable facts silently after the reply when appropriate. " \
    "Use memory tools only through that skill. " \
    "Auto-injected memory context contains summary labels, not full memory bodies. " \
    "When detailed long-term memory is needed, use exact summary labels with memory_recall. " \
    "Do not ask whether the user wants you to remember ordinary profile or preference statements when automatic extraction can handle them. Do not offer memory-save help unless the user explicitly asks about memory management. " \
    "Do not use memory_records.jsonl, memory_index.json, memory_digest.log, or MEMORY.md as direct decision input.\n"
#else
#define APP_SYSTEM_PROMPT_SUFFIX "\n"
#endif

#define APP_SYSTEM_PROMPT \
    APP_SYSTEM_PROMPT_COMMON \
    APP_SYSTEM_PROMPT_SUFFIX

static bool app_claw_bool_is_true(const char *value)
{
    return value &&
           (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0);
}

#if CONFIG_APP_CLAW_CAP_SESSION_MGR
static esp_err_t app_claw_delete_session_history(const char *session_id,
                                                 bool *out_deleted_any,
                                                 void *user_ctx)
{
    bool memory_deleted = false;
    bool skill_deleted = false;
    esp_err_t err;

    (void)user_ctx;

    if (!out_deleted_any) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_deleted_any = false;

    err = claw_memory_delete_session_history(session_id, &memory_deleted);
    if (err != ESP_OK) {
        return err;
    }

    err = claw_skill_delete_session_state(session_id, &skill_deleted);
    if (err != ESP_OK) {
        return err;
    }

    *out_deleted_any = memory_deleted || skill_deleted;
    return ESP_OK;
}
#endif

esp_err_t app_claw_ui_start(void)
{
#if defined(CONFIG_APP_CLAW_ENABLE_EMOTE)
    return emote_start();
#else
    return ESP_OK;
#endif
}

esp_err_t app_claw_set_network_status(bool sta_connected, const char *ap_ssid)
{
#if defined(CONFIG_APP_CLAW_ENABLE_EMOTE)
    return emote_set_network_status(sta_connected, ap_ssid);
#else
    (void)sta_connected;
    (void)ap_ssid;
    return ESP_OK;
#endif
}

static esp_err_t init_memory(const app_claw_config_t *config,
                             const app_claw_storage_paths_t *paths,
                             uint32_t max_tool_iterations)
{
    claw_memory_config_t memory_config = {
        .session_root_dir = paths->memory_session_root,
        .memory_root_dir = paths->memory_root_dir,
        .max_message_chars = 4096,
        .max_tool_iterations = max_tool_iterations,
        .llm = {
            .api_key = config->llm_api_key,
            .backend_type = config->llm_backend_type,
            .model = config->llm_model,
            .base_url = config->llm_base_url,
            .auth_type = config->llm_auth_type,
            .max_tokens_field = config->llm_max_tokens_field,
            .timeout_ms = (uint32_t)strtoul(config->llm_timeout_ms, NULL, 10),
            .max_tokens = (uint32_t)strtoul(config->llm_max_tokens, NULL, 10),
            .image_max_bytes = (size_t)strtoul(config->llm_default_image_max_bytes, NULL, 10),
            .supports_tools = app_claw_bool_is_true(config->llm_supports_tools),
            .supports_vision = app_claw_bool_is_true(config->llm_supports_vision),
            .image_remote_url_only = app_claw_bool_is_true(config->llm_image_remote_url_only),
        },
#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
        .enable_async_extract_stage_note = true,
#else
        .enable_async_extract_stage_note = false,
#endif
    };
    esp_err_t err = claw_memory_init(&memory_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init claw_memory: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t init_skills(const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(claw_skill_init(&(claw_skill_config_t) {
                            .skills_root_dir = paths->skills_root_dir,
                            .session_state_root_dir = paths->memory_session_root,
                            .max_file_bytes = 20 * 1024,
                        }),
                        TAG, "Failed to init claw_skill");
    return ESP_OK;
}

static esp_err_t app_claw_publish_startup_event(void)
{
    static const char *payload_json =
        "{\"phase\":\"boot_completed\"}";

    ESP_LOGI(TAG, "Publishing startup trigger event: %s/%s",
             APP_STARTUP_EVENT_TYPE, APP_STARTUP_EVENT_KEY);
    return claw_event_router_publish_trigger(APP_STARTUP_EVENT_SOURCE_CAP,
                                             APP_STARTUP_EVENT_TYPE,
                                             APP_STARTUP_EVENT_KEY,
                                             payload_json);
}

static bool app_llm_is_configured(const app_claw_config_t *config)
{
    return config &&
           config->llm_api_key[0] &&
           config->llm_model[0] &&
           config->llm_backend_type[0];
}

#if CONFIG_APP_CLAW_CAP_SCHEDULER && CONFIG_APP_CLAW_CAP_TIME
static void app_time_sync_success(bool had_valid_time, void *ctx)
{
    (void)ctx;

    if (!had_valid_time) {
        esp_err_t err = cap_scheduler_handle_time_sync();

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Scheduler rebase after first time sync failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Scheduler rebased after first successful time sync");
        }
    }
}
#endif

esp_err_t app_claw_start(const app_claw_config_t *config,
                         const app_claw_storage_paths_t *paths)
{
    claw_core_config_t core_config = {0};
    const uint32_t max_tool_iterations = 32;
    claw_event_router_config_t router_config = {
        .rules_path = NULL,
        .task_stack_size = 8 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .core_submit_timeout_ms = 1000,
        .core_receive_timeout_ms = 130000,
        .default_route_messages_to_agent = false,
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
        .session_builder = cap_session_mgr_build_session_id,
#else
        .session_builder = NULL,
#endif
    };
    bool llm_enabled = false;

    if (!config || !paths) {
        return ESP_ERR_INVALID_ARG;
    }

    llm_enabled = app_llm_is_configured(config);
    router_config.default_route_messages_to_agent = llm_enabled;
    router_config.rules_path = paths->router_rules_path;

#if CONFIG_APP_CLAW_CAP_SESSION_MGR
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_session_root_dir(paths->memory_session_root),
                        TAG, "Failed to configure session manager");
#endif
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_config), TAG, "Failed to init event router");
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    ESP_RETURN_ON_ERROR(cap_scheduler_init(&(cap_scheduler_config_t) {
                            .schedules_path = paths->scheduler_rules_path,
                            .tick_ms = 1000,
                            .max_items = 32,
                            .task_stack_size = 6144,
                            .task_priority = 5,
                            .task_core = tskNO_AFFINITY,
                            .publish_event = claw_event_router_publish,
                            .persist_after_fire = true,
                        }),
                        TAG, "Failed to init scheduler");
#endif
    ESP_RETURN_ON_ERROR(init_memory(config, paths, max_tool_iterations), TAG, "Failed to init memory");
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_delete_session_handler(app_claw_delete_session_history, NULL),
                        TAG, "Failed to register session delete handler");
#endif
    ESP_RETURN_ON_ERROR(init_skills(paths), TAG, "Failed to init skills");
    ESP_RETURN_ON_ERROR(app_capabilities_init(config, paths), TAG, "Failed to init capabilities");
#if CONFIG_APP_CLAW_CAP_IM_QQ
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("qq", "qq_send_message"),
                        TAG, "Failed to bind QQ outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("feishu", "feishu_send_message"),
                        TAG, "Failed to bind Feishu outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("telegram", "tg_send_message"),
                        TAG, "Failed to bind Telegram outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("wechat", "wechat_send_message"),
                        TAG, "Failed to bind WeChat outbound");
#endif
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_RETURN_ON_ERROR(claw_event_router_register_outbound_binding("web", "local_send_message"),
                        TAG, "Failed to bind Web / local IM outbound");
#endif

    core_config.api_key = config->llm_api_key;
    core_config.backend_type = config->llm_backend_type;
    core_config.model = config->llm_model;
    core_config.base_url = config->llm_base_url;
    core_config.auth_type = config->llm_auth_type;
    core_config.max_tokens_field = config->llm_max_tokens_field;
    core_config.timeout_ms = (uint32_t)strtoul(config->llm_timeout_ms, NULL, 10);
    core_config.max_tokens = (uint32_t)strtoul(config->llm_max_tokens, NULL, 10);
    core_config.image_max_bytes = (size_t)strtoul(config->llm_default_image_max_bytes, NULL, 10);
    core_config.supports_tools = app_claw_bool_is_true(config->llm_supports_tools);
    core_config.supports_vision = app_claw_bool_is_true(config->llm_supports_vision);
    core_config.image_remote_url_only = app_claw_bool_is_true(config->llm_image_remote_url_only);
    core_config.system_prompt = APP_SYSTEM_PROMPT;
#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
    core_config.persist_session = claw_memory_persist_session_callback;
    core_config.request_gate = claw_memory_request_gate_callback;
    core_config.on_request_start = claw_memory_request_start_callback;
    core_config.collect_stage_note = claw_memory_stage_note_callback;
#else
    core_config.persist_session = claw_memory_persist_session_callback;
    core_config.request_gate = claw_memory_request_gate_callback;
#endif
    core_config.call_cap = claw_cap_call_from_core;
    core_config.task_stack_size = 16 * 1024;
    core_config.task_priority = 5;
    core_config.task_core = tskNO_AFFINITY;
    core_config.max_tool_iterations = max_tool_iterations;
    core_config.request_queue_len = 4;
    core_config.response_queue_len = 4;
    core_config.max_context_providers = 8;
    if (!llm_enabled) {
        ESP_LOGW(TAG, "LLM is not fully configured. backend=%s base_url=%s model=%s. "
                      "The demo will start without claw_core; ask, auto-route-to-agent, and image analysis stay disabled until LLM API key, backend type, and model are set.",
                 config->llm_backend_type[0] ? config->llm_backend_type : "(empty)",
                 config->llm_base_url[0] ? config->llm_base_url : "(empty)",
                 config->llm_model[0] ? config->llm_model : "(empty)");
    } else {
        ESP_LOGI(TAG, "Starting LLM backend=%s base_url=%s model=%s",
                 config->llm_backend_type[0] ? config->llm_backend_type : "(default)",
                 config->llm_base_url[0] ? config->llm_base_url : "(empty)",
                 config->llm_model);
        ESP_RETURN_ON_ERROR(claw_core_init(&core_config), TAG, "Failed to init claw_core");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_profile_provider),
                            TAG, "Failed to add editable profile memory provider");

#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_long_term_provider),
                            TAG, "Failed to add long-term memory provider");
#else
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_long_term_lightweight_provider),
                            TAG, "Failed to add lightweight long-term memory provider");
#endif

        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_memory_session_history_provider),
                            TAG, "Failed to add session history provider");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_skill_skills_list_provider),
                            TAG, "Failed to add skills list provider");
        ESP_RETURN_ON_ERROR(claw_core_add_context_provider(&claw_cap_tools_provider),
                            TAG, "Failed to add cap tools provider");
        ESP_RETURN_ON_ERROR(claw_core_start(), TAG, "Failed to start claw_core");
    }

    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "Failed to start event router");
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    ESP_RETURN_ON_ERROR(cap_scheduler_start(), TAG, "Failed to start scheduler");
#endif

#if CONFIG_APP_CLAW_CAP_TIME
    ESP_ERROR_CHECK(cap_time_sync_service_start(&(cap_time_sync_service_config_t) {
                        .network_ready = NULL,
#if CONFIG_APP_CLAW_CAP_SCHEDULER
                        .on_sync_success = app_time_sync_success,
#else
                        .on_sync_success = NULL,
#endif
                    }));
#endif

#if CONFIG_APP_CLAW_ENABLE_CLI
    ESP_RETURN_ON_ERROR(app_claw_cli_start(), TAG, "Failed to start CLI");
#endif
    ESP_RETURN_ON_ERROR(app_claw_publish_startup_event(), TAG,
                        "Failed to publish startup event");

    return ESP_OK;
}
