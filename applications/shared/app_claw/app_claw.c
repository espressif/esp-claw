/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
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
#include "claw_paths.h"
#if CONFIG_APP_CLAW_CAP_CORE
#include "claw_agent.h"
#endif
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#endif
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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

#define APP_SYSTEM_PROMPT_SUFFIX "\n"

#define APP_SYSTEM_PROMPT \
    APP_SYSTEM_PROMPT_COMMON \
    APP_SYSTEM_PROMPT_SUFFIX

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
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "agent", paths->agent_persistence_dir, sizeof(paths->agent_persistence_dir)),
                        TAG, "agent persistence path too long");
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

esp_err_t app_claw_start(const app_claw_config_t *config)
{
    app_claw_storage_paths_t paths;
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    claw_event_router_config_t router_config = {
        .rules_path = NULL,
        .task_stack_size = 8 * 1024,
        .task_priority = 5,
        .task_core = tskNO_AFFINITY,
        .default_route_messages_to_agent = false,
    };
#endif
    bool llm_enabled = false;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(build_storage_paths(&paths), TAG, "Failed to resolve storage paths");

    llm_enabled = app_llm_is_configured(config);
#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    router_config.default_route_messages_to_agent = llm_enabled;
    router_config.rules_path = paths.router_rules_path;
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(claw_event_router_init(&router_config), TAG, "Failed to init event router");
#endif

#if CONFIG_APP_CLAW_CAP_SCHEDULER
    ESP_RETURN_ON_ERROR(cap_scheduler_init(&(cap_scheduler_config_t) {
                            .schedules_path = paths.scheduler_rules_path,
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
    ESP_RETURN_ON_ERROR(app_capabilities_init(config, &paths), TAG, "Failed to init capabilities");
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
    if (!llm_enabled) {
        ESP_LOGW(TAG, "LLM is not fully configured. backend=%s base_url=%s model=%s. "
                      "The demo will start without claw_agent; ask and auto-route-to-agent stay disabled until LLM API key, backend type, and model are set.",
                 config->llm_backend_type[0] ? config->llm_backend_type : "(empty)",
                 config->llm_base_url[0] ? config->llm_base_url : "(empty)",
                 config->llm_model[0] ? config->llm_model : "(empty)");
    } else {
        ESP_LOGI(TAG, "Starting LLM backend=%s base_url=%s model=%s",
                 config->llm_backend_type[0] ? config->llm_backend_type : "(default)",
                 config->llm_base_url[0] ? config->llm_base_url : "(empty)",
                 config->llm_model);
        ESP_RETURN_ON_ERROR(claw_agent_init(&(claw_agent_config_t) {
                                .api_key = config->llm_api_key,
                                .backend_type = config->llm_backend_type,
                                .model = config->llm_model,
                                .base_url = config->llm_base_url,
                                .persistence_dir = paths.agent_persistence_dir,
                                .skills_root_dir = paths.skills_root_dir,
                                .system_skills_root_dir = paths.system_skills_root_dir,
                            }),
                            TAG, "Failed to init claw_agent");
        ESP_RETURN_ON_ERROR(claw_agent_start(), TAG, "Failed to start claw_agent");
        ESP_LOGI(TAG, "claw_agent ready");
    }
#endif

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(claw_event_router_start(), TAG, "Failed to start event router");
#endif
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

#if CONFIG_APP_CLAW_CAP_EVENT_ROUTER
    ESP_RETURN_ON_ERROR(app_claw_publish_startup_event(), TAG,
                        "Failed to publish startup event");
#endif
    ESP_LOGI(TAG, "App Claw runtime started");

    return ESP_OK;
}
