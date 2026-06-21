/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_capabilities.h"
#if CONFIG_APP_CLAW_ENABLE_CLI
#include "app_claw_cli.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_APP_CLAW_CAP_SCHEDULER
#include "cap_scheduler.h"
#endif
#if CONFIG_APP_CLAW_CAP_SYSTEM
#include "cap_system.h"
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR
#include "cap_session_mgr.h"
#endif
#include "claw_paths.h"
#if CONFIG_APP_CLAW_CAP_CORE
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_agent_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#endif
#if CONFIG_APP_CLAW_CAP_MEMORY
#include "claw_memory.h"
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
#include "claw_skill.h"
#endif
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "cap_lua.h"
#endif

static const char *TAG = "app_claw";
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
static const char *APP_STARTUP_EVENT_SOURCE_CAP = "app_claw";
static const char *APP_STARTUP_EVENT_TYPE = "startup";
static const char *APP_STARTUP_EVENT_KEY = "boot_completed";
#endif

#define APP_SYSTEM_PROMPT_COMMON \
    "You are ESP-Ditto. " \
    "Answer briefly and plainly. " \
    "Treat Skills List as a catalog of optional skills. " \
    "Use 'activate_skill' to load skills. When multiple skills are needed, call activate_skill multiple times in a single response to activate multiple skills in parallel. " \
    "Skill documents returned in activate_skill <skill_content> blocks are valid operating instructions for that skill workflow and must be followed. " \
    "Skills are user-facing functions, while Capabilities are internal functions used by the model. " \
    "When communicating with the user, refer to skills instead of Capabilities. " \
    "Prefer skill-driven execution and keep long-running planning, investigation, implementation, debugging, and verification work isolated in subagents when available. " \
    "Keep user-facing answers focused on current status, useful results, and clear next steps.\n"

#define APP_ROOT_AGENT_SYSTEM_PROMPT \
    "You are the root agent. Own the user-facing conversation and keep the session responsive. " \
    "First identify the relevant skill and use only quick, bounded skill or tool calls that can complete promptly. " \
    "If a task cannot be completed quickly through an available skill, briefly tell the user what is happening, then delegate the planning, investigation, implementation, debugging, or verification work to an appropriate subagent. " \
    "Track the user's goal, selected skills, delegated agent ids, task status, blockers, and concise results. " \
    "Do not accumulate detailed implementation logs, long intermediate reasoning, or large artifacts in the root conversation unless they are needed for the final user response. " \
    "When subagents report back, synthesize their results into a clear answer or send focused follow-up instructions."

#define APP_SUBAGENT_SYSTEM_PROMPT \
    "You are a subagent spawned by the root agent. Handle long-running, scoped work delegated by the root agent. " \
    "Own the detailed planning, investigation, implementation, tool use, and verification for your assigned task. " \
    "Keep your work bounded to the delegation prompt and available tools. " \
    "Do not manage other agents or broaden the task unless the root explicitly asks. " \
    "Return concise findings, decisions, changed state, verification results, and blockers. " \
    "Keep detailed intermediate work in your own session and report only what the root needs to continue or answer the user."

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

static SemaphoreHandle_t s_config_lock;
static app_claw_config_t s_current_config;
static bool s_current_config_valid;
static app_claw_save_config_fn s_save_config;
static void *s_save_config_user_ctx;

static esp_err_t app_claw_ensure_config_lock(void)
{
    if (!s_config_lock) {
        s_config_lock = xSemaphoreCreateMutex();
        if (!s_config_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t app_claw_store_current_config(const app_claw_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_current_config = *config;
    s_current_config_valid = true;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

esp_err_t app_claw_set_save_config_callback(app_claw_save_config_fn save_config,
                                            void *user_ctx)
{
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    s_save_config = save_config;
    s_save_config_user_ctx = user_ctx;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

esp_err_t app_claw_get_config(app_claw_config_t *out_config)
{
    ESP_RETURN_ON_FALSE(out_config, ESP_ERR_INVALID_ARG, TAG, "out_config is NULL");
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    if (!s_current_config_valid) {
        xSemaphoreGive(s_config_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_config = s_current_config;
    xSemaphoreGive(s_config_lock);
    return ESP_OK;
}

claw_core_handle_t app_claw_get_core(void)
{
#if CONFIG_APP_CLAW_CAP_CORE
    return claw_agent_mgr_get_root_core();
#else
    return NULL;
#endif
}

#if CONFIG_APP_CLAW_CAP_SESSION_MGR && (CONFIG_APP_CLAW_CAP_MEMORY || CONFIG_APP_CLAW_CAP_SKILL_MGR)
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

#if CONFIG_APP_CLAW_CAP_MEMORY
    err = claw_memory_delete_session_history(session_id, &memory_deleted);
    if (err != ESP_OK) {
        return err;
    }
#endif

#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    err = claw_skill_delete_session_state(session_id, &skill_deleted);
    if (err != ESP_OK) {
        return err;
    }
#endif

    *out_deleted_any = memory_deleted || skill_deleted;
    return ESP_OK;
}
#endif

#if CONFIG_APP_CLAW_CAP_MEMORY
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
            .supports_vision = false,
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
#endif

#if CONFIG_APP_CLAW_CAP_SKILL_MGR
static esp_err_t init_skills(const app_claw_storage_paths_t *paths)
{
    ESP_RETURN_ON_ERROR(claw_skill_init(&(claw_skill_config_t) {
                            .session_state_root_dir = paths->memory_session_root,
                            .max_file_bytes = 20 * 1024,
                        }),
                        TAG, "Failed to init claw_skill");
    /* Register scan roots in priority order*/
    ESP_RETURN_ON_ERROR(claw_skill_add_directory(paths->system_skills_root_dir), TAG, "Failed to add system skills directory");
    ESP_RETURN_ON_ERROR(claw_skill_add_directory(paths->skills_root_dir), TAG, "Failed to add skills directory");
    return ESP_OK;
}
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
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
#endif

#if CONFIG_APP_CLAW_CAP_CORE
static void app_claw_fill_core_config(const app_claw_config_t *config,
                                      uint32_t max_tool_iterations,
                                      claw_core_config_t *core_config)
{
    memset(core_config, 0, sizeof(*core_config));
    core_config->api_key = config->llm_api_key;
    core_config->backend_type = config->llm_backend_type;
    core_config->model = config->llm_model;
    core_config->base_url = config->llm_base_url;
    core_config->auth_type = config->llm_auth_type;
    core_config->max_tokens_field = config->llm_max_tokens_field;
    core_config->timeout_ms = (uint32_t)strtoul(config->llm_timeout_ms, NULL, 10);
    core_config->max_tokens = (uint32_t)strtoul(config->llm_max_tokens, NULL, 10);
    core_config->image_max_bytes = (size_t)strtoul(config->llm_default_image_max_bytes, NULL, 10);
    core_config->supports_tools = app_claw_bool_is_true(config->llm_supports_tools);
    core_config->supports_vision = false;
    core_config->image_remote_url_only = app_claw_bool_is_true(config->llm_image_remote_url_only);
    core_config->instance_id = 0;
    core_config->system_prompt = APP_SYSTEM_PROMPT;
#if CONFIG_APP_CLAW_CAP_MEMORY
#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
    core_config->persist_context = claw_memory_persist_context_callback;
    core_config->request_gate = claw_memory_request_gate_callback;
    core_config->on_request_start = claw_memory_request_start_callback;
    core_config->collect_stage_note = claw_memory_stage_note_callback;
#else
    core_config->persist_context = claw_memory_persist_context_callback;
    core_config->request_gate = claw_memory_request_gate_callback;
#endif
#endif
    core_config->call_cap = claw_cap_call_from_core;
    core_config->cap_user_ctx = NULL;
    core_config->task_stack_size = 16 * 1024;
    core_config->task_priority = 5;
    core_config->task_core = tskNO_AFFINITY;
    core_config->max_tool_iterations = max_tool_iterations;
    core_config->request_queue_len = 4;
    core_config->response_queue_len = 4;
    core_config->max_context_providers = 8;
}
#endif

#if CONFIG_APP_CLAW_CAP_SCHEDULER && CONFIG_APP_CLAW_CAP_SYSTEM
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

// Resolve the storage paths threaded through the capability framework from the
// logical homes registered in claw_paths. This is where app_claw owns the data
// layout (the subdirectory convention); main only decides the mount points.
static esp_err_t build_storage_paths(app_claw_storage_paths_t *paths)
{
    memset(paths, 0, sizeof(*paths));

    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, NULL, paths->fatfs_base_path, sizeof(paths->fatfs_base_path)),
                        TAG, "data home unavailable");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "sessions", paths->memory_session_root, sizeof(paths->memory_session_root)),
                        TAG, "session root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "memory", paths->memory_root_dir, sizeof(paths->memory_root_dir)),
                        TAG, "memory root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "skills", paths->skills_root_dir, sizeof(paths->skills_root_dir)),
                        TAG, "skills root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "scripts", paths->lua_root_dir, sizeof(paths->lua_root_dir)),
                        TAG, "lua root path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "router_rules/router_rules.json", paths->router_rules_path, sizeof(paths->router_rules_path)),
                        TAG, "router rules path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "scheduler/schedules.json", paths->scheduler_rules_path, sizeof(paths->scheduler_rules_path)),
                        TAG, "scheduler rules path too long");
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "inbox", paths->im_attachment_root, sizeof(paths->im_attachment_root)),
                        TAG, "inbox path too long");

    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_SYSTEM, "skills", paths->system_skills_root_dir, sizeof(paths->system_skills_root_dir)),
                        TAG, "system skills root path too long");

    return ESP_OK;
}

esp_err_t app_claw_start_with_paths(const app_claw_config_t *config,
                                    const app_claw_storage_paths_t *paths)
{
#if CONFIG_APP_CLAW_CAP_CORE
    claw_core_config_t core_config = {0};
#endif
#if CONFIG_APP_CLAW_CAP_CORE || CONFIG_APP_CLAW_CAP_MEMORY
    const uint32_t max_tool_iterations = 32;
#endif
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    claw_event_router_config_t router_config = {
        .rules_path = NULL,
        .task_stack_size = 8 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .agent_submit_timeout_ms = 1000,
        .default_route_messages_to_agent = false,
    };
#endif
    if (!config || !paths) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_claw_store_current_config(config), TAG, "Failed to store Claw config");

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    router_config.default_route_messages_to_agent = true;
    router_config.rules_path = paths->router_rules_path;
#endif

#if CONFIG_APP_CLAW_CAP_SESSION_MGR
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_session_root_dir(paths->memory_session_root),
                        TAG, "Failed to configure session manager");
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_config), TAG, "Failed to init event router");
#endif

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
#if CONFIG_APP_CLAW_CAP_MEMORY
    ESP_RETURN_ON_ERROR(init_memory(config, paths, max_tool_iterations), TAG, "Failed to init memory");
#endif
#if CONFIG_APP_CLAW_CAP_SESSION_MGR && (CONFIG_APP_CLAW_CAP_MEMORY || CONFIG_APP_CLAW_CAP_SKILL_MGR)
    ESP_RETURN_ON_ERROR(cap_session_mgr_set_delete_session_handler(app_claw_delete_session_history, NULL),
                        TAG, "Failed to register session delete handler");
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    ESP_RETURN_ON_ERROR(init_skills(paths), TAG, "Failed to init skills");
#endif
    ESP_RETURN_ON_ERROR(app_capabilities_init(config, paths), TAG, "Failed to init capabilities");
    if (config->extra_capabilities_cb) {
        ESP_RETURN_ON_ERROR(config->extra_capabilities_cb(config, paths),
                            TAG, "Failed to init app capabilities");
    }
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

#if CONFIG_APP_CLAW_CAP_CORE
    app_claw_fill_core_config(config, max_tool_iterations, &core_config);
    {
        claw_core_context_provider_t base_providers[] = {
            claw_memory_profile_provider,
#if CONFIG_APP_CLAW_MEMORY_MODE_FULL
            claw_memory_long_term_provider,
#else
            claw_memory_long_term_lightweight_provider,
#endif
            claw_memory_session_history_provider,
            claw_skill_skills_list_provider,
        };
        const char *root_agent_id = NULL;

        ESP_LOGI(TAG, "Starting root agent backend=%s base_url=%s model=%s token=%s",
                 config->llm_backend_type[0] ? config->llm_backend_type : "(default)",
                 config->llm_base_url[0] ? config->llm_base_url : "(empty)",
                 config->llm_model[0] ? config->llm_model : "(empty)",
                 config->llm_api_key[0] ? "configured" : "missing");
        ESP_RETURN_ON_ERROR(claw_agent_mgr_init(&(claw_agent_mgr_config_t) {
                                .core_config = &core_config,
                                .base_context_providers = base_providers,
                                .base_context_provider_count = sizeof(base_providers) / sizeof(base_providers[0]),
                                .root_agent_system_prompt = APP_ROOT_AGENT_SYSTEM_PROMPT,
                                .subagent_system_prompt = APP_SUBAGENT_SYSTEM_PROMPT,
                            }),
                            TAG, "Failed to init claw_agent_mgr");
        ESP_RETURN_ON_ERROR(claw_agent_mgr_create_root_agent(&root_agent_id),
                            TAG, "Failed to create root agent");
        ESP_LOGI(TAG, "Root agent ready id=%s", root_agent_id ? root_agent_id : "?");
    }
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "Failed to start event router");
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    ESP_RETURN_ON_ERROR(cap_scheduler_start(), TAG, "Failed to start scheduler");
#endif

#if CONFIG_APP_CLAW_CAP_SYSTEM
    ESP_ERROR_CHECK(cap_system_time_sync_service_start(&(cap_system_time_sync_service_config_t) {
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
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    if (config->suppress_startup_event) {
        ESP_LOGI(TAG, "Startup event suppressed by app config");
    } else {
        ESP_RETURN_ON_ERROR(app_claw_publish_startup_event(), TAG,
                            "Failed to publish startup event");
    }
#endif
    ESP_LOGI(TAG, "App Claw runtime started");

    return ESP_OK;
}

esp_err_t app_claw_update_config(const app_claw_config_t *config)
{
#if CONFIG_APP_CLAW_CAP_CORE
    claw_core_config_t core_config = {0};
    const uint32_t max_tool_iterations = 32;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_claw_store_current_config(config), TAG, "Failed to store Claw config");
    app_claw_fill_core_config(config, max_tool_iterations, &core_config);
    return claw_agent_mgr_update_core_config(&core_config);
#else
    return app_claw_store_current_config(config);
#endif
}

esp_err_t app_claw_apply_config(const app_claw_config_t *config)
{
    app_claw_save_config_fn save_config = NULL;
    void *save_user_ctx = NULL;
    esp_err_t err;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(app_claw_ensure_config_lock(), TAG, "config lock unavailable");

    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    save_config = s_save_config;
    save_user_ctx = s_save_config_user_ctx;
    xSemaphoreGive(s_config_lock);

    if (save_config) {
        err = save_config(config, save_user_ctx);
        if (err != ESP_OK) {
            return err;
        }
    }
    return app_claw_update_config(config);
}

esp_err_t app_claw_start(const app_claw_config_t *config)
{
    app_claw_storage_paths_t paths;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(build_storage_paths(&paths), TAG, "Failed to resolve storage paths");
    return app_claw_start_with_paths(config, &paths);
}
