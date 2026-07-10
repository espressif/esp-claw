/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "cap_agent_mgr.h"
#include "claw_agent_mgr.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_core_llm.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "wear_levelling.h"

#define TEST_FATFS_BASE_PATH       "/fatfs"
#define TEST_FATFS_PARTITION_LABEL "storage"
#define TEST_SESSION_ROOT          TEST_FATFS_BASE_PATH "/agent_mgr_sessions"
#define TEST_BACKEND_ID            "agent_mgr_runtime_test"
#define TEST_OUTPUT_SIZE           512
#define TEST_WAIT_MS               5000
#define TEST_CAPTURE_MAX           32
#define TEST_RECORD_MAX            160
#define TEST_CLOSE_HEAP_ALLOWANCE  4096
#define TEST_RACE_TASK_COUNT       5
#define TEST_RACE_ITERATIONS       12
#define TEST_HELPER_TASK_STACK     8192

static const char *TAG = "agent_mgr_test";

typedef enum {
    TEST_BACKEND_MODE_FINAL = 0,
    TEST_BACKEND_MODE_WAIT_FOR_ABORT,
} test_backend_mode_t;

typedef enum {
    TEST_CLOSE_DIRECT = 0,
    TEST_CLOSE_CAP,
} test_close_mode_t;

typedef struct {
    char model[32];
} test_backend_ctx_t;

typedef struct {
    char session_id[CLAW_SESSION_MGR_ID_SIZE];
    claw_core_context_record_type_t type;
    char *text;
} test_record_t;

static bool s_backend_registered;
static bool s_runtime_ready;
static size_t s_backend_call_count;
static char *s_captured_messages[TEST_CAPTURE_MAX];
static char *s_captured_system_prompts[TEST_CAPTURE_MAX];
static size_t s_capture_count;
static test_record_t s_records[TEST_RECORD_MAX];
static size_t s_record_count;
static SemaphoreHandle_t s_test_mutex;
static volatile test_backend_mode_t s_backend_mode = TEST_BACKEND_MODE_FINAL;
static SemaphoreHandle_t s_backend_entered_sem;
static volatile bool s_context_block_once;
static SemaphoreHandle_t s_context_entered_sem;
static SemaphoreHandle_t s_context_release_sem;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

typedef struct claw_event_stub claw_event_t;

esp_err_t claw_event_router_publish(const claw_event_t *event)
{
    (void)event;
    return ESP_OK;
}

static void remove_tree(const char *path)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    if (!path || !path[0]) {
        return;
    }
    dir = opendir(path);
    if (!dir) {
        (void)unlink(path);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char child_path[256];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name) >= (int)sizeof(child_path)) {
            continue;
        }
        remove_tree(child_path);
    }
    closedir(dir);
    (void)rmdir(path);
}

static char *test_strdup(const char *text)
{
    return text ? strdup(text) : NULL;
}

static bool test_lock_for(uint32_t timeout_ms)
{
    if (s_test_mutex) {
        return xSemaphoreTake(s_test_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }
    return true;
}

static void test_lock(void)
{
    TEST_ASSERT_TRUE(test_lock_for(TEST_WAIT_MS));
}

static void test_unlock(void)
{
    if (s_test_mutex) {
        (void)xSemaphoreGive(s_test_mutex);
    }
}

static void test_clear_captures(void)
{
    test_lock();
    for (size_t i = 0; i < s_capture_count; i++) {
        free(s_captured_messages[i]);
        s_captured_messages[i] = NULL;
        free(s_captured_system_prompts[i]);
        s_captured_system_prompts[i] = NULL;
    }
    s_capture_count = 0;
    s_backend_call_count = 0;
    test_unlock();
}

static void test_clear_records(void)
{
    test_lock();
    for (size_t i = 0; i < s_record_count; i++) {
        free(s_records[i].text);
        memset(&s_records[i], 0, sizeof(s_records[i]));
    }
    s_record_count = 0;
    test_unlock();
}

static void test_capture_messages(const claw_llm_chat_request_t *request)
{
    char *messages_json = NULL;
    char *system_prompt = NULL;

    if (!request) {
        return;
    }
    messages_json = cJSON_PrintUnformatted(request->messages);
    if (!messages_json) {
        return;
    }
    system_prompt = test_strdup(request->system_prompt ? request->system_prompt : "");
    if (!system_prompt) {
        free(messages_json);
        return;
    }
    if (!test_lock_for(TEST_WAIT_MS)) {
        free(system_prompt);
        free(messages_json);
        return;
    }
    if (s_capture_count >= TEST_CAPTURE_MAX) {
        test_unlock();
        free(system_prompt);
        free(messages_json);
        return;
    }
    s_captured_messages[s_capture_count] = messages_json;
    s_captured_system_prompts[s_capture_count] = system_prompt;
    s_capture_count++;
    test_unlock();
}

static bool test_capture_contains_text(const char *text)
{
    bool found = false;

    test_lock();
    for (size_t i = 0; i < s_capture_count; i++) {
        if (s_captured_messages[i] &&
                strstr(s_captured_messages[i], text)) {
            found = true;
            break;
        }
    }
    test_unlock();
    return found;
}

static bool test_system_capture_contains_text(const char *text)
{
    bool found = false;

    test_lock();
    for (size_t i = 0; i < s_capture_count; i++) {
        if (s_captured_system_prompts[i] &&
                strstr(s_captured_system_prompts[i], text)) {
            found = true;
            break;
        }
    }
    test_unlock();
    return found;
}

static size_t test_record_count_for(const char *session_id,
                                    claw_core_context_record_type_t type,
                                    const char *needle)
{
    size_t count = 0;

    test_lock();
    for (size_t i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].session_id, session_id) != 0 ||
                s_records[i].type != type) {
            continue;
        }
        if (!needle || (s_records[i].text && strstr(s_records[i].text, needle))) {
            count++;
        }
    }
    test_unlock();
    return count;
}

static esp_err_t test_persist_context(const claw_core_context_persist_batch_t *batch,
                                      void *user_ctx)
{
    (void)user_ctx;

    if (!batch || !batch->session_id || !batch->records) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!test_lock_for(TEST_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_record_count + batch->record_count > TEST_RECORD_MAX) {
        test_unlock();
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < batch->record_count; i++) {
        const claw_core_context_record_t *src = &batch->records[i];
        test_record_t *dst = &s_records[s_record_count++];

        strlcpy(dst->session_id, batch->session_id, sizeof(dst->session_id));
        dst->type = src->type;
        dst->text = test_strdup(src->text);
        if (src->text && !dst->text) {
            test_unlock();
            return ESP_ERR_NO_MEM;
        }
    }

    test_unlock();
    return ESP_OK;
}

static esp_err_t test_delete_session_history(const char *session_id,
                                             bool *out_deleted_any,
                                             void *user_ctx)
{
    bool deleted_any = false;

    (void)user_ctx;

    if (!session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_deleted_any) {
        *out_deleted_any = false;
    }
    if (!test_lock_for(TEST_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < s_record_count;) {
        if (strcmp(s_records[i].session_id, session_id) != 0) {
            i++;
            continue;
        }
        free(s_records[i].text);
        for (size_t j = i; j + 1 < s_record_count; j++) {
            s_records[j] = s_records[j + 1];
        }
        s_record_count--;
        memset(&s_records[s_record_count], 0, sizeof(s_records[s_record_count]));
        deleted_any = true;
    }
    test_unlock();
    if (out_deleted_any) {
        *out_deleted_any = deleted_any;
    }
    return ESP_OK;
}

static esp_err_t test_session_history_collect(const claw_core_request_t *request,
                                              claw_core_context_t *out_context,
                                              void *user_ctx)
{
    cJSON *messages = NULL;
    char *json = NULL;

    (void)user_ctx;

    if (!request || !out_context || !request->session_id) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_context_block_once) {
        s_context_block_once = false;
        if (s_context_entered_sem) {
            (void)xSemaphoreGive(s_context_entered_sem);
        }
        if (s_context_release_sem) {
            (void)xSemaphoreTake(s_context_release_sem, pdMS_TO_TICKS(TEST_WAIT_MS));
        }
    }

    messages = cJSON_CreateArray();
    if (!messages) {
        return ESP_ERR_NO_MEM;
    }

    if (!test_lock_for(TEST_WAIT_MS)) {
        cJSON_Delete(messages);
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < s_record_count; i++) {
        cJSON *msg = NULL;

        if (strcmp(s_records[i].session_id, request->session_id) != 0 ||
                (s_records[i].type != CLAW_CORE_CONTEXT_RECORD_USER &&
                 s_records[i].type != CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL)) {
            continue;
        }
        msg = cJSON_CreateObject();
        if (!msg ||
                !cJSON_AddStringToObject(msg,
                                         "role",
                                         s_records[i].type == CLAW_CORE_CONTEXT_RECORD_USER ?
                                         "user" : "assistant") ||
                !cJSON_AddStringToObject(msg, "content", s_records[i].text ? s_records[i].text : "")) {
            cJSON_Delete(msg);
            cJSON_Delete(messages);
            test_unlock();
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, msg);
    }
    test_unlock();

    json = cJSON_PrintUnformatted(messages);
    cJSON_Delete(messages);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_MESSAGES;
    out_context->content = json;
    return ESP_OK;
}

static esp_err_t test_backend_init(const claw_llm_runtime_config_t *config,
                                   const claw_llm_model_profile_t *profile,
                                   void **out_backend_ctx,
                                   char **out_error_message)
{
    test_backend_ctx_t *ctx = NULL;

    (void)profile;

    if (!out_backend_ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx = calloc(1, sizeof(test_backend_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(ctx->model, config && config->model ? config->model : "", sizeof(ctx->model));
    *out_backend_ctx = ctx;
    return ESP_OK;
}

static esp_err_t test_backend_chat(void *backend_ctx,
                                   const claw_llm_model_profile_t *profile,
                                   const claw_llm_chat_request_t *request,
                                   claw_llm_response_t *out_response,
                                   char **out_error_message)
{
    test_backend_ctx_t *ctx = (test_backend_ctx_t *)backend_ctx;
    char text[96];

    (void)profile;
    (void)out_error_message;

    if (!out_response) {
        return ESP_ERR_INVALID_ARG;
    }
    test_capture_messages(request);
    if (!test_lock_for(TEST_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    s_backend_call_count++;
    uint32_t call_count = (uint32_t)s_backend_call_count;
    test_unlock();

    if (s_backend_mode == TEST_BACKEND_MODE_WAIT_FOR_ABORT) {
        int64_t deadline = esp_timer_get_time() + (int64_t)TEST_WAIT_MS * 1000;

        if (s_backend_entered_sem) {
            (void)xSemaphoreGive(s_backend_entered_sem);
        }
        while (esp_timer_get_time() < deadline) {
            if (request && request->abort_flag && *request->abort_flag) {
                if (out_error_message) {
                    *out_error_message = test_strdup("test backend observed abort");
                }
                return ESP_ERR_INVALID_STATE;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (out_error_message) {
            *out_error_message = test_strdup("test backend timed out waiting for abort");
        }
        return ESP_ERR_TIMEOUT;
    }

    snprintf(text,
             sizeof(text),
             "final:%s:%u",
             ctx && ctx->model[0] ? ctx->model : "unknown",
             (unsigned)call_count);
    out_response->text = test_strdup(text);
    out_response->raw_message_json = test_strdup("{\"role\":\"assistant\",\"content\":\"final\"}");
    if (!out_response->text || !out_response->raw_message_json) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t test_backend_infer_media(void *backend_ctx,
                                          const claw_llm_model_profile_t *profile,
                                          const claw_llm_media_request_t *request,
                                          char **out_text,
                                          char **out_error_message)
{
    (void)backend_ctx;
    (void)profile;
    (void)request;
    (void)out_error_message;

    *out_text = test_strdup("media:ok");
    return *out_text ? ESP_OK : ESP_ERR_NO_MEM;
}

static void test_backend_deinit(void *backend_ctx)
{
    free(backend_ctx);
}

static const claw_llm_backend_vtable_t s_backend_vtable = {
    .init = test_backend_init,
    .chat = test_backend_chat,
    .infer_media = test_backend_infer_media,
    .deinit = test_backend_deinit,
};

static void ensure_backend_registered(void)
{
    if (s_backend_registered) {
        return;
    }

    const claw_llm_custom_backend_registration_t registration = {
        .id = TEST_BACKEND_ID,
        .vtable = &s_backend_vtable,
        .defaults = {
            .auth_type = "none",
            .chat_path = "",
            .max_tokens_field = "max_tokens",
        },
    };

    TEST_ASSERT_EQUAL(ESP_OK, claw_core_llm_register_custom_backend(&registration));
    s_backend_registered = true;
}

static void ensure_runtime_ready(void)
{
    static const claw_core_context_provider_t base_providers[] = {
        {
            .name = "test_session_history",
            .collect = test_session_history_collect,
        },
    };
    static const char *visible_groups[] = {
        "cap_agent_mgr",
    };
    const char *root_agent_id = NULL;
    claw_core_config_t core_config = {
        .api_key = "test-key",
        .backend_type = TEST_BACKEND_ID,
        .model = "agent-mgr-test",
        .base_url = "http://127.0.0.1",
        .auth_type = "none",
        .max_tokens_field = "max_tokens",
        .timeout_ms = TEST_WAIT_MS,
        .max_tokens = 128,
        .supports_tools = true,
        .system_prompt = "test root system prompt",
        .persist_context = test_persist_context,
        .task_stack_size = 8192,
        .task_priority = 4,
        .task_core = tskNO_AFFINITY,
        .max_tool_iterations = 4,
        .request_queue_len = 8,
        .response_queue_len = 8,
        .max_context_providers = 4,
    };
    claw_agent_mgr_config_t mgr_config = {
        .core_config = &core_config,
        .base_context_providers = base_providers,
        .base_context_provider_count = sizeof(base_providers) / sizeof(base_providers[0]),
    };

    if (s_runtime_ready) {
        return;
    }

    ensure_backend_registered();
    TEST_ASSERT_EQUAL(ESP_OK, claw_session_mgr_set_session_root_dir(TEST_SESSION_ROOT));
    TEST_ASSERT_EQUAL(ESP_OK, claw_session_mgr_set_delete_session_handler(test_delete_session_history, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, claw_cap_init());
    TEST_ASSERT_EQUAL(ESP_OK, cap_agent_mgr_register_group());
    TEST_ASSERT_EQUAL(ESP_OK, claw_cap_start_all());
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_cap_set_llm_visible_groups(visible_groups,
                                                      sizeof(visible_groups) / sizeof(visible_groups[0])));
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_init(&mgr_config));
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_create_root_agent(&root_agent_id));
    TEST_ASSERT_NOT_NULL(root_agent_id);
    TEST_ASSERT_EQUAL_STRING(CLAW_AGENT_MGR_ROOT_AGENT_ID, root_agent_id);
    s_runtime_ready = true;
}

static claw_cap_call_context_t test_root_ctx(const char *session_id)
{
    claw_cap_call_context_t ctx = {
        .session_id = session_id,
        .channel = "test",
        .chat_id = "room",
        .target_channel = "test",
        .target_chat_id = "room",
        .source_cap = "unity",
        .caller = CLAW_CAP_CALLER_ROOT_AGENT,
    };

    return ctx;
}

static void wait_for_record_text(const char *session_id,
                                 claw_core_context_record_type_t type,
                                 const char *needle)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)TEST_WAIT_MS * 1000;

    while (esp_timer_get_time() < deadline) {
        if (test_record_count_for(session_id, type, needle) > 0) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    TEST_FAIL_MESSAGE("timed out waiting for persisted context record");
}

static void wait_for_sem(SemaphoreHandle_t sem, const char *message)
{
    TEST_ASSERT_NOT_NULL(sem);
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE,
                              xSemaphoreTake(sem, pdMS_TO_TICKS(TEST_WAIT_MS)),
                              message);
}

static char *call_cap_and_dup_output(const char *name,
                                     const char *input_json,
                                     const claw_cap_call_context_t *ctx,
                                     esp_err_t expected_err)
{
    char *output = NULL;
    char *copy = NULL;
    esp_err_t err;

    output = calloc(1, TEST_OUTPUT_SIZE);
    TEST_ASSERT_NOT_NULL(output);
    err = claw_cap_call(name, input_json, ctx, output, TEST_OUTPUT_SIZE);
    TEST_ASSERT_EQUAL(expected_err, err);
    copy = test_strdup(output);
    free(output);
    TEST_ASSERT_NOT_NULL(copy);
    return copy;
}

static esp_err_t call_close_cap(const claw_cap_call_context_t *ctx, const char *agent_id)
{
    char input_json[192];
    char *output = NULL;
    esp_err_t err;

    if (!ctx || !agent_id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(input_json,
                 sizeof(input_json),
                 "{\"agent_id\":\"%s\"}",
                 agent_id) >= (int)sizeof(input_json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    output = calloc(1, TEST_OUTPUT_SIZE);
    if (!output) {
        return ESP_ERR_NO_MEM;
    }
    err = claw_cap_call("close_agent", input_json, ctx, output, TEST_OUTPUT_SIZE);
    free(output);
    return err;
}

static void close_agent_if_set(const claw_cap_call_context_t *ctx, const char *agent_id)
{
    if (ctx && agent_id && agent_id[0]) {
        (void)claw_agent_mgr_close_agent(ctx, agent_id);
    }
}

static void test_drain_runtime_records(void)
{
    vTaskDelay(pdMS_TO_TICKS(120));
    test_clear_records();
    test_clear_captures();
}

static size_t test_heap_after_cleanup(void)
{
    test_drain_runtime_records();
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

static void assert_heap_not_leaked(size_t before, size_t after, const char *message)
{
    intptr_t delta = (intptr_t)after - (intptr_t)before;

    ESP_LOGI(TAG,
             "%s heap before=%u after=%u delta=%d",
             message,
             (unsigned)before,
             (unsigned)after,
             (int)delta);
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(-(int)TEST_CLOSE_HEAP_ALLOWANCE,
                                             (int)delta,
                                             message);
}

typedef struct {
    claw_cap_call_context_t ctx;
    char agent_id[CLAW_SESSION_MGR_ID_SIZE];
    test_close_mode_t mode;
    SemaphoreHandle_t start_sem;
    SemaphoreHandle_t done_sem;
    esp_err_t result;
} test_close_task_ctx_t;

static void test_close_task(void *arg)
{
    test_close_task_ctx_t *task_ctx = (test_close_task_ctx_t *)arg;

    if (!task_ctx) {
        vTaskDelete(NULL);
        return;
    }
    (void)xSemaphoreTake(task_ctx->start_sem, portMAX_DELAY);
    if (task_ctx->mode == TEST_CLOSE_CAP) {
        task_ctx->result = call_close_cap(&task_ctx->ctx, task_ctx->agent_id);
    } else {
        task_ctx->result = claw_agent_mgr_close_agent(&task_ctx->ctx, task_ctx->agent_id);
    }
    (void)xSemaphoreGive(task_ctx->done_sem);
    vTaskDelete(NULL);
}

static void start_close_task(test_close_task_ctx_t *task_ctx,
                             const claw_cap_call_context_t *ctx,
                             const char *agent_id,
                             test_close_mode_t mode)
{
    BaseType_t ret;

    memset(task_ctx, 0, sizeof(*task_ctx));
    task_ctx->ctx = *ctx;
    strlcpy(task_ctx->agent_id, agent_id, sizeof(task_ctx->agent_id));
    task_ctx->mode = mode;
    task_ctx->start_sem = xSemaphoreCreateBinary();
    task_ctx->done_sem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(task_ctx->start_sem);
    TEST_ASSERT_NOT_NULL(task_ctx->done_sem);
    ret = xTaskCreate(test_close_task,
                      "test_close_agent",
                      TEST_HELPER_TASK_STACK,
                      task_ctx,
                      5,
                      NULL);
    TEST_ASSERT_EQUAL(pdPASS, ret);
}

static void assert_json_string_field(const char *json, const char *field, char *out, size_t out_size);

static void spawn_direct_agent(const claw_cap_call_context_t *ctx,
                               const char *prompt,
                               char *out_agent_id,
                               size_t out_agent_id_size)
{
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_spawn_subagent(ctx,
                                                   prompt,
                                                   "leak-test",
                                                   true,
                                                   out_agent_id,
                                                   out_agent_id_size));
    wait_for_record_text(out_agent_id, CLAW_CORE_CONTEXT_RECORD_USER, prompt);
}

static void spawn_cap_agent(const claw_cap_call_context_t *ctx,
                            const char *prompt,
                            char *out_agent_id,
                            size_t out_agent_id_size)
{
    char input_json[160];
    char *output = NULL;

    TEST_ASSERT_TRUE(snprintf(input_json,
                              sizeof(input_json),
                              "{\"prompt\":\"%s\",\"agent_type\":\"leak-test\",\"background\":true}",
                              prompt) < (int)sizeof(input_json));
    output = call_cap_and_dup_output("spawn_agent", input_json, ctx, ESP_OK);
    assert_json_string_field(output, "agent_id", out_agent_id, out_agent_id_size);
    free(output);
    wait_for_record_text(out_agent_id, CLAW_CORE_CONTEXT_RECORD_USER, prompt);
}

static void resume_agent_and_wait(const claw_cap_call_context_t *ctx,
                                  const char *agent_id,
                                  const char *input,
                                  bool interrupt)
{
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_send_subagent_input(ctx,
                                                        agent_id,
                                                        input,
                                                        interrupt));
    wait_for_record_text(agent_id, CLAW_CORE_CONTEXT_RECORD_USER, input);
}

static void close_agent_while_backend_hook_waits(const claw_cap_call_context_t *ctx,
                                                 const char *agent_id,
                                                 const char *input)
{
    s_backend_entered_sem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_backend_entered_sem);
    s_backend_mode = TEST_BACKEND_MODE_WAIT_FOR_ABORT;
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_send_subagent_input(ctx,
                                                        agent_id,
                                                        input,
                                                        false));
    wait_for_sem(s_backend_entered_sem, "backend hook was not reached");
    TEST_ASSERT_EQUAL(ESP_OK, call_close_cap(ctx, agent_id));
    s_backend_mode = TEST_BACKEND_MODE_FINAL;
    vSemaphoreDelete(s_backend_entered_sem);
    s_backend_entered_sem = NULL;
}

typedef struct {
    claw_cap_call_context_t ctx;
    char agent_id[CLAW_SESSION_MGR_ID_SIZE];
    SemaphoreHandle_t start_sem;
    SemaphoreHandle_t done_sem;
    volatile uint32_t unexpected_errors;
    volatile uint32_t ok_count;
} test_race_ctx_t;

static bool test_race_err_allowed(esp_err_t err)
{
    return err == ESP_OK ||
           err == ESP_ERR_INVALID_STATE ||
           err == ESP_ERR_NO_MEM ||
           err == ESP_ERR_TIMEOUT;
}

static void test_race_record_result(test_race_ctx_t *race_ctx, esp_err_t err)
{
    if (err == ESP_OK) {
        (void)__sync_fetch_and_add(&race_ctx->ok_count, 1);
    } else if (!test_race_err_allowed(err)) {
        (void)__sync_fetch_and_add(&race_ctx->unexpected_errors, 1);
    }
}

static void test_race_send_task(void *arg)
{
    test_race_ctx_t *race_ctx = (test_race_ctx_t *)arg;

    (void)xSemaphoreTake(race_ctx->start_sem, portMAX_DELAY);
    for (uint32_t i = 0; i < TEST_RACE_ITERATIONS; i++) {
        esp_err_t err = claw_agent_mgr_send_subagent_input(&race_ctx->ctx,
                                                           race_ctx->agent_id,
                                                           "race send",
                                                           (i % 2) == 0);
        test_race_record_result(race_ctx, err);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    (void)xSemaphoreGive(race_ctx->done_sem);
    vTaskDelete(NULL);
}

static void test_race_inspect_task(void *arg)
{
    test_race_ctx_t *race_ctx = (test_race_ctx_t *)arg;

    (void)xSemaphoreTake(race_ctx->start_sem, portMAX_DELAY);
    for (uint32_t i = 0; i < TEST_RACE_ITERATIONS; i++) {
        claw_agent_mgr_agent_info_t info = {0};
        esp_err_t err = claw_agent_mgr_inspect_agent(&race_ctx->ctx,
                                                     race_ctx->agent_id,
                                                     &info);
        test_race_record_result(race_ctx, err);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    (void)xSemaphoreGive(race_ctx->done_sem);
    vTaskDelete(NULL);
}

static void test_race_close_task(void *arg)
{
    test_race_ctx_t *race_ctx = (test_race_ctx_t *)arg;

    (void)xSemaphoreTake(race_ctx->start_sem, portMAX_DELAY);
    for (uint32_t i = 0; i < TEST_RACE_ITERATIONS; i++) {
        esp_err_t err = claw_agent_mgr_close_agent(&race_ctx->ctx,
                                                   race_ctx->agent_id);
        test_race_record_result(race_ctx, err);
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    (void)xSemaphoreGive(race_ctx->done_sem);
    vTaskDelete(NULL);
}

static void start_race_task(TaskFunction_t task_fn,
                            const char *task_name,
                            test_race_ctx_t *race_ctx)
{
    BaseType_t ret = xTaskCreate(task_fn,
                                 task_name,
                                 TEST_HELPER_TASK_STACK,
                                 race_ctx,
                                 5,
                                 NULL);

    TEST_ASSERT_EQUAL(pdPASS, ret);
}

static void assert_json_string_field(const char *json, const char *field, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *item = NULL;

    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE(cJSON_IsObject(root));
    item = cJSON_GetObjectItemCaseSensitive(root, field);
    TEST_ASSERT_TRUE(cJSON_IsString(item));
    TEST_ASSERT_NOT_NULL(item->valuestring);
    TEST_ASSERT_TRUE(strlcpy(out, item->valuestring, out_size) < out_size);
    cJSON_Delete(root);
}

TEST_CASE("session manager builds root ids without legacy prefix and persists subagent suffixes",
          "[claw_agent_mgr][session]")
{
    claw_session_build_context_t chat = {
        .agent_id = 0,
        .session_policy = CLAW_SESSION_POLICY_CHAT,
        .source_channel = "feishu",
        .chat_id = "room42",
    };
    claw_session_build_context_t trigger = {
        .agent_id = 0,
        .session_policy = CLAW_SESSION_POLICY_TRIGGER,
        .source_cap = "sensor",
        .event_id = "evt9",
    };
    char session_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char child_a[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char child_b[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t len = 0;
    bool known = false;

    ensure_runtime_ready();

    TEST_ASSERT_EQUAL(ESP_OK, claw_session_mgr_build_session_id(&chat, session_id, sizeof(session_id), &len));
    TEST_ASSERT_EQUAL_STRING("chat:feishu:room42:default_01", session_id);
    TEST_ASSERT_NULL(strstr(session_id, "agent:0:"));

    TEST_ASSERT_EQUAL(ESP_OK, claw_session_mgr_build_session_id(&trigger, session_id, sizeof(session_id), &len));
    TEST_ASSERT_EQUAL_STRING("trigger:sensor:evt9", session_id);
    TEST_ASSERT_NULL(strstr(session_id, "agent:0:"));

    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_session_mgr_alloc_subagent_session_id("chat:feishu:room42:default_01",
                                                                 child_a,
                                                                 sizeof(child_a),
                                                                 &len));
    TEST_ASSERT_EQUAL_STRING("chat:feishu:room42:default_01:subagent_01", child_a);
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_session_mgr_alloc_subagent_session_id("chat:feishu:room42:default_01",
                                                                 child_b,
                                                                 sizeof(child_b),
                                                                 &len));
    TEST_ASSERT_EQUAL_STRING("chat:feishu:room42:default_01:subagent_02", child_b);
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_session_mgr_subagent_id_is_known("chat:feishu:room42:default_01",
                                                           child_a,
                                                           &known));
    TEST_ASSERT_TRUE(known);
}

TEST_CASE("root-only agent manager tools are visible to root and rejected for subagents",
          "[claw_agent_mgr][cap]")
{
    claw_cap_call_context_t root_ctx = test_root_ctx("chat:cap:root");
    claw_cap_call_context_t sub_ctx = root_ctx;
    char *root_tools = NULL;
    char *sub_tools = NULL;
    char *output = NULL;

    ensure_runtime_ready();
    sub_ctx.caller = CLAW_CAP_CALLER_SUB_AGENT;

    root_tools = claw_cap_build_llm_tools_json(&root_ctx, false);
    sub_tools = claw_cap_build_llm_tools_json(&sub_ctx, false);
    TEST_ASSERT_NOT_NULL(root_tools);
    TEST_ASSERT_NOT_NULL(sub_tools);
    TEST_ASSERT_NOT_NULL(strstr(root_tools, "spawn_agent"));
    TEST_ASSERT_NOT_NULL(strstr(root_tools, "send_input"));
    TEST_ASSERT_NOT_NULL(strstr(root_tools, "inspect_agent"));
    TEST_ASSERT_NOT_NULL(strstr(root_tools, "list_agents"));
    TEST_ASSERT_NOT_NULL(strstr(root_tools, "close_agent"));
    TEST_ASSERT_NOT_NULL(strstr(root_tools, "delete_agent"));
    TEST_ASSERT_NULL(strstr(root_tools, "wait_agent"));
    TEST_ASSERT_NULL(strstr(sub_tools, "spawn_agent"));
    TEST_ASSERT_NULL(strstr(sub_tools, "send_input"));
    TEST_ASSERT_NULL(strstr(sub_tools, "inspect_agent"));
    TEST_ASSERT_NULL(strstr(sub_tools, "list_agents"));
    TEST_ASSERT_NULL(strstr(sub_tools, "close_agent"));
    TEST_ASSERT_NULL(strstr(sub_tools, "delete_agent"));

    output = call_cap_and_dup_output("spawn_agent",
                                     "{\"prompt\":\"should be rejected\"}",
                                     &sub_ctx,
                                     ESP_ERR_INVALID_STATE);
    TEST_ASSERT_NOT_NULL(output);
    TEST_ASSERT_NOT_NULL(strstr(output, "not exposed"));

    free(root_tools);
    free(sub_tools);
    free(output);
}

TEST_CASE("root text submit returns response through manager facade",
          "[claw_agent_mgr][root]")
{
    claw_core_response_t response = {0};
    uint32_t request_id = 0;

    ensure_runtime_ready();
    test_clear_captures();

    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_submit_root_text("root hello",
                                                     "chat:root:facade",
                                                     0,
                                                     1000,
                                                     &request_id));
    TEST_ASSERT_NOT_EQUAL(0, request_id);
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_receive_root_for(request_id,
                                                     &response,
                                                     TEST_WAIT_MS));
    TEST_ASSERT_EQUAL(request_id, response.request_id);
    TEST_ASSERT_EQUAL(CLAW_CORE_RESPONSE_STATUS_OK, response.status);
    TEST_ASSERT_NOT_NULL(response.text);
    TEST_ASSERT_NOT_NULL(strstr(response.text, "final:agent-mgr-test"));
    wait_for_record_text("chat:root:facade", CLAW_CORE_CONTEXT_RECORD_USER, "root hello");
    TEST_ASSERT_TRUE(test_system_capture_contains_text("test root system prompt"));
    TEST_ASSERT_TRUE(test_system_capture_contains_text("# Root Agent Role"));
    TEST_ASSERT_TRUE(test_system_capture_contains_text("Agent type: root."));
    TEST_ASSERT_TRUE(test_system_capture_contains_text("Selected agent_type: root"));
    TEST_ASSERT_FALSE(test_system_capture_contains_text("# Subagent Role"));
    claw_core_response_free(&response);
}

TEST_CASE("spawn send inspect and close subagent through root-only capability",
          "[claw_agent_mgr][runtime]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:runtime:parent");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char *output = NULL;
    claw_agent_mgr_agent_info_t info = {0};

    ensure_runtime_ready();
    test_clear_captures();

    output = call_cap_and_dup_output("spawn_agent",
                                     "{\"prompt\":\"child initial task\",\"agent_type\":\"research\",\"background\":true}",
                                     &ctx,
                                     ESP_OK);
    assert_json_string_field(output, "agent_id", agent_id, sizeof(agent_id));
    free(output);
    TEST_ASSERT_EQUAL_STRING("chat:runtime:parent:subagent_01", agent_id);

    wait_for_record_text(agent_id, CLAW_CORE_CONTEXT_RECORD_USER, "child initial task");
    TEST_ASSERT_TRUE(test_system_capture_contains_text("test root system prompt"));
    TEST_ASSERT_TRUE(test_system_capture_contains_text("# Subagent Role"));
    TEST_ASSERT_TRUE(test_system_capture_contains_text("Agent type: research."));
    TEST_ASSERT_TRUE(test_system_capture_contains_text("Selected agent_type: research"));
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_inspect_agent(&ctx, agent_id, &info));
    TEST_ASSERT_EQUAL_STRING(agent_id, info.agent_id);
    TEST_ASSERT_EQUAL_STRING(agent_id, info.session_id);
    TEST_ASSERT_EQUAL_STRING("chat:runtime:parent", info.parent_session_id);
    TEST_ASSERT_EQUAL_STRING("research", info.agent_type);
    TEST_ASSERT_TRUE(info.status == CLAW_AGENT_MGR_STATUS_RUNNING ||
                     info.status == CLAW_AGENT_MGR_STATUS_IDLE);

    output = call_cap_and_dup_output("send_input",
                                     "{\"agent_id\":\"chat:runtime:parent:subagent_01\",\"input\":\"follow up\",\"interrupt\":false}",
                                     &ctx,
                                     ESP_OK);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"status\":\"submitted\""));
    free(output);
    wait_for_record_text(agent_id, CLAW_CORE_CONTEXT_RECORD_USER, "follow up");
    TEST_ASSERT_TRUE(test_capture_contains_text("child initial task"));

    output = call_cap_and_dup_output("inspect_agent",
                                     "{\"agent_id\":\"chat:runtime:parent:subagent_01\"}",
                                     &ctx,
                                     ESP_OK);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"agent_type\":\"research\""));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"last_request_id\":"));
    free(output);

    output = call_cap_and_dup_output("close_agent",
                                     "{\"agent_id\":\"chat:runtime:parent:subagent_01\"}",
                                     &ctx,
                                     ESP_OK);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"status\":\"closed\""));
    free(output);
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_inspect_agent(&ctx, agent_id, &info));
    TEST_ASSERT_EQUAL(CLAW_AGENT_MGR_STATUS_CLOSED, info.status);
}

TEST_CASE("closed known subagent can be resumed and invalid parent scoped ids fail",
          "[claw_agent_mgr][runtime]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:resume:parent");
    claw_cap_call_context_t other_ctx = test_root_ctx("chat:resume:other");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char evict_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char *output = NULL;
    claw_agent_mgr_agent_info_t info = {0};
    bool known = false;

    ensure_runtime_ready();
    test_clear_captures();

    output = call_cap_and_dup_output("spawn_agent",
                                     "{\"prompt\":\"resume original\"}",
                                     &ctx,
                                     ESP_OK);
    assert_json_string_field(output, "agent_id", agent_id, sizeof(agent_id));
    free(output);
    wait_for_record_text(agent_id, CLAW_CORE_CONTEXT_RECORD_USER, "resume original");
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, agent_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_session_mgr_subagent_id_is_known(ctx.session_id, agent_id, &known));
    TEST_ASSERT_TRUE(known);

    spawn_direct_agent(&other_ctx, "reuse closed runtime slot", evict_id, sizeof(evict_id));
    close_agent_if_set(&other_ctx, evict_id);
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_inspect_agent(&ctx, agent_id, &info));
    TEST_ASSERT_EQUAL_STRING(agent_id, info.agent_id);
    TEST_ASSERT_EQUAL_STRING(agent_id, info.session_id);
    TEST_ASSERT_EQUAL_STRING(ctx.session_id, info.parent_session_id);
    TEST_ASSERT_EQUAL(CLAW_AGENT_MGR_STATUS_CLOSED, info.status);

    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_send_subagent_input(&ctx,
                                                        agent_id,
                                                        "resume after close",
                                                        true));
    wait_for_record_text(agent_id, CLAW_CORE_CONTEXT_RECORD_USER, "resume after close");
    TEST_ASSERT_TRUE(test_capture_contains_text("resume original"));

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      claw_agent_mgr_send_subagent_input(&other_ctx,
                                                        agent_id,
                                                        "wrong parent",
                                                        false));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      claw_agent_mgr_send_subagent_input(&ctx,
                                                        "chat:resume:parent:subagent_99",
                                                        "unknown child",
                                                        false));
    close_agent_if_set(&ctx, agent_id);
}

TEST_CASE("delete_agent removes subagent history and prevents lazy resume",
          "[claw_agent_mgr][runtime][delete]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:delete:parent");
    claw_cap_call_context_t other_ctx = test_root_ctx("chat:delete:other");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char *output = NULL;
    claw_agent_mgr_agent_info_t info = {0};
    bool known = false;

    ensure_runtime_ready();
    test_clear_captures();

    output = call_cap_and_dup_output("spawn_agent",
                                     "{\"prompt\":\"delete original\"}",
                                     &ctx,
                                     ESP_OK);
    assert_json_string_field(output, "agent_id", agent_id, sizeof(agent_id));
    free(output);
    wait_for_record_text(agent_id, CLAW_CORE_CONTEXT_RECORD_USER, "delete original");

    output = call_cap_and_dup_output("delete_agent",
                                     "{\"agent_id\":\"chat:delete:parent:subagent_01\"}",
                                     &ctx,
                                     ESP_OK);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"status\":\"deleted\""));
    free(output);

    TEST_ASSERT_EQUAL(ESP_OK, claw_session_mgr_subagent_id_is_known(ctx.session_id, agent_id, &known));
    TEST_ASSERT_FALSE(known);
    TEST_ASSERT_EQUAL_UINT32(0,
                             test_record_count_for(agent_id,
                                                   CLAW_CORE_CONTEXT_RECORD_USER,
                                                   "delete original"));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_agent_mgr_inspect_agent(&ctx, agent_id, &info));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      claw_agent_mgr_send_subagent_input(&ctx,
                                                        agent_id,
                                                        "resume after delete",
                                                        false));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      claw_agent_mgr_delete_agent(&ctx, agent_id));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      claw_agent_mgr_delete_agent(&other_ctx, agent_id));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      claw_agent_mgr_delete_agent(&ctx, CLAW_AGENT_MGR_ROOT_AGENT_ID));
}

TEST_CASE("child completion notification is injected into stored parent session",
          "[claw_agent_mgr][notification]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:notify:parent");
    claw_cap_call_context_t later_ctx = test_root_ctx("chat:notify:later");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    char *output = NULL;

    ensure_runtime_ready();
    test_clear_records();

    output = call_cap_and_dup_output("spawn_agent",
                                     "{\"prompt\":\"notify child\"}",
                                     &ctx,
                                     ESP_OK);
    assert_json_string_field(output, "agent_id", agent_id, sizeof(agent_id));
    free(output);
    wait_for_record_text("chat:notify:parent",
                         CLAW_CORE_CONTEXT_RECORD_USER,
                         "<subagent_completed");
    TEST_ASSERT_EQUAL(0,
                      test_record_count_for("chat:notify:later",
                                            CLAW_CORE_CONTEXT_RECORD_USER,
                                            "<subagent_completed"));

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
                      claw_agent_mgr_send_subagent_input(&later_ctx,
                                                        agent_id,
                                                        "must not reparent",
                                                        false));
    TEST_ASSERT_EQUAL(0,
                      test_record_count_for("chat:notify:later",
                                            CLAW_CORE_CONTEXT_RECORD_USER,
                                            "<subagent_completed"));
    close_agent_if_set(&ctx, agent_id);
}

TEST_CASE("direct close releases subagent core memory across resume cycles",
          "[claw_agent_mgr][leak]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:leak:direct");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t before = 0;
    size_t after = 0;
    claw_agent_mgr_agent_info_t info = {0};

    ensure_runtime_ready();
    spawn_direct_agent(&ctx, "direct leak warmup", agent_id, sizeof(agent_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, agent_id));

    before = test_heap_after_cleanup();
    resume_agent_and_wait(&ctx, agent_id, "direct leak resume 1", true);
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, agent_id));
    after = test_heap_after_cleanup();
    assert_heap_not_leaked(before, after, "direct close resume cycle leaked heap");

    before = after;
    resume_agent_and_wait(&ctx, agent_id, "direct leak resume 2", false);
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, agent_id));
    after = test_heap_after_cleanup();
    assert_heap_not_leaked(before, after, "direct close queued cycle leaked heap");

    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_inspect_agent(&ctx, agent_id, &info));
    TEST_ASSERT_EQUAL(CLAW_AGENT_MGR_STATUS_CLOSED, info.status);
}

TEST_CASE("cap close releases subagent core memory across resume cycles",
          "[claw_agent_mgr][leak][cap]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:leak:cap");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t before = 0;
    size_t after = 0;
    claw_agent_mgr_agent_info_t info = {0};

    ensure_runtime_ready();
    spawn_cap_agent(&ctx, "cap leak warmup", agent_id, sizeof(agent_id));
    TEST_ASSERT_EQUAL(ESP_OK, call_close_cap(&ctx, agent_id));

    before = test_heap_after_cleanup();
    resume_agent_and_wait(&ctx, agent_id, "cap leak resume 1", true);
    TEST_ASSERT_EQUAL(ESP_OK, call_close_cap(&ctx, agent_id));
    after = test_heap_after_cleanup();
    assert_heap_not_leaked(before, after, "cap close resume cycle leaked heap");

    before = after;
    resume_agent_and_wait(&ctx, agent_id, "cap leak resume 2", false);
    TEST_ASSERT_EQUAL(ESP_OK, call_close_cap(&ctx, agent_id));
    after = test_heap_after_cleanup();
    assert_heap_not_leaked(before, after, "cap close queued cycle leaked heap");

    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_inspect_agent(&ctx, agent_id, &info));
    TEST_ASSERT_EQUAL(CLAW_AGENT_MGR_STATUS_CLOSED, info.status);
}

TEST_CASE("close while context provider hook is active releases memory",
          "[claw_agent_mgr][leak][hook]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:leak:context-hook");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    test_close_task_ctx_t close_ctx = {0};
    size_t before = 0;
    size_t after = 0;

    ensure_runtime_ready();
    spawn_direct_agent(&ctx, "context hook warmup", agent_id, sizeof(agent_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, agent_id));
    before = test_heap_after_cleanup();

    s_context_entered_sem = xSemaphoreCreateBinary();
    s_context_release_sem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(s_context_entered_sem);
    TEST_ASSERT_NOT_NULL(s_context_release_sem);
    s_context_block_once = true;
    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_send_subagent_input(&ctx,
                                                        agent_id,
                                                        "context hook blocked",
                                                        false));
    wait_for_sem(s_context_entered_sem, "context provider hook was not reached");
    start_close_task(&close_ctx, &ctx, agent_id, TEST_CLOSE_DIRECT);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(close_ctx.start_sem));
    vTaskDelay(pdMS_TO_TICKS(80));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(s_context_release_sem));
    wait_for_sem(close_ctx.done_sem, "direct close from context hook timed out");
    TEST_ASSERT_EQUAL(ESP_OK, close_ctx.result);

    vSemaphoreDelete(close_ctx.start_sem);
    vSemaphoreDelete(close_ctx.done_sem);
    vSemaphoreDelete(s_context_entered_sem);
    vSemaphoreDelete(s_context_release_sem);
    s_context_entered_sem = NULL;
    s_context_release_sem = NULL;
    s_context_block_once = false;

    after = test_heap_after_cleanup();
    assert_heap_not_leaked(before, after, "context provider hook close leaked heap");
}

TEST_CASE("close while backend hook is active aborts worker and releases memory",
          "[claw_agent_mgr][leak][hook][cap]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:leak:backend-hook");
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t before = 0;
    size_t after = 0;

    ensure_runtime_ready();
    spawn_direct_agent(&ctx, "backend hook warmup", agent_id, sizeof(agent_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, agent_id));

    close_agent_while_backend_hook_waits(&ctx, agent_id, "backend hook abort warmup");
    before = test_heap_after_cleanup();

    close_agent_while_backend_hook_waits(&ctx, agent_id, "backend hook blocked");
    after = test_heap_after_cleanup();
    assert_heap_not_leaked(before, after, "backend hook cap close leaked heap");
}

TEST_CASE("concurrent send inspect and close access remains protected",
          "[claw_agent_mgr][race]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:race:parent");
    test_race_ctx_t race_ctx = {0};
    claw_agent_mgr_agent_info_t info = {0};

    ensure_runtime_ready();
    spawn_direct_agent(&ctx, "race warmup", race_ctx.agent_id, sizeof(race_ctx.agent_id));
    race_ctx.ctx = ctx;
    race_ctx.start_sem = xSemaphoreCreateCounting(TEST_RACE_TASK_COUNT, 0);
    race_ctx.done_sem = xSemaphoreCreateCounting(TEST_RACE_TASK_COUNT, 0);
    TEST_ASSERT_NOT_NULL(race_ctx.start_sem);
    TEST_ASSERT_NOT_NULL(race_ctx.done_sem);

    start_race_task(test_race_send_task, "race_send_a", &race_ctx);
    start_race_task(test_race_send_task, "race_send_b", &race_ctx);
    start_race_task(test_race_inspect_task, "race_inspect_a", &race_ctx);
    start_race_task(test_race_inspect_task, "race_inspect_b", &race_ctx);
    start_race_task(test_race_close_task, "race_close", &race_ctx);
    for (uint32_t i = 0; i < TEST_RACE_TASK_COUNT; i++) {
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(race_ctx.start_sem));
    }
    for (uint32_t i = 0; i < TEST_RACE_TASK_COUNT; i++) {
        wait_for_sem(race_ctx.done_sem, "race task timed out");
    }

    TEST_ASSERT_EQUAL_UINT32(0, race_ctx.unexpected_errors);
    TEST_ASSERT_GREATER_THAN_UINT32(0, race_ctx.ok_count);
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, race_ctx.agent_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_inspect_agent(&ctx, race_ctx.agent_id, &info));
    TEST_ASSERT_EQUAL(CLAW_AGENT_MGR_STATUS_CLOSED, info.status);
    vSemaphoreDelete(race_ctx.start_sem);
    vSemaphoreDelete(race_ctx.done_sem);
}

TEST_CASE("subagent capacity returns clear error and close preserves runtime capacity",
          "[claw_agent_mgr][capacity]")
{
    claw_cap_call_context_t ctx = test_root_ctx("chat:capacity:parent");
    char (*ids)[CLAW_SESSION_MGR_ID_SIZE] = NULL;
    char new_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    size_t created = 0;
    char *output = NULL;

    ensure_runtime_ready();
    ids = calloc(CONFIG_CLAW_AGENT_MGR_MAX_SUBAGENTS, CLAW_SESSION_MGR_ID_SIZE);
    TEST_ASSERT_NOT_NULL(ids);

    for (size_t i = 0; i < CONFIG_CLAW_AGENT_MGR_MAX_SUBAGENTS; i++) {
        output = call_cap_and_dup_output("spawn_agent",
                                         "{\"prompt\":\"capacity child\"}",
                                         &ctx,
                                         ESP_OK);
        assert_json_string_field(output, "agent_id", ids[created], sizeof(ids[created]));
        created++;
        free(output);
    }

    output = call_cap_and_dup_output("spawn_agent",
                                     "{\"prompt\":\"one too many\"}",
                                     &ctx,
                                     ESP_ERR_NO_MEM);
    TEST_ASSERT_NOT_NULL(strstr(output, "ESP_ERR_NO_MEM"));
    free(output);

    TEST_ASSERT_EQUAL_UINT32(CONFIG_CLAW_AGENT_MGR_MAX_SUBAGENTS, created);
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, ids[0]));

    output = call_cap_and_dup_output("spawn_agent",
                                     "{\"prompt\":\"capacity after close\"}",
                                     &ctx,
                                     ESP_OK);
    assert_json_string_field(output, "agent_id", new_id, sizeof(new_id));
    free(output);
    TEST_ASSERT_EQUAL(ESP_OK, claw_agent_mgr_close_agent(&ctx, new_id));

    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_agent_mgr_send_subagent_input(&ctx,
                                                        ids[0],
                                                        "capacity resume",
                                                        false));
    for (size_t i = 0; i < created; i++) {
        close_agent_if_set(&ctx, ids[i]);
    }
    free(ids);
}

void app_main(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 16,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(TEST_FATFS_BASE_PATH,
                                                     TEST_FATFS_PARTITION_LABEL,
                                                     &mount_config,
                                                     &s_wl_handle));
    s_test_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_test_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    remove_tree(TEST_SESSION_ROOT);
    ESP_LOGI(TAG, "Starting agent manager runtime tests");
    unity_run_menu();
}
