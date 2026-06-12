/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_agent_mgr.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "claw_agent_mgr";

#ifndef CONFIG_CLAW_AGENT_MGR_MAX_SUBAGENTS
#define CONFIG_CLAW_AGENT_MGR_MAX_SUBAGENTS 3
#endif

#define CLAW_AGENT_MGR_MAX_BASE_PROVIDERS 7
#define CLAW_AGENT_MGR_MAX_AGENTS        (1 + CONFIG_CLAW_AGENT_MGR_MAX_SUBAGENTS)
#define CLAW_AGENT_MGR_NOTIFY_TEXT_SIZE  768

static const char *CLAW_AGENT_MGR_DEFAULT_ROOT_AGENT_SYSTEM_PROMPT =
    "You are the root agent. Own the user-facing conversation, decide whether work "
    "should be handled directly or delegated, spawn subagents only for bounded tasks, "
    "track their results, and integrate their findings into the final response.";

static const char *CLAW_AGENT_MGR_ROOT_AGENT_TYPE_PROMPT =
    "Agent type: root. Coordinate the session, manage subagents when useful, and "
    "deliver the final answer to the user.";

static const char *CLAW_AGENT_MGR_DEFAULT_SUBAGENT_SYSTEM_PROMPT =
    "You are a subagent spawned by the root agent. Work only on the delegated task "
    "and keep your scope narrow. Do not manage, spawn, close, or inspect other agents. "
    "Use the tools available to you when needed, then report concise findings, decisions, "
    "and any blockers back to the root agent.";

static const char *CLAW_AGENT_MGR_PROMPT_STACK_FMT =
    "%s\n\n# %s Role\n%s\n\n# %s Type\n%s\n\nSelected agent_type: %s";

static const claw_agent_mgr_subagent_type_prompt_t s_default_subagent_type_prompts[] = {
    {
        .agent_type = "subagent",
        .system_prompt = "Agent type: subagent. Follow the parent agent's prompt as the task definition "
                         "and return the useful result.",
    },
    {
        .agent_type = "research",
        .system_prompt = "Agent type: research. Prioritize investigation, source/context gathering, "
                         "and concise synthesis. Do not make code changes unless the delegated task "
                         "explicitly asks for them.",
    },
    {
        .agent_type = "coding",
        .system_prompt = "Agent type: coding. Prioritize implementation details, affected files, "
                         "focused fixes, and verification. Keep changes scoped to the delegated task.",
    },
    {
        .agent_type = "worker",
        .system_prompt = "Agent type: coding. Prioritize implementation details, affected files, "
                         "focused fixes, and verification. Keep changes scoped to the delegated task.",
    },
    {
        .agent_type = "debug",
        .system_prompt = "Agent type: debug. Prioritize reproducing symptoms, isolating root cause, "
                         "and proposing the smallest defensible fix with verification steps.",
    },
    {
        .agent_type = "debugger",
        .system_prompt = "Agent type: debug. Prioritize reproducing symptoms, isolating root cause, "
                         "and proposing the smallest defensible fix with verification steps.",
    },
};

typedef enum {
    CLAW_AGENT_MGR_ROLE_ROOT = 0,
    CLAW_AGENT_MGR_ROLE_SUBAGENT,
} claw_agent_mgr_role_t;

typedef struct {
    bool occupied;
    claw_agent_mgr_role_t role;
    claw_agent_mgr_status_t status;
    char agent_id[CLAW_SESSION_MGR_ID_SIZE];
    char session_id[CLAW_SESSION_MGR_ID_SIZE];
    char parent_session_id[CLAW_SESSION_MGR_ID_SIZE];
    char agent_type[32];
    char parent_source_channel[16];
    char parent_source_chat_id[96];
    char parent_target_channel[16];
    char parent_target_chat_id[96];
    bool background;
    bool closing;
    claw_core_handle_t core;
    claw_cap_core_call_user_ctx_t cap_user_ctx;
    uint32_t last_request_id;
    char last_error[96];
} claw_agent_mgr_agent_t;

typedef struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    claw_core_config_t core_config;
    char *api_key;
    char *backend_type;
    char *model;
    char *base_url;
    char *auth_type;
    char *max_tokens_field;
    char *system_prompt;
    char *root_agent_system_prompt;
    char *subagent_system_prompt;
    claw_agent_mgr_subagent_type_prompt_t *subagent_type_prompts;
    size_t subagent_type_prompt_count;
    claw_core_context_provider_t base_providers[CLAW_AGENT_MGR_MAX_BASE_PROVIDERS];
    size_t base_provider_count;
    uint32_t next_instance_id;
    uint32_t next_request_id;
    claw_agent_mgr_agent_t agents[CLAW_AGENT_MGR_MAX_AGENTS];
} claw_agent_mgr_state_t;

static claw_agent_mgr_state_t s_mgr = {0};

static void claw_agent_mgr_completion_observer(const claw_core_completion_summary_t *summary,
                                               void *user_ctx);
static void claw_agent_mgr_fill_info_locked(const claw_agent_mgr_agent_t *agent,
                                            claw_agent_mgr_agent_info_t *out_info);

static char *claw_agent_mgr_strdup(const char *src)
{
    char *copy = NULL;
    size_t len;

    if (!src) {
        return NULL;
    }
    len = strlen(src) + 1;
    copy = malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, src, len);
    return copy;
}

static void claw_agent_mgr_free_prompt_config(void)
{
    if (s_mgr.subagent_type_prompts) {
        for (size_t i = 0; i < s_mgr.subagent_type_prompt_count; i++) {
            free((char *)s_mgr.subagent_type_prompts[i].agent_type);
            free((char *)s_mgr.subagent_type_prompts[i].system_prompt);
        }
        free(s_mgr.subagent_type_prompts);
    }
    s_mgr.subagent_type_prompts = NULL;
    s_mgr.subagent_type_prompt_count = 0;
    free(s_mgr.root_agent_system_prompt);
    s_mgr.root_agent_system_prompt = NULL;
    free(s_mgr.subagent_system_prompt);
    s_mgr.subagent_system_prompt = NULL;
}

static void claw_agent_mgr_free_config_storage(void)
{
    free(s_mgr.api_key);
    free(s_mgr.backend_type);
    free(s_mgr.model);
    free(s_mgr.base_url);
    free(s_mgr.auth_type);
    free(s_mgr.max_tokens_field);
    free(s_mgr.system_prompt);
    s_mgr.api_key = NULL;
    s_mgr.backend_type = NULL;
    s_mgr.model = NULL;
    s_mgr.base_url = NULL;
    s_mgr.auth_type = NULL;
    s_mgr.max_tokens_field = NULL;
    s_mgr.system_prompt = NULL;
    claw_agent_mgr_free_prompt_config();
}

static const char *claw_agent_mgr_find_subagent_type_prompt(const char *agent_type)
{
    const char *type_name = (agent_type && agent_type[0]) ? agent_type : "subagent";

    for (size_t i = 0; i < s_mgr.subagent_type_prompt_count; i++) {
        if (s_mgr.subagent_type_prompts[i].agent_type &&
                strcmp(s_mgr.subagent_type_prompts[i].agent_type, type_name) == 0) {
            return s_mgr.subagent_type_prompts[i].system_prompt;
        }
    }
    for (size_t i = 0; i < sizeof(s_default_subagent_type_prompts) /
            sizeof(s_default_subagent_type_prompts[0]); i++) {
        if (strcmp(s_default_subagent_type_prompts[i].agent_type, type_name) == 0) {
            return s_default_subagent_type_prompts[i].system_prompt;
        }
    }

    return s_default_subagent_type_prompts[0].system_prompt;
}

static char *claw_agent_mgr_format_prompt_stack(const char *base_prompt,
                                                const char *role_name,
                                                const char *role_prompt,
                                                const char *type_name,
                                                const char *type_prompt,
                                                const char *agent_type)
{
    const char *selected_type = (agent_type && agent_type[0]) ? agent_type : "subagent";
    int needed;
    char *prompt = NULL;

    if (!base_prompt || !role_name || !role_prompt || !type_name || !type_prompt) {
        return NULL;
    }

    needed = snprintf(NULL,
                      0,
                      CLAW_AGENT_MGR_PROMPT_STACK_FMT,
                      base_prompt,
                      role_name,
                      role_prompt,
                      type_name,
                      type_prompt,
                      selected_type);
    if (needed < 0) {
        return NULL;
    }
    prompt = malloc((size_t)needed + 1U);
    if (!prompt) {
        return NULL;
    }
    if (snprintf(prompt,
                 (size_t)needed + 1U,
                 CLAW_AGENT_MGR_PROMPT_STACK_FMT,
                 base_prompt,
                 role_name,
                 role_prompt,
                 type_name,
                 type_prompt,
                 selected_type) < 0) {
        free(prompt);
        return NULL;
    }

    return prompt;
}

static char *claw_agent_mgr_build_agent_system_prompt(const claw_agent_mgr_agent_t *agent)
{
    if (!agent) {
        return NULL;
    }
    if (agent->role == CLAW_AGENT_MGR_ROLE_ROOT) {
        return claw_agent_mgr_format_prompt_stack(s_mgr.system_prompt,
                                                 "Root Agent",
                                                 s_mgr.root_agent_system_prompt,
                                                 "Root Agent",
                                                 CLAW_AGENT_MGR_ROOT_AGENT_TYPE_PROMPT,
                                                 agent->agent_type);
    }
    return claw_agent_mgr_format_prompt_stack(s_mgr.system_prompt,
                                             "Subagent",
                                             s_mgr.subagent_system_prompt,
                                             "Subagent",
                                             claw_agent_mgr_find_subagent_type_prompt(agent->agent_type),
                                             agent->agent_type);
}

static void claw_agent_mgr_lock(void)
{
    xSemaphoreTakeRecursive(s_mgr.mutex, portMAX_DELAY);
}

static void claw_agent_mgr_unlock(void)
{
    xSemaphoreGiveRecursive(s_mgr.mutex);
}

static bool claw_agent_mgr_is_ready(void)
{
    return s_mgr.initialized && s_mgr.mutex;
}

static claw_agent_mgr_agent_t *claw_agent_mgr_find_locked(const char *agent_id)
{
    if (!agent_id || !agent_id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < CLAW_AGENT_MGR_MAX_AGENTS; i++) {
        if (s_mgr.agents[i].occupied && strcmp(s_mgr.agents[i].agent_id, agent_id) == 0) {
            return &s_mgr.agents[i];
        }
    }

    return NULL;
}

static claw_agent_mgr_agent_t *claw_agent_mgr_alloc_locked(claw_agent_mgr_role_t role)
{
    size_t start = role == CLAW_AGENT_MGR_ROLE_ROOT ? 0 : 1;

    if (role == CLAW_AGENT_MGR_ROLE_ROOT) {
        return s_mgr.agents[0].occupied ? NULL : &s_mgr.agents[0];
    }
    for (size_t i = start; i < CLAW_AGENT_MGR_MAX_AGENTS; i++) {
        if (!s_mgr.agents[i].occupied ||
                (s_mgr.agents[i].role == CLAW_AGENT_MGR_ROLE_SUBAGENT &&
                 s_mgr.agents[i].status == CLAW_AGENT_MGR_STATUS_CLOSED &&
                 !s_mgr.agents[i].core && !s_mgr.agents[i].closing)) {
            return &s_mgr.agents[i];
        }
    }

    return NULL;
}

static esp_err_t claw_agent_mgr_copy_core_config(const claw_core_config_t *config)
{
    if (!config || !config->api_key || !config->model || !config->backend_type ||
            !config->system_prompt) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mgr.api_key = claw_agent_mgr_strdup(config->api_key);
    s_mgr.backend_type = claw_agent_mgr_strdup(config->backend_type);
    s_mgr.model = claw_agent_mgr_strdup(config->model);
    s_mgr.base_url = claw_agent_mgr_strdup(config->base_url ? config->base_url : "");
    s_mgr.auth_type = claw_agent_mgr_strdup(config->auth_type ? config->auth_type : "");
    s_mgr.max_tokens_field = claw_agent_mgr_strdup(config->max_tokens_field ? config->max_tokens_field : "");
    s_mgr.system_prompt = claw_agent_mgr_strdup(config->system_prompt);
    if (!s_mgr.api_key || !s_mgr.backend_type || !s_mgr.model ||
            !s_mgr.base_url || !s_mgr.auth_type || !s_mgr.max_tokens_field ||
            !s_mgr.system_prompt) {
        return ESP_ERR_NO_MEM;
    }

    s_mgr.core_config = *config;
    s_mgr.core_config.api_key = s_mgr.api_key;
    s_mgr.core_config.backend_type = s_mgr.backend_type;
    s_mgr.core_config.model = s_mgr.model;
    s_mgr.core_config.base_url = s_mgr.base_url;
    s_mgr.core_config.auth_type = s_mgr.auth_type;
    s_mgr.core_config.max_tokens_field = s_mgr.max_tokens_field;
    s_mgr.core_config.system_prompt = s_mgr.system_prompt;
    return ESP_OK;
}

static esp_err_t claw_agent_mgr_copy_prompt_config(const claw_agent_mgr_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->subagent_type_prompt_count > 0 && !config->subagent_type_prompts) {
        return ESP_ERR_INVALID_ARG;
    }

    s_mgr.root_agent_system_prompt = claw_agent_mgr_strdup(
                                         config->root_agent_system_prompt ?
                                         config->root_agent_system_prompt :
                                         CLAW_AGENT_MGR_DEFAULT_ROOT_AGENT_SYSTEM_PROMPT);
    if (!s_mgr.root_agent_system_prompt) {
        return ESP_ERR_NO_MEM;
    }

    s_mgr.subagent_system_prompt = claw_agent_mgr_strdup(
                                       config->subagent_system_prompt ?
                                       config->subagent_system_prompt :
                                       CLAW_AGENT_MGR_DEFAULT_SUBAGENT_SYSTEM_PROMPT);
    if (!s_mgr.subagent_system_prompt) {
        return ESP_ERR_NO_MEM;
    }

    if (config->subagent_type_prompt_count == 0) {
        return ESP_OK;
    }

    s_mgr.subagent_type_prompts = calloc(config->subagent_type_prompt_count,
                                         sizeof(s_mgr.subagent_type_prompts[0]));
    if (!s_mgr.subagent_type_prompts) {
        return ESP_ERR_NO_MEM;
    }
    s_mgr.subagent_type_prompt_count = config->subagent_type_prompt_count;
    for (size_t i = 0; i < config->subagent_type_prompt_count; i++) {
        const claw_agent_mgr_subagent_type_prompt_t *src = &config->subagent_type_prompts[i];

        if (!src->agent_type || !src->agent_type[0] ||
                !src->system_prompt || !src->system_prompt[0]) {
            return ESP_ERR_INVALID_ARG;
        }
        s_mgr.subagent_type_prompts[i].agent_type = claw_agent_mgr_strdup(src->agent_type);
        s_mgr.subagent_type_prompts[i].system_prompt = claw_agent_mgr_strdup(src->system_prompt);
        if (!s_mgr.subagent_type_prompts[i].agent_type ||
                !s_mgr.subagent_type_prompts[i].system_prompt) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t claw_agent_mgr_init(const claw_agent_mgr_config_t *config)
{
    esp_err_t err;

    if (!config || !config->core_config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config->base_context_provider_count > CLAW_AGENT_MGR_MAX_BASE_PROVIDERS ||
            (config->base_context_provider_count > 0 && !config->base_context_providers)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_mgr, 0, sizeof(s_mgr));
    s_mgr.mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mgr.mutex) {
        return ESP_ERR_NO_MEM;
    }
    err = claw_agent_mgr_copy_core_config(config->core_config);
    if (err != ESP_OK) {
        goto fail;
    }
    err = claw_agent_mgr_copy_prompt_config(config);
    if (err != ESP_OK) {
        goto fail;
    }
    for (size_t i = 0; i < config->base_context_provider_count; i++) {
        s_mgr.base_providers[i] = config->base_context_providers[i];
    }
    s_mgr.base_provider_count = config->base_context_provider_count;
    s_mgr.next_instance_id = 0;
    s_mgr.next_request_id = 1;
    s_mgr.initialized = true;
    return ESP_OK;

fail:
    claw_agent_mgr_free_config_storage();
    if (s_mgr.mutex) {
        vSemaphoreDelete(s_mgr.mutex);
    }
    memset(&s_mgr, 0, sizeof(s_mgr));
    return err;
}

esp_err_t claw_agent_mgr_update_core_config(const claw_core_config_t *core_config)
{
    char *api_key = NULL;
    char *backend_type = NULL;
    char *model = NULL;
    char *base_url = NULL;
    char *auth_type = NULL;
    char *max_tokens_field = NULL;
    char *system_prompt = NULL;
    claw_core_config_t next_config = {0};
    esp_err_t err = ESP_OK;

    if (!core_config || !core_config->system_prompt) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_agent_mgr_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    api_key = claw_agent_mgr_strdup(core_config->api_key ? core_config->api_key : "");
    backend_type = claw_agent_mgr_strdup(core_config->backend_type ? core_config->backend_type : "");
    model = claw_agent_mgr_strdup(core_config->model ? core_config->model : "");
    base_url = claw_agent_mgr_strdup(core_config->base_url ? core_config->base_url : "");
    auth_type = claw_agent_mgr_strdup(core_config->auth_type ? core_config->auth_type : "");
    max_tokens_field = claw_agent_mgr_strdup(core_config->max_tokens_field ? core_config->max_tokens_field : "");
    system_prompt = claw_agent_mgr_strdup(core_config->system_prompt);
    if (!api_key || !backend_type || !model || !base_url ||
            !auth_type || !max_tokens_field || !system_prompt) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    next_config = *core_config;
    next_config.api_key = api_key;
    next_config.backend_type = backend_type;
    next_config.model = model;
    next_config.base_url = base_url;
    next_config.auth_type = auth_type;
    next_config.max_tokens_field = max_tokens_field;
    next_config.system_prompt = system_prompt;

    claw_agent_mgr_lock();
    free(s_mgr.api_key);
    free(s_mgr.backend_type);
    free(s_mgr.model);
    free(s_mgr.base_url);
    free(s_mgr.auth_type);
    free(s_mgr.max_tokens_field);
    free(s_mgr.system_prompt);
    s_mgr.api_key = api_key;
    s_mgr.backend_type = backend_type;
    s_mgr.model = model;
    s_mgr.base_url = base_url;
    s_mgr.auth_type = auth_type;
    s_mgr.max_tokens_field = max_tokens_field;
    s_mgr.system_prompt = system_prompt;
    s_mgr.core_config = next_config;
    api_key = NULL;
    backend_type = NULL;
    model = NULL;
    base_url = NULL;
    auth_type = NULL;
    max_tokens_field = NULL;
    system_prompt = NULL;

    for (size_t i = 0; i < CLAW_AGENT_MGR_MAX_AGENTS; i++) {
        if (s_mgr.agents[i].occupied && s_mgr.agents[i].core) {
            esp_err_t update_err = claw_core_update_llm_config(s_mgr.agents[i].core,
                                                               &s_mgr.core_config);
            if (update_err != ESP_OK && err == ESP_OK) {
                err = update_err;
            }
        }
    }
    claw_agent_mgr_unlock();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Updated agent manager LLM config");
    }

fail:
    free(api_key);
    free(backend_type);
    free(model);
    free(base_url);
    free(auth_type);
    free(max_tokens_field);
    free(system_prompt);
    return err;
}

static void claw_agent_mgr_fill_core_config(claw_agent_mgr_agent_t *agent,
                                            claw_core_config_t *out_config)
{
    *out_config = s_mgr.core_config;
    out_config->instance_id = s_mgr.next_instance_id++;
    out_config->cap_user_ctx = &agent->cap_user_ctx;
    out_config->max_context_providers = s_mgr.base_provider_count + 1;
}

static esp_err_t claw_agent_mgr_start_agent_core(claw_agent_mgr_agent_t *agent)
{
    claw_core_config_t core_config = {0};
    claw_core_context_provider_t cap_tools_provider = {0};
    char *agent_system_prompt = NULL;
    esp_err_t ret;

    if (!agent) {
        return ESP_ERR_INVALID_ARG;
    }
    if (agent->core) {
        return ESP_OK;
    }

    claw_agent_mgr_fill_core_config(agent, &core_config);
    agent_system_prompt = claw_agent_mgr_build_agent_system_prompt(agent);
    if (!agent_system_prompt) {
        snprintf(agent->last_error, sizeof(agent->last_error), "%s", esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    core_config.system_prompt = agent_system_prompt;
    agent->cap_user_ctx.magic = CLAW_CAP_CORE_CALL_USER_CTX_MAGIC;
    agent->cap_user_ctx.core = &agent->core;
    agent->cap_user_ctx.caller = agent->role == CLAW_AGENT_MGR_ROLE_ROOT ?
                                 CLAW_CAP_CALLER_ROOT_AGENT : CLAW_CAP_CALLER_SUB_AGENT;
    agent->cap_user_ctx.agent_id = agent->agent_id;
    agent->cap_user_ctx.agent_type = agent->agent_type;
    agent->cap_user_ctx.parent_agent_id = agent->role == CLAW_AGENT_MGR_ROLE_SUBAGENT ?
                                          CLAW_AGENT_MGR_ROOT_AGENT_ID : NULL;
    agent->cap_user_ctx.parent_session_id = agent->role == CLAW_AGENT_MGR_ROLE_SUBAGENT ?
                                            agent->parent_session_id : NULL;

    ret = claw_core_create(&core_config, &agent->core);
    free(agent_system_prompt);
    agent_system_prompt = NULL;
    if (ret != ESP_OK) {
        snprintf(agent->last_error, sizeof(agent->last_error), "%s", esp_err_to_name(ret));
        return ret;
    }
    for (size_t i = 0; i < s_mgr.base_provider_count; i++) {
        ESP_GOTO_ON_ERROR(claw_core_add_context_provider(agent->core, &s_mgr.base_providers[i]),
                          fail, TAG, "add base context provider");
    }
    cap_tools_provider = agent->role == CLAW_AGENT_MGR_ROLE_ROOT ?
                         claw_cap_root_agent_tools_provider :
                         claw_cap_sub_agent_tools_provider;
    cap_tools_provider.user_ctx = &agent->cap_user_ctx;
    ESP_GOTO_ON_ERROR(claw_core_add_context_provider(agent->core, &cap_tools_provider),
                      fail, TAG, "add cap tools provider");
    if (agent->role == CLAW_AGENT_MGR_ROLE_SUBAGENT) {
        ESP_GOTO_ON_ERROR(claw_core_add_completion_observer(agent->core,
                                                            claw_agent_mgr_completion_observer,
                                                            agent),
                          fail, TAG, "add completion observer");
    }
    ESP_GOTO_ON_ERROR(claw_core_start(agent->core), fail, TAG, "start agent core");
    agent->status = CLAW_AGENT_MGR_STATUS_IDLE;
    return ESP_OK;

fail:
    if (agent->core) {
        (void)claw_core_destroy(agent->core);
        agent->core = NULL;
    }
    snprintf(agent->last_error, sizeof(agent->last_error), "%s", esp_err_to_name(ret));
    return ret;
}

esp_err_t claw_agent_mgr_create_root_agent(const char **out_agent_id)
{
    claw_agent_mgr_agent_t *agent = NULL;
    esp_err_t err;

    if (!claw_agent_mgr_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    claw_agent_mgr_lock();
    agent = claw_agent_mgr_alloc_locked(CLAW_AGENT_MGR_ROLE_ROOT);
    if (!agent) {
        claw_agent_mgr_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(agent, 0, sizeof(*agent));
    agent->occupied = true;
    agent->role = CLAW_AGENT_MGR_ROLE_ROOT;
    agent->status = CLAW_AGENT_MGR_STATUS_RUNNING;
    strlcpy(agent->agent_id, CLAW_AGENT_MGR_ROOT_AGENT_ID, sizeof(agent->agent_id));
    strlcpy(agent->agent_type, "root", sizeof(agent->agent_type));
    err = claw_agent_mgr_start_agent_core(agent);
    if (err != ESP_OK) {
        memset(agent, 0, sizeof(*agent));
        claw_agent_mgr_unlock();
        return err;
    }
    if (out_agent_id) {
        *out_agent_id = agent->agent_id;
    }
    claw_agent_mgr_unlock();
    ESP_LOGI(TAG, "Created root agent id=%s", CLAW_AGENT_MGR_ROOT_AGENT_ID);
    return ESP_OK;
}

claw_core_handle_t claw_agent_mgr_get_root_core(void)
{
    claw_core_handle_t core = NULL;

    if (!s_mgr.initialized || !s_mgr.mutex) {
        return NULL;
    }
    claw_agent_mgr_lock();
    if (s_mgr.agents[0].occupied) {
        core = s_mgr.agents[0].core;
    }
    claw_agent_mgr_unlock();
    return core;
}

static esp_err_t claw_agent_mgr_build_root_session_id(const claw_agent_mgr_root_input_t *input,
                                                      char *session_id,
                                                      size_t session_id_size,
                                                      size_t *out_len)
{
    claw_session_build_context_t ctx = {0};

    ctx.agent_id = 0;
    ctx.session_policy = input->session_policy;
    ctx.source_cap = input->source_cap;
    ctx.event_type = input->event_type;
    ctx.source_channel = input->source_channel;
    ctx.chat_id = input->source_chat_id;
    ctx.message_id = input->source_message_id;
    ctx.event_id = input->event_id;
    return claw_session_mgr_build_session_id(&ctx, session_id, session_id_size, out_len);
}

static uint32_t claw_agent_mgr_next_request_id_locked(void)
{
    return s_mgr.next_request_id++;
}

static esp_err_t claw_agent_mgr_submit_to_agent(claw_agent_mgr_agent_t *agent,
                                                const claw_core_request_t *request,
                                                uint32_t timeout_ms)
{
    esp_err_t err;

    if (!agent || !agent->core || !request) {
        return ESP_ERR_INVALID_STATE;
    }
    err = claw_core_submit(agent->core, request, timeout_ms);
    if (err == ESP_OK) {
        agent->last_request_id = request->request_id;
        agent->status = CLAW_AGENT_MGR_STATUS_RUNNING;
        agent->last_error[0] = '\0';
    } else {
        agent->status = CLAW_AGENT_MGR_STATUS_ERROR;
        snprintf(agent->last_error, sizeof(agent->last_error), "%s", esp_err_to_name(err));
    }

    return err;
}

static void claw_agent_mgr_completion_observer(const claw_core_completion_summary_t *summary,
                                               void *user_ctx)
{
    claw_agent_mgr_agent_t *agent = (claw_agent_mgr_agent_t *)user_ctx;
    claw_agent_mgr_agent_t *root = NULL;
    claw_core_request_t request = {0};
    char text[CLAW_AGENT_MGR_NOTIFY_TEXT_SIZE];
    static const char truncated_suffix[] = "\n[truncated]\n</subagent_completed>";
    int written;

    if (!summary || !agent || !agent->parent_session_id[0]) {
        return;
    }

    written = snprintf(text,
                       sizeof(text),
                       "<subagent_completed agent_id=\"%s\" request_id=\"%" PRIu32 "\">\n%s\n</subagent_completed>",
                       agent->agent_id,
                       summary->request_id,
                       summary->final_text ? summary->final_text : "");
    if (written < 0) {
        return;
    }
    if ((size_t)written >= sizeof(text)) {
        size_t suffix_len = strlen(truncated_suffix);
        if (suffix_len + 1U < sizeof(text)) {
            size_t p = sizeof(text) - suffix_len - 1U;
            /* Back off so the suffix never splices into the middle of a
             * multi-byte UTF-8 sequence, which would emit invalid bytes. */
            while (p > 0 && ((unsigned char)text[p] & 0xC0) == 0x80) {
                p--;
            }
            strlcpy(text + p, truncated_suffix, sizeof(text) - p);
        }
    }

    claw_agent_mgr_lock();
    if (agent->closing) {
        claw_agent_mgr_unlock();
        return;
    }
    agent->status = CLAW_AGENT_MGR_STATUS_IDLE;
    agent->last_request_id = summary->request_id;
    root = claw_agent_mgr_find_locked(CLAW_AGENT_MGR_ROOT_AGENT_ID);
    if (root && root->core) {
        request.request_id = claw_agent_mgr_next_request_id_locked();
        request.flags = CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE |
                        CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE |
                        CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT;
        request.session_id = agent->parent_session_id;
        request.user_text = text;
        request.source_channel = agent->parent_source_channel;
        request.source_chat_id = agent->parent_source_chat_id;
        request.source_cap = "cap_agent_mgr";
        request.target_channel = agent->parent_target_channel;
        request.target_chat_id = agent->parent_target_chat_id;
        (void)claw_agent_mgr_submit_to_agent(root, &request, 1000);
    }
    claw_agent_mgr_unlock();
}

esp_err_t claw_agent_mgr_submit_root(const claw_agent_mgr_root_input_t *input,
                                     uint32_t timeout_ms)
{
    claw_agent_mgr_agent_t *root = NULL;
    claw_core_request_t request = {0};
    char session_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t session_id_len = 0;
    esp_err_t err;

    if (!input || !input->user_text || !input->user_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_agent_mgr_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    err = claw_agent_mgr_build_root_session_id(input,
                                               session_id,
                                               sizeof(session_id),
                                               &session_id_len);
    if (err != ESP_OK) {
        return err;
    }

    claw_agent_mgr_lock();
    root = claw_agent_mgr_find_locked(CLAW_AGENT_MGR_ROOT_AGENT_ID);
    if (!root || !root->core) {
        claw_agent_mgr_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    request.request_id = input->request_id ? input->request_id : claw_agent_mgr_next_request_id_locked();
    request.flags = input->flags;
    request.session_id = session_id_len > 0 ? session_id : NULL;
    request.user_text = input->user_text;
    request.source_channel = input->source_channel;
    request.source_chat_id = input->source_chat_id;
    request.source_sender_id = input->source_sender_id;
    request.source_message_id = input->source_message_id;
    request.source_cap = input->source_cap;
    request.target_channel = input->target_channel;
    request.target_chat_id = input->target_chat_id;
    err = claw_agent_mgr_submit_to_agent(root, &request, timeout_ms);
    claw_agent_mgr_unlock();
    return err;
}

esp_err_t claw_agent_mgr_submit_root_text(const char *text,
                                          const char *session_id,
                                          uint32_t flags,
                                          uint32_t timeout_ms,
                                          uint32_t *out_request_id)
{
    claw_agent_mgr_agent_t *root = NULL;
    claw_core_request_t request = {0};
    esp_err_t err;

    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_agent_mgr_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    claw_agent_mgr_lock();
    root = claw_agent_mgr_find_locked(CLAW_AGENT_MGR_ROOT_AGENT_ID);
    if (!root || !root->core) {
        claw_agent_mgr_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    request.request_id = claw_agent_mgr_next_request_id_locked();
    request.flags = flags;
    request.session_id = (session_id && session_id[0]) ? session_id : NULL;
    request.user_text = text;
    err = claw_agent_mgr_submit_to_agent(root, &request, timeout_ms);
    if (err == ESP_OK && out_request_id) {
        *out_request_id = request.request_id;
    }
    claw_agent_mgr_unlock();
    return err;
}

esp_err_t claw_agent_mgr_receive_root_for(uint32_t request_id,
                                          claw_core_response_t *response,
                                          uint32_t timeout_ms)
{
    claw_core_handle_t core = claw_agent_mgr_get_root_core();

    if (!core) {
        return ESP_ERR_INVALID_STATE;
    }
    return claw_core_receive_for(core, request_id, response, timeout_ms);
}

static bool claw_agent_mgr_ctx_is_root_agent(const claw_cap_call_context_t *ctx)
{
    return ctx && (ctx->caller == CLAW_CAP_CALLER_ROOT_AGENT ||
                   ctx->caller == CLAW_CAP_CALLER_AGENT);
}

static esp_err_t claw_agent_mgr_start_subagent_locked(claw_agent_mgr_agent_t *agent)
{
    if (!agent) {
        return ESP_ERR_INVALID_ARG;
    }
    if (agent->core) {
        return ESP_OK;
    }
    if (agent->closing) {
        return ESP_ERR_INVALID_STATE;
    }
    return claw_agent_mgr_start_agent_core(agent);
}

esp_err_t claw_agent_mgr_spawn_subagent(const claw_cap_call_context_t *parent_ctx,
                                        const char *prompt,
                                        const char *agent_type,
                                        bool background,
                                        char *out_agent_id,
                                        size_t out_agent_id_size)
{
    claw_agent_mgr_agent_t *agent = NULL;
    claw_core_request_t request = {0};
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t agent_id_len = 0;
    esp_err_t err;

    if (!claw_agent_mgr_ctx_is_root_agent(parent_ctx) ||
            !parent_ctx->session_id || !parent_ctx->session_id[0] ||
            !prompt || !prompt[0] || !out_agent_id || out_agent_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_session_mgr_alloc_subagent_session_id(parent_ctx->session_id,
                                                     agent_id,
                                                     sizeof(agent_id),
                                                     &agent_id_len);
    if (err != ESP_OK) {
        return err;
    }

    claw_agent_mgr_lock();
    agent = claw_agent_mgr_alloc_locked(CLAW_AGENT_MGR_ROLE_SUBAGENT);
    if (!agent) {
        claw_agent_mgr_unlock();
        return ESP_ERR_NO_MEM;
    }
    memset(agent, 0, sizeof(*agent));
    agent->occupied = true;
    agent->role = CLAW_AGENT_MGR_ROLE_SUBAGENT;
    agent->status = CLAW_AGENT_MGR_STATUS_RUNNING;
    agent->background = background;
    strlcpy(agent->agent_id, agent_id, sizeof(agent->agent_id));
    strlcpy(agent->session_id, agent_id, sizeof(agent->session_id));
    strlcpy(agent->parent_session_id, parent_ctx->session_id, sizeof(agent->parent_session_id));
    strlcpy(agent->parent_source_channel, parent_ctx->channel ? parent_ctx->channel : "",
            sizeof(agent->parent_source_channel));
    strlcpy(agent->parent_source_chat_id, parent_ctx->chat_id ? parent_ctx->chat_id : "",
            sizeof(agent->parent_source_chat_id));
    strlcpy(agent->parent_target_channel,
            parent_ctx->target_channel ? parent_ctx->target_channel :
            (parent_ctx->channel ? parent_ctx->channel : ""),
            sizeof(agent->parent_target_channel));
    strlcpy(agent->parent_target_chat_id,
            parent_ctx->target_chat_id ? parent_ctx->target_chat_id :
            (parent_ctx->chat_id ? parent_ctx->chat_id : ""),
            sizeof(agent->parent_target_chat_id));
    strlcpy(agent->agent_type, agent_type && agent_type[0] ? agent_type : "subagent",
            sizeof(agent->agent_type));

    err = claw_agent_mgr_start_subagent_locked(agent);
    if (err == ESP_OK) {
        request.request_id = claw_agent_mgr_next_request_id_locked();
        request.flags = CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE;
        request.session_id = agent->session_id;
        request.user_text = prompt;
        request.source_channel = parent_ctx->channel;
        request.source_chat_id = parent_ctx->chat_id;
        request.source_cap = "cap_agent_mgr";
        err = claw_agent_mgr_submit_to_agent(agent, &request, 1000);
    }
    if (err != ESP_OK) {
        if (agent->core) {
            (void)claw_core_destroy(agent->core);
        }
        memset(agent, 0, sizeof(*agent));
        claw_agent_mgr_unlock();
        return err;
    }
    if (strlcpy(out_agent_id, agent_id, out_agent_id_size) >= out_agent_id_size) {
        claw_agent_mgr_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    claw_agent_mgr_unlock();
    ESP_LOGI(TAG, "Spawned subagent id=%s parent_session=%s", agent_id, parent_ctx->session_id);
    return ESP_OK;
}

static esp_err_t claw_agent_mgr_find_or_lazy_create_subagent_locked(const claw_cap_call_context_t *ctx,
                                                                    const char *agent_id,
                                                                    claw_agent_mgr_agent_t **out_agent)
{
    claw_agent_mgr_agent_t *agent = NULL;
    bool known = false;
    esp_err_t err;

    if (!ctx || !ctx->session_id || !ctx->session_id[0] || !agent_id || !out_agent) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_agent = NULL;
    agent = claw_agent_mgr_find_locked(agent_id);
    if (agent) {
        *out_agent = agent;
        return ESP_OK;
    }

    err = claw_session_mgr_subagent_id_is_known(ctx->session_id, agent_id, &known);
    if (err != ESP_OK) {
        return err;
    }
    if (!known) {
        return ESP_ERR_NOT_FOUND;
    }

    agent = claw_agent_mgr_alloc_locked(CLAW_AGENT_MGR_ROLE_SUBAGENT);
    if (!agent) {
        return ESP_ERR_NO_MEM;
    }
    memset(agent, 0, sizeof(*agent));
    agent->occupied = true;
    agent->role = CLAW_AGENT_MGR_ROLE_SUBAGENT;
    agent->status = CLAW_AGENT_MGR_STATUS_CLOSED;
    strlcpy(agent->agent_id, agent_id, sizeof(agent->agent_id));
    strlcpy(agent->session_id, agent_id, sizeof(agent->session_id));
    strlcpy(agent->parent_session_id, ctx->session_id, sizeof(agent->parent_session_id));
    strlcpy(agent->parent_source_channel, ctx->channel ? ctx->channel : "",
            sizeof(agent->parent_source_channel));
    strlcpy(agent->parent_source_chat_id, ctx->chat_id ? ctx->chat_id : "",
            sizeof(agent->parent_source_chat_id));
    strlcpy(agent->parent_target_channel,
            ctx->target_channel ? ctx->target_channel :
            (ctx->channel ? ctx->channel : ""),
            sizeof(agent->parent_target_channel));
    strlcpy(agent->parent_target_chat_id,
            ctx->target_chat_id ? ctx->target_chat_id :
            (ctx->chat_id ? ctx->chat_id : ""),
            sizeof(agent->parent_target_chat_id));
    strlcpy(agent->agent_type, "subagent", sizeof(agent->agent_type));
    *out_agent = agent;
    return ESP_OK;
}

esp_err_t claw_agent_mgr_send_subagent_input(const claw_cap_call_context_t *ctx,
                                             const char *agent_id,
                                             const char *input,
                                             bool interrupt)
{
    claw_agent_mgr_agent_t *agent = NULL;
    claw_core_request_t request = {0};
    esp_err_t err;

    if (!claw_agent_mgr_ctx_is_root_agent(ctx) || !input || !input[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_agent_mgr_lock();
    err = claw_agent_mgr_find_or_lazy_create_subagent_locked(ctx, agent_id, &agent);
    if (err == ESP_OK) {
        err = claw_agent_mgr_start_subagent_locked(agent);
    }
    if (err == ESP_OK) {
        request.request_id = claw_agent_mgr_next_request_id_locked();
        request.flags = CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE |
                        (interrupt ? CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT : 0);
        request.session_id = agent->session_id;
        request.user_text = input;
        request.source_channel = ctx->channel;
        request.source_chat_id = ctx->chat_id;
        request.source_cap = "cap_agent_mgr";
        err = claw_agent_mgr_submit_to_agent(agent, &request, 1000);
    }
    claw_agent_mgr_unlock();
    return err;
}

esp_err_t claw_agent_mgr_inspect_agent(const claw_cap_call_context_t *ctx,
                                       const char *agent_id,
                                       claw_agent_mgr_agent_info_t *out_info)
{
    claw_agent_mgr_agent_t *agent = NULL;
    esp_err_t err = ESP_OK;

    if (!claw_agent_mgr_ctx_is_root_agent(ctx) || !agent_id || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_agent_mgr_lock();
    agent = claw_agent_mgr_find_locked(agent_id);
    if (!agent && strcmp(agent_id, CLAW_AGENT_MGR_ROOT_AGENT_ID) != 0) {
        err = claw_agent_mgr_find_or_lazy_create_subagent_locked(ctx, agent_id, &agent);
        if (err != ESP_OK) {
            claw_agent_mgr_unlock();
            return err;
        }
    }
    if (!agent) {
        claw_agent_mgr_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    claw_agent_mgr_fill_info_locked(agent, out_info);
    claw_agent_mgr_unlock();
    return ESP_OK;
}

static void claw_agent_mgr_fill_info_locked(const claw_agent_mgr_agent_t *agent,
                                            claw_agent_mgr_agent_info_t *out_info)
{
    memset(out_info, 0, sizeof(*out_info));
    strlcpy(out_info->agent_id, agent->agent_id, sizeof(out_info->agent_id));
    strlcpy(out_info->session_id, agent->session_id, sizeof(out_info->session_id));
    strlcpy(out_info->parent_session_id, agent->parent_session_id, sizeof(out_info->parent_session_id));
    strlcpy(out_info->agent_type, agent->agent_type, sizeof(out_info->agent_type));
    out_info->status = agent->status;
    out_info->phase = agent->core ? claw_core_get_agent_loop_phase(agent->core) :
                      CLAW_CORE_AGENT_LOOP_PHASE_IDLE;
    out_info->last_request_id = agent->last_request_id;
    strlcpy(out_info->last_error, agent->last_error, sizeof(out_info->last_error));
}

esp_err_t claw_agent_mgr_list_agents(const claw_cap_call_context_t *ctx,
                                     claw_agent_mgr_agent_info_t *out_infos,
                                     size_t max_infos,
                                     size_t *out_count)
{
    char (*child_ids)[CLAW_SESSION_MGR_ID_SIZE] = NULL;
    size_t child_count = 0;
    esp_err_t err;

    if (!claw_agent_mgr_ctx_is_root_agent(ctx) || !ctx->session_id || !ctx->session_id[0] ||
            !out_infos || max_infos == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    child_ids = calloc(max_infos, sizeof(child_ids[0]));
    if (!child_ids) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_session_mgr_list_subagent_sessions(ctx->session_id, child_ids, max_infos, &child_count);
    if (err != ESP_OK) {
        free(child_ids);
        return err;
    }

    claw_agent_mgr_lock();
    for (size_t i = 0; i < child_count; i++) {
        claw_agent_mgr_agent_t *agent = claw_agent_mgr_find_locked(child_ids[i]);

        if (agent) {
            claw_agent_mgr_fill_info_locked(agent, &out_infos[i]);
        } else {
            /* Persisted child with no live runtime: report as closed. */
            memset(&out_infos[i], 0, sizeof(out_infos[i]));
            strlcpy(out_infos[i].agent_id, child_ids[i], sizeof(out_infos[i].agent_id));
            strlcpy(out_infos[i].session_id, child_ids[i], sizeof(out_infos[i].session_id));
            strlcpy(out_infos[i].parent_session_id, ctx->session_id,
                    sizeof(out_infos[i].parent_session_id));
            strlcpy(out_infos[i].agent_type, "subagent", sizeof(out_infos[i].agent_type));
            out_infos[i].status = CLAW_AGENT_MGR_STATUS_CLOSED;
            out_infos[i].phase = CLAW_CORE_AGENT_LOOP_PHASE_IDLE;
        }
    }
    claw_agent_mgr_unlock();

    *out_count = child_count;
    free(child_ids);
    return ESP_OK;
}

esp_err_t claw_agent_mgr_close_agent(const claw_cap_call_context_t *ctx,
                                     const char *agent_id)
{
    claw_agent_mgr_agent_t *agent = NULL;
    claw_core_handle_t core = NULL;
    esp_err_t err = ESP_OK;

    if (!claw_agent_mgr_ctx_is_root_agent(ctx) ||
            !agent_id || strcmp(agent_id, CLAW_AGENT_MGR_ROOT_AGENT_ID) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_agent_mgr_lock();
    agent = claw_agent_mgr_find_locked(agent_id);
    if (!agent) {
        claw_agent_mgr_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    core = agent->core;
    if (!core) {
        agent->status = CLAW_AGENT_MGR_STATUS_CLOSED;
        claw_agent_mgr_unlock();
        return ESP_OK;
    }
    agent->core = NULL;
    agent->status = CLAW_AGENT_MGR_STATUS_CLOSED;
    agent->closing = true;
    claw_agent_mgr_unlock();

    err = claw_core_destroy(core);

    claw_agent_mgr_lock();
    agent = claw_agent_mgr_find_locked(agent_id);
    if (agent && agent->closing && !agent->core) {
        agent->closing = false;
        if (err != ESP_OK) {
            agent->status = CLAW_AGENT_MGR_STATUS_ERROR;
            snprintf(agent->last_error, sizeof(agent->last_error), "%s", esp_err_to_name(err));
        } else {
            agent->status = CLAW_AGENT_MGR_STATUS_CLOSED;
        }
    }
    claw_agent_mgr_unlock();
    return err;
}

esp_err_t claw_agent_mgr_delete_agent(const claw_cap_call_context_t *ctx,
                                      const char *agent_id)
{
    claw_agent_mgr_agent_t *agent = NULL;
    claw_core_handle_t core = NULL;
    char agent_id_copy[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char parent_session_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    bool known = false;
    bool deleted_any = false;
    esp_err_t err = ESP_OK;

    if (!claw_agent_mgr_ctx_is_root_agent(ctx) ||
            !ctx->session_id || !ctx->session_id[0] ||
            !agent_id || !agent_id[0] ||
            strcmp(agent_id, CLAW_AGENT_MGR_ROOT_AGENT_ID) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(agent_id_copy, agent_id, sizeof(agent_id_copy)) >= sizeof(agent_id_copy) ||
            strlcpy(parent_session_id, ctx->session_id, sizeof(parent_session_id)) >= sizeof(parent_session_id)) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = claw_session_mgr_subagent_id_is_known(parent_session_id, agent_id_copy, &known);
    if (err != ESP_OK) {
        return err;
    }
    if (!known) {
        return ESP_ERR_NOT_FOUND;
    }

    claw_agent_mgr_lock();
    agent = claw_agent_mgr_find_locked(agent_id_copy);
    if (agent && strcmp(agent->parent_session_id, parent_session_id) != 0) {
        claw_agent_mgr_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (agent && agent->closing) {
        claw_agent_mgr_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (agent) {
        agent->closing = true;
        if (agent->core) {
            core = agent->core;
            agent->core = NULL;
            agent->status = CLAW_AGENT_MGR_STATUS_CLOSED;
        }
    }
    claw_agent_mgr_unlock();

    if (core) {
        err = claw_core_destroy(core);
        claw_agent_mgr_lock();
        agent = claw_agent_mgr_find_locked(agent_id_copy);
        if (agent && agent->closing && !agent->core) {
            if (err != ESP_OK) {
                agent->closing = false;
                agent->status = CLAW_AGENT_MGR_STATUS_ERROR;
                snprintf(agent->last_error, sizeof(agent->last_error), "%s", esp_err_to_name(err));
            } else {
                agent->status = CLAW_AGENT_MGR_STATUS_CLOSED;
            }
        }
        claw_agent_mgr_unlock();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Delete failed to close subagent id=%s: %s",
                     agent_id_copy,
                     esp_err_to_name(err));
            return err;
        }
    }

    err = claw_session_mgr_delete_subagent_session(parent_session_id,
                                                   agent_id_copy,
                                                   &deleted_any);
    if (err != ESP_OK) {
        claw_agent_mgr_lock();
        agent = claw_agent_mgr_find_locked(agent_id_copy);
        if (agent && agent->closing && !agent->core) {
            agent->closing = false;
            agent->status = CLAW_AGENT_MGR_STATUS_ERROR;
            snprintf(agent->last_error, sizeof(agent->last_error), "%s", esp_err_to_name(err));
        }
        claw_agent_mgr_unlock();
        ESP_LOGE(TAG, "Delete subagent persistence failed id=%s parent_session=%s: %s",
                 agent_id_copy,
                 parent_session_id,
                 esp_err_to_name(err));
        return err;
    }

    claw_agent_mgr_lock();
    agent = claw_agent_mgr_find_locked(agent_id_copy);
    if (agent && !agent->core && agent->closing) {
        memset(agent, 0, sizeof(*agent));
    }
    claw_agent_mgr_unlock();

    ESP_LOGI(TAG,
             "Deleted subagent id=%s parent_session=%s history_deleted=%s",
             agent_id_copy,
             parent_session_id,
             deleted_any ? "true" : "false");
    return ESP_OK;
}

const char *claw_agent_mgr_status_to_string(claw_agent_mgr_status_t status)
{
    switch (status) {
    case CLAW_AGENT_MGR_STATUS_EMPTY:
        return "empty";
    case CLAW_AGENT_MGR_STATUS_RUNNING:
        return "running";
    case CLAW_AGENT_MGR_STATUS_IDLE:
        return "idle";
    case CLAW_AGENT_MGR_STATUS_COMPLETED:
        return "completed";
    case CLAW_AGENT_MGR_STATUS_CLOSED:
        return "closed";
    case CLAW_AGENT_MGR_STATUS_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
