/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdbool.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_core.h"
#include "claw_core_llm.h"
#include "llm/claw_llm_http_transport.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_runner.h"

#define TEST_RECORD_MAX 64
#define TEST_CAPTURE_MAX 16
#define TEST_HTTP_PORT 18181
#define TEST_HTTP_URL_A "http://127.0.0.1:18181/slow-a"
#define TEST_HTTP_URL_B "http://127.0.0.1:18181/fast-b"
#define TEST_HTTP_TASK_STACK 4096
#define TEST_HTTP_TIMEOUT_MS 5000
#define TEST_HTTP_LEAK_THRESHOLD (-1024)
#define TEST_HTTP_LEAK_RUNS 3
#define TEST_CORE_LEAK_THRESHOLD (-1024)
#define TEST_CORE_LEAK_RUNS 5

typedef enum {
    TEST_BACKEND_FINAL = 0,
    TEST_BACKEND_HTTP_ABORT,
    TEST_BACKEND_AFTER_LLM_INTERRUPT,
    TEST_BACKEND_TOOL_INTERRUPT,
} test_backend_mode_t;

typedef struct {
    char session_id[32];
    claw_core_context_record_type_t type;
    char *text;
    char *message_json;
} test_record_t;

typedef struct {
    char model[48];
} test_backend_ctx_t;

typedef struct {
    int sock;
} test_http_conn_t;

typedef struct {
    const char *url;
    volatile bool *abort_flag;
    esp_err_t err;
    claw_llm_http_response_t response;
    char *error_message;
    SemaphoreHandle_t done_sem;
} test_http_client_task_t;

static test_record_t s_records[TEST_RECORD_MAX];
static size_t s_record_count;
static char *s_captured_messages[TEST_CAPTURE_MAX];
static size_t s_capture_count;
static char *s_isolate_a_messages;
static char *s_isolate_b_messages;
static test_backend_mode_t s_backend_mode;
static uint32_t s_active_request_id;
static char s_active_session_id[32];
static size_t s_backend_call_count;
static size_t s_tool_call_count;
static size_t s_backend_init_count;
static size_t s_backend_deinit_count;
static bool s_backend_registered;
static bool s_core_ready;
static bool s_saw_building_phase;
static bool s_saw_before_build_phase;
static bool s_saw_in_llm_phase;
static bool s_request_start_interrupt_enabled;
static bool s_interrupt_provider_enabled;
static bool s_interrupt_provider_done;
static bool s_fail_user_context_persist;
static esp_err_t s_queue_full_err = ESP_OK;
static uint32_t s_next_interrupt_request_id;
static claw_core_handle_t s_core;
static bool s_esp_netif_ready;
static bool s_esp_event_loop_ready;
static int s_http_listen_sock = -1;
static TaskHandle_t s_http_server_task;
static test_http_conn_t s_http_conns[2];
static SemaphoreHandle_t s_http_server_ready_sem;
static SemaphoreHandle_t s_http_slow_ready_sem;
static SemaphoreHandle_t s_http_slow_release_sem;

static char *test_dup_string(const char *text)
{
    return text ? strdup(text) : NULL;
}

static void test_clear_captures(void)
{
    for (size_t i = 0; i < s_capture_count; i++) {
        free(s_captured_messages[i]);
        s_captured_messages[i] = NULL;
    }
    s_capture_count = 0;
    free(s_isolate_a_messages);
    free(s_isolate_b_messages);
    s_isolate_a_messages = NULL;
    s_isolate_b_messages = NULL;
}

static void test_clear_records(void)
{
    for (size_t i = 0; i < s_record_count; i++) {
        free(s_records[i].text);
        free(s_records[i].message_json);
        memset(&s_records[i], 0, sizeof(s_records[i]));
    }
    s_record_count = 0;
}

static void test_reset_scenario(test_backend_mode_t mode, uint32_t request_id)
{
    test_clear_captures();
    s_backend_mode = mode;
    s_active_request_id = request_id;
    s_active_session_id[0] = '\0';
    s_backend_call_count = 0;
    s_tool_call_count = 0;
    s_saw_building_phase = false;
    s_saw_before_build_phase = false;
    s_saw_in_llm_phase = false;
    s_request_start_interrupt_enabled = false;
    s_interrupt_provider_enabled = false;
    s_interrupt_provider_done = false;
    s_fail_user_context_persist = false;
    s_queue_full_err = ESP_OK;
    s_next_interrupt_request_id = request_id + 10000;
}

static esp_err_t submit_user_interrupt(const char *session_id, const char *text)
{
    claw_core_request_t request = {
        .request_id = s_next_interrupt_request_id++,
        .flags = CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT,
        .session_id = session_id,
        .user_text = text,
    };

    return claw_core_submit(s_core, &request, 1000);
}

static esp_err_t test_persist_context(const claw_core_context_persist_batch_t *batch, void *user_ctx)
{
    (void)user_ctx;

    if (!batch || !batch->session_id || !batch->records) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_fail_user_context_persist) {
        for (size_t i = 0; i < batch->record_count; i++) {
            if (batch->records[i].type == CLAW_CORE_CONTEXT_RECORD_USER) {
                return ESP_FAIL;
            }
        }
    }
    if (s_record_count + batch->record_count > TEST_RECORD_MAX) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < batch->record_count; i++) {
        const claw_core_context_record_t *src = &batch->records[i];
        test_record_t *dst = &s_records[s_record_count++];

        strlcpy(dst->session_id, batch->session_id, sizeof(dst->session_id));
        dst->type = src->type;
        dst->text = test_dup_string(src->text);
        dst->message_json = test_dup_string(src->message_json);
        if ((src->text && !dst->text) || (src->message_json && !dst->message_json)) {
            return ESP_ERR_NO_MEM;
        }
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

    messages = cJSON_CreateArray();
    if (!messages) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_record_count; i++) {
        test_record_t *record = &s_records[i];

        if (strcmp(record->session_id, request->session_id) != 0) {
            continue;
        }
        if (record->type == CLAW_CORE_CONTEXT_RECORD_USER) {
            cJSON *msg = cJSON_CreateObject();

            if (!msg ||
                    !cJSON_AddStringToObject(msg, "role", "user") ||
                    !cJSON_AddStringToObject(msg, "content", record->text ? record->text : "")) {
                cJSON_Delete(msg);
                cJSON_Delete(messages);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(messages, msg);
        } else if (record->type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL) {
            cJSON *msg = cJSON_CreateObject();

            if (!msg ||
                    !cJSON_AddStringToObject(msg, "role", "assistant") ||
                    !cJSON_AddStringToObject(msg, "content", record->text ? record->text : "")) {
                cJSON_Delete(msg);
                cJSON_Delete(messages);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(messages, msg);
        } else if (record->type == CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL && record->message_json) {
            cJSON *msg = cJSON_Parse(record->message_json);

            if (!msg) {
                cJSON_Delete(messages);
                return ESP_FAIL;
            }
            cJSON_AddItemToArray(messages, msg);
        } else if (record->type == CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT && record->message_json) {
            cJSON *array = cJSON_Parse(record->message_json);

            if (!array || !cJSON_IsArray(array)) {
                cJSON_Delete(array);
                cJSON_Delete(messages);
                return ESP_FAIL;
            }
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, array) {
                cJSON *dup = cJSON_Duplicate(item, true);

                if (!dup) {
                    cJSON_Delete(array);
                    cJSON_Delete(messages);
                    return ESP_ERR_NO_MEM;
                }
                cJSON_AddItemToArray(messages, dup);
            }
            cJSON_Delete(array);
        }
    }

    if (cJSON_GetArraySize(messages) == 0) {
        cJSON_Delete(messages);
        return ESP_ERR_NOT_FOUND;
    }

    json = cJSON_PrintUnformatted(messages);
    cJSON_Delete(messages);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_MESSAGES;
    out_context->content = json;
    return ESP_OK;
}

static esp_err_t test_request_start(const claw_core_request_t *request, void *user_ctx)
{
    (void)user_ctx;

    if (!request || !s_request_start_interrupt_enabled ||
            request->request_id != s_active_request_id) {
        return ESP_OK;
    }

    s_saw_before_build_phase =
        claw_core_get_agent_loop_phase(s_core) ==
        CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT;
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "B"));
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "C"));
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "D"));
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "E"));
    s_queue_full_err = submit_user_interrupt(request->session_id, "F");
    return ESP_OK;
}

static esp_err_t test_interrupt_provider_collect(const claw_core_request_t *request,
                                                 claw_core_context_t *out_context,
                                                 void *user_ctx)
{
    (void)out_context;
    (void)user_ctx;

    if (!request || !s_interrupt_provider_enabled || s_interrupt_provider_done) {
        return ESP_ERR_NOT_FOUND;
    }
    if (request->request_id != s_active_request_id) {
        return ESP_ERR_NOT_FOUND;
    }

    s_saw_building_phase =
        claw_core_get_agent_loop_phase(s_core) == CLAW_CORE_AGENT_LOOP_PHASE_BUILDING_ITERATION_CONTEXT;
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "B"));
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "C"));
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "D"));
    TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "E"));
    s_queue_full_err = submit_user_interrupt(request->session_id, "F");
    s_interrupt_provider_done = true;
    return ESP_ERR_NOT_FOUND;
}

static const claw_core_context_provider_t s_session_provider = {
    .name = "Test Session History",
    .collect = test_session_history_collect,
    .user_ctx = NULL,
    .flags = CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY,
};

static const claw_core_context_provider_t s_interrupt_provider = {
    .name = "Test Interrupt Provider",
    .collect = test_interrupt_provider_collect,
    .user_ctx = NULL,
};

static esp_err_t test_call_cap(const char *cap_name,
                               const char *input_json,
                               const claw_core_request_t *request,
                               char **out_output,
                               void *user_ctx)
{
    (void)cap_name;
    (void)input_json;
    (void)user_ctx;

    s_tool_call_count++;
    if (s_backend_mode == TEST_BACKEND_TOOL_INTERRUPT && s_tool_call_count == 1) {
        TEST_ASSERT_EQUAL(CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL, claw_core_get_agent_loop_phase(s_core));
        TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(request->session_id, "I"));
    }

    *out_output = test_dup_string("{\"ok\":true}");
    return *out_output ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t test_backend_init(const claw_llm_runtime_config_t *config,
                                   const claw_llm_model_profile_t *profile,
                                   void **out_backend_ctx,
                                   char **out_error_message)
{
    (void)profile;
    (void)out_error_message;

    test_backend_ctx_t *ctx = calloc(1, sizeof(test_backend_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(ctx->model, config && config->model ? config->model : "", sizeof(ctx->model));
    *out_backend_ctx = ctx;
    s_backend_init_count++;
    return ESP_OK;
}

static esp_err_t test_set_final_response(claw_llm_response_t *out_response, const char *text)
{
    out_response->text = test_dup_string(text);
    out_response->raw_message_json = test_dup_string("{\"role\":\"assistant\",\"content\":\"final\"}");
    if (!out_response->text || !out_response->raw_message_json) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t test_set_tool_response(claw_llm_response_t *out_response, size_t tool_call_count)
{
    const char *raw_message_json = NULL;

    if (tool_call_count == 1) {
        raw_message_json =
            "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"test_tool\",\"arguments\":\"{}\"}}]}";
    } else if (tool_call_count == 2) {
        raw_message_json =
            "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"test_tool\",\"arguments\":\"{}\"}},"
            "{\"id\":\"call_2\",\"type\":\"function\","
            "\"function\":{\"name\":\"test_tool_2\",\"arguments\":\"{}\"}}]}";
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    out_response->raw_message_json = test_dup_string(raw_message_json);
    out_response->tool_calls = calloc(tool_call_count, sizeof(out_response->tool_calls[0]));
    if (!out_response->raw_message_json || !out_response->tool_calls) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < tool_call_count; i++) {
        char id[16];
        const char *name = i == 0 ? "test_tool" : "test_tool_2";

        snprintf(id, sizeof(id), "call_%u", (unsigned int)(i + 1));
        out_response->tool_calls[i].id = test_dup_string(id);
        out_response->tool_calls[i].name = test_dup_string(name);
        out_response->tool_calls[i].arguments_json = test_dup_string("{}");
        if (!out_response->tool_calls[i].id ||
                !out_response->tool_calls[i].name ||
                !out_response->tool_calls[i].arguments_json) {
            return ESP_ERR_NO_MEM;
        }
    }
    out_response->tool_call_count = tool_call_count;
    return ESP_OK;
}

static esp_err_t test_backend_chat(void *backend_ctx,
                                   const claw_llm_model_profile_t *profile,
                                   const claw_llm_chat_request_t *request,
                                   claw_llm_response_t *out_response,
                                   char **out_error_message)
{
    test_backend_ctx_t *ctx = (test_backend_ctx_t *)backend_ctx;
    char *messages_json = NULL;

    (void)profile;

    if (s_core) {
        s_saw_in_llm_phase =
            s_saw_in_llm_phase ||
            claw_core_get_agent_loop_phase(s_core) == CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP;
    }
    messages_json = cJSON_PrintUnformatted(request->messages);
    if (s_capture_count < TEST_CAPTURE_MAX) {
        s_captured_messages[s_capture_count++] = test_dup_string(messages_json);
    }
    if (ctx && strcmp(ctx->model, "isolate-a") == 0) {
        free(s_isolate_a_messages);
        s_isolate_a_messages = test_dup_string(messages_json);
    } else if (ctx && strcmp(ctx->model, "isolate-b") == 0) {
        free(s_isolate_b_messages);
        s_isolate_b_messages = test_dup_string(messages_json);
    }
    free(messages_json);
    s_backend_call_count++;

    if (s_backend_mode == TEST_BACKEND_HTTP_ABORT && s_backend_call_count == 1) {
        TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(s_active_session_id, "G"));
        *out_error_message = test_dup_string("aborted");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_backend_mode == TEST_BACKEND_AFTER_LLM_INTERRUPT && s_backend_call_count == 1) {
        TEST_ASSERT_EQUAL(ESP_OK, submit_user_interrupt(s_active_session_id, "H"));
        return test_set_tool_response(out_response, 2);
    }
    if (s_backend_mode == TEST_BACKEND_TOOL_INTERRUPT && s_backend_call_count == 1) {
        return test_set_tool_response(out_response, 1);
    }

    if (ctx && (strcmp(ctx->model, "isolate-a") == 0 || strcmp(ctx->model, "isolate-b") == 0)) {
        return test_set_final_response(out_response, ctx->model);
    }

    return test_set_final_response(out_response, "final");
}

static esp_err_t test_backend_infer_media(void *backend_ctx,
                                          const claw_llm_model_profile_t *profile,
                                          const claw_llm_media_request_t *request,
                                          char **out_text,
                                          char **out_error_message)
{
    test_backend_ctx_t *ctx = (test_backend_ctx_t *)backend_ctx;
    char text[64];

    (void)profile;
    (void)request;
    (void)out_error_message;

    snprintf(text, sizeof(text), "media:%s", ctx && ctx->model[0] ? ctx->model : "unknown");
    *out_text = test_dup_string(text);
    return *out_text ? ESP_OK : ESP_ERR_NO_MEM;
}

static void test_backend_deinit(void *backend_ctx)
{
    free(backend_ctx);
    s_backend_deinit_count++;
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
        .id = "core_interrupt_test",
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

static void ensure_core_ready(void)
{
    if (s_core_ready) {
        return;
    }

    const claw_core_config_t config = {
        .instance_id = 0,
        .api_key = "test",
        .backend_type = "core_interrupt_test",
        .model = "test",
        .system_prompt = "system",
        .persist_context = test_persist_context,
        .on_request_start = test_request_start,
        .call_cap = test_call_cap,
        .supports_tools = true,
        .request_queue_len = 4,
        .response_queue_len = 4,
        .max_context_providers = 2,
    };

    ensure_backend_registered();
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_create(&config, &s_core));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_context_provider(s_core, &s_session_provider));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_context_provider(s_core, &s_interrupt_provider));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_start(s_core));
    s_core_ready = true;
}

static void submit_and_expect_ok(uint32_t request_id, const char *session_id, const char *text)
{
    claw_core_request_t request = {
        .request_id = request_id,
        .session_id = session_id,
        .user_text = text,
    };
    claw_core_response_t response = {0};

    strlcpy(s_active_session_id, session_id ? session_id : "", sizeof(s_active_session_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_submit(s_core, &request, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_receive_for(s_core, request_id, &response, 5000));
    TEST_ASSERT_EQUAL(request_id, response.request_id);
    TEST_ASSERT_EQUAL(CLAW_CORE_RESPONSE_STATUS_OK, response.status);
    TEST_ASSERT_NOT_NULL(response.text);
    claw_core_response_free(&response);
}

static void submit_interrupt_flagged_and_expect_ok(uint32_t request_id, const char *session_id, const char *text)
{
    claw_core_request_t request = {
        .request_id = request_id,
        .flags = CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT,
        .session_id = session_id,
        .user_text = text,
    };
    claw_core_response_t response = {0};

    strlcpy(s_active_session_id, session_id ? session_id : "", sizeof(s_active_session_id));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_submit(s_core, &request, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_receive_for(s_core, request_id, &response, 5000));
    TEST_ASSERT_EQUAL(request_id, response.request_id);
    TEST_ASSERT_EQUAL(CLAW_CORE_RESPONSE_STATUS_OK, response.status);
    TEST_ASSERT_NOT_NULL(response.text);
    claw_core_response_free(&response);
}

static size_t count_session_context_records(const char *session_id, claw_core_context_record_type_t type)
{
    size_t count = 0;

    for (size_t i = 0; i < s_record_count; i++) {
        if (strcmp(s_records[i].session_id, session_id) == 0 && s_records[i].type == type) {
            count++;
        }
    }
    return count;
}

static void assert_messages_contain_users(size_t capture_index,
                                          const char *u0,
                                          const char *u1,
                                          const char *u2,
                                          const char *u3,
                                          const char *u4)
{
    const char *expected[] = {u0, u1, u2, u3, u4, NULL};
    cJSON *messages = cJSON_Parse(s_captured_messages[capture_index]);
    size_t user_index = 0;

    TEST_ASSERT_NOT_NULL(messages);
    TEST_ASSERT_TRUE(cJSON_IsArray(messages));
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, messages) {
        cJSON *role = cJSON_GetObjectItem(item, "role");
        cJSON *content = cJSON_GetObjectItem(item, "content");

        if (!cJSON_IsString(role) || strcmp(role->valuestring, "user") != 0) {
            continue;
        }
        TEST_ASSERT_LESS_THAN((sizeof(expected) / sizeof(expected[0])) - 1, user_index);
        TEST_ASSERT_NOT_NULL(expected[user_index]);
        TEST_ASSERT_TRUE(cJSON_IsString(content));
        TEST_ASSERT_EQUAL_STRING(expected[user_index], content->valuestring);
        user_index++;
    }
    TEST_ASSERT_NULL(expected[user_index]);
    cJSON_Delete(messages);
}

static void assert_text_contains(const char *text, const char *needle)
{
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_NOT_NULL(needle);
    TEST_ASSERT_NOT_NULL(strstr(text, needle));
}

static void test_check_heap_delta(size_t before_free,
                                  size_t after_free,
                                  const char *type,
                                  ssize_t leak_threshold,
                                  const char *message)
{
    ssize_t delta = (ssize_t)after_free - (ssize_t)before_free;

    printf("MALLOC_CAP_%s: Before %u bytes free, After %u bytes free (delta %d)\n",
           type,
           (unsigned int)before_free,
           (unsigned int)after_free,
           (int)delta);
    TEST_ASSERT_MESSAGE(delta >= leak_threshold, message);
}

static void test_socket_close(int *sock)
{
    if (sock && *sock >= 0) {
        shutdown(*sock, 0);
        close(*sock);
        *sock = -1;
    }
}

static esp_err_t test_http_read_request(int sock, char *buffer, size_t buffer_size)
{
    size_t used = 0;

    if (sock < 0 || !buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer[0] = '\0';
    while (used + 1 < buffer_size) {
        int len = recv(sock, buffer + used, buffer_size - used - 1, 0);

        if (len <= 0) {
            return ESP_FAIL;
        }
        used += (size_t)len;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n")) {
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_SIZE;
}

static void test_http_connection_task(void *arg)
{
    test_http_conn_t *conn = (test_http_conn_t *)arg;
    char request_buf[512];
    int sock = conn ? conn->sock : -1;

    if (!conn || sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    if (test_http_read_request(sock, request_buf, sizeof(request_buf)) == ESP_OK) {
        if (strstr(request_buf, "POST /slow-a ")) {
            const char *head =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 19\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"result\":\"slow";
            const char *tail = "-a\"}";

            (void)send(sock, head, strlen(head), 0);
            xSemaphoreGive(s_http_slow_ready_sem);
            (void)xSemaphoreTake(s_http_slow_release_sem, pdMS_TO_TICKS(TEST_HTTP_TIMEOUT_MS));
            (void)send(sock, tail, strlen(tail), 0);
        } else if (strstr(request_buf, "POST /fast-b ")) {
            const char *response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 19\r\n"
                "Connection: close\r\n"
                "\r\n"
                "{\"result\":\"fast-b\"}";

            (void)send(sock, response, strlen(response), 0);
        }
    }

    test_socket_close(&sock);
    conn->sock = -1;
    vTaskDelete(NULL);
}

static void test_http_server_task(void *arg)
{
    struct sockaddr_in server_addr = {0};
    int listen_sock = -1;

    (void)arg;
    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        xSemaphoreGive(s_http_server_ready_sem);
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    (void)setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(TEST_HTTP_PORT);
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0 ||
            listen(listen_sock, 2) != 0) {
        test_socket_close(&listen_sock);
        xSemaphoreGive(s_http_server_ready_sem);
        vTaskDelete(NULL);
        return;
    }

    s_http_listen_sock = listen_sock;
    xSemaphoreGive(s_http_server_ready_sem);

    for (size_t i = 0; i < sizeof(s_http_conns) / sizeof(s_http_conns[0]); i++) {
        int sock = accept(listen_sock, NULL, NULL);

        if (sock < 0) {
            break;
        }
        s_http_conns[i].sock = sock;
        xTaskCreate(test_http_connection_task,
                    "test_http_conn",
                    TEST_HTTP_TASK_STACK,
                    &s_http_conns[i],
                    5,
                    NULL);
    }

    test_socket_close(&listen_sock);
    s_http_listen_sock = -1;
    vTaskDelete(NULL);
}

static esp_err_t test_http_server_start(void)
{
    if (!s_esp_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();

        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_esp_event_loop_ready = true;
    }
    if (!s_esp_netif_ready) {
        esp_err_t err = esp_netif_init();

        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_esp_netif_ready = true;
    }

    s_http_server_ready_sem = xSemaphoreCreateBinary();
    s_http_slow_ready_sem = xSemaphoreCreateBinary();
    s_http_slow_release_sem = xSemaphoreCreateBinary();
    if (!s_http_server_ready_sem || !s_http_slow_ready_sem || !s_http_slow_release_sem) {
        return ESP_ERR_NO_MEM;
    }
    s_http_listen_sock = -1;
    for (size_t i = 0; i < sizeof(s_http_conns) / sizeof(s_http_conns[0]); i++) {
        s_http_conns[i].sock = -1;
    }

    if (xTaskCreate(test_http_server_task,
                    "test_http_srv",
                    TEST_HTTP_TASK_STACK,
                    NULL,
                    5,
                    &s_http_server_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_http_server_ready_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_http_listen_sock >= 0 ? ESP_OK : ESP_FAIL;
}

static void test_http_server_stop(void)
{
    test_socket_close(&s_http_listen_sock);
    for (size_t i = 0; i < sizeof(s_http_conns) / sizeof(s_http_conns[0]); i++) {
        test_socket_close(&s_http_conns[i].sock);
    }
    if (s_http_slow_release_sem) {
        xSemaphoreGive(s_http_slow_release_sem);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (s_http_server_ready_sem) {
        vSemaphoreDelete(s_http_server_ready_sem);
        s_http_server_ready_sem = NULL;
    }
    if (s_http_slow_ready_sem) {
        vSemaphoreDelete(s_http_slow_ready_sem);
        s_http_slow_ready_sem = NULL;
    }
    if (s_http_slow_release_sem) {
        vSemaphoreDelete(s_http_slow_release_sem);
        s_http_slow_release_sem = NULL;
    }
    s_http_server_task = NULL;
}

static void test_http_client_task(void *arg)
{
    test_http_client_task_t *task = (test_http_client_task_t *)arg;
    claw_llm_http_json_request_t request = {
        .url = task->url,
        .body = "{\"input\":\"test\"}",
        .auth_type = "none",
        .timeout_ms = TEST_HTTP_TIMEOUT_MS,
        .abort_flag = task->abort_flag,
    };

    task->err = claw_llm_http_post_json(&request, &task->response, &task->error_message);
    xSemaphoreGive(task->done_sem);
    vTaskDelete(NULL);
}

static esp_err_t test_static_messages_collect(const claw_core_request_t *request,
                                              claw_core_context_t *out_context,
                                              void *user_ctx)
{
    const char *text = (const char *)user_ctx;
    cJSON *messages = NULL;
    cJSON *message = NULL;
    char *json = NULL;

    (void)request;
    if (!out_context || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    messages = cJSON_CreateArray();
    message = cJSON_CreateObject();
    if (!messages || !message ||
            !cJSON_AddStringToObject(message, "role", "user") ||
            !cJSON_AddStringToObject(message, "content", text)) {
        cJSON_Delete(message);
        cJSON_Delete(messages);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(messages, message);
    message = NULL;

    json = cJSON_PrintUnformatted(messages);
    cJSON_Delete(messages);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_MESSAGES;
    out_context->content = json;
    return ESP_OK;
}

static void test_completion_observer(const claw_core_completion_summary_t *summary,
                                     void *user_ctx)
{
    uint32_t *count = (uint32_t *)user_ctx;

    (void)summary;
    if (count) {
        (*count)++;
    }
}

static claw_core_config_t make_test_core_config(uint32_t instance_id,
                                                const char *model,
                                                bool supports_vision)
{
    claw_core_config_t config = {
        .instance_id = instance_id,
        .api_key = "test",
        .backend_type = "core_interrupt_test",
        .model = model,
        .system_prompt = "system",
        .supports_tools = true,
        .supports_vision = supports_vision,
        .image_remote_url_only = supports_vision,
        .request_queue_len = 4,
        .response_queue_len = 4,
        .max_context_providers = 2,
    };

    return config;
}

static void run_core_create_destroy_once(uint32_t instance_id, const char *model)
{
    claw_core_handle_t core = NULL;
    uint32_t observer_count = 0;
    claw_core_config_t config = make_test_core_config(instance_id, model, false);
    claw_core_context_provider_t provider = {
        .name = "static provider",
        .collect = test_static_messages_collect,
        .user_ctx = "provider-lifecycle",
    };

    TEST_ASSERT_EQUAL(ESP_OK, claw_core_create(&config, &core));
    TEST_ASSERT_NOT_NULL(core);
    TEST_ASSERT_EQUAL(CLAW_CORE_AGENT_LOOP_PHASE_IDLE, claw_core_get_agent_loop_phase(core));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_context_provider(core, &provider));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_completion_observer(core,
                                                               test_completion_observer,
                                                               &observer_count));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_destroy(core));
}

void setUp(void)
{
    test_clear_records();
    test_clear_captures();
}

void tearDown(void)
{
    test_clear_records();
    test_clear_captures();
}

static void run_http_abort_pair(void)
{
    volatile bool abort_a = false;
    volatile bool abort_b = false;
    test_http_client_task_t client_a = {
        .url = TEST_HTTP_URL_A,
        .abort_flag = &abort_a,
    };
    test_http_client_task_t client_b = {
        .url = TEST_HTTP_URL_B,
        .abort_flag = &abort_b,
    };

    TEST_ASSERT_EQUAL(ESP_OK, test_http_server_start());
    client_a.done_sem = xSemaphoreCreateBinary();
    client_b.done_sem = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(client_a.done_sem);
    TEST_ASSERT_NOT_NULL(client_b.done_sem);

    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(test_http_client_task,
                                          "test_http_a",
                                          TEST_HTTP_TASK_STACK,
                                          &client_a,
                                          5,
                                          NULL));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_http_slow_ready_sem, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(test_http_client_task,
                                          "test_http_b",
                                          TEST_HTTP_TASK_STACK,
                                          &client_b,
                                          5,
                                          NULL));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(client_b.done_sem, pdMS_TO_TICKS(TEST_HTTP_TIMEOUT_MS)));
    TEST_ASSERT_EQUAL(ESP_OK, client_b.err);
    TEST_ASSERT_EQUAL(200, client_b.response.status_code);
    TEST_ASSERT_EQUAL_STRING("{\"result\":\"fast-b\"}", client_b.response.body);

    abort_a = true;
    xSemaphoreGive(s_http_slow_release_sem);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(client_a.done_sem, pdMS_TO_TICKS(TEST_HTTP_TIMEOUT_MS)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, client_a.err);
    assert_text_contains(client_a.error_message, "aborted");

    claw_llm_http_response_free(&client_a.response);
    claw_llm_http_response_free(&client_b.response);
    free(client_a.error_message);
    free(client_b.error_message);
    vSemaphoreDelete(client_a.done_sem);
    vSemaphoreDelete(client_b.done_sem);
    test_http_server_stop();
}

TEST_CASE("claw_llm_http transport abort flag is request local", "[claw_core][http]")
{
    run_http_abort_pair();
}

TEST_CASE("claw_llm_http transport abort path does not leak heap", "[claw_core][http][leak]")
{
    size_t before_free_8bit;
    size_t before_free_32bit;
    size_t after_free_8bit;
    size_t after_free_32bit;

    run_http_abort_pair();
    vTaskDelay(pdMS_TO_TICKS(200));

    before_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    before_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    for (size_t i = 0; i < TEST_HTTP_LEAK_RUNS; i++) {
        run_http_abort_pair();
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    after_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    after_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);

    test_check_heap_delta(before_free_8bit,
                          after_free_8bit,
                          "8BIT",
                          TEST_HTTP_LEAK_THRESHOLD,
                          "http transport memory leak");
    test_check_heap_delta(before_free_32bit,
                          after_free_32bit,
                          "32BIT",
                          TEST_HTTP_LEAK_THRESHOLD,
                          "http transport memory leak");
}

TEST_CASE("claw_core create initializes resources and destroy rejects started cores", "[claw_core]")
{
    claw_core_handle_t core = NULL;
    claw_core_handle_t started_core = NULL;
    uint32_t observer_count = 0;
    size_t init_count_before;
    size_t deinit_count_before;
    claw_core_config_t config;
    claw_core_context_provider_t provider = {
        .name = "static provider",
        .collect = test_static_messages_collect,
        .user_ctx = "provider-lifecycle",
    };

    ensure_backend_registered();
    init_count_before = s_backend_init_count;
    deinit_count_before = s_backend_deinit_count;
    config = make_test_core_config(0, "lifecycle", false);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, claw_core_create(NULL, &core));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, claw_core_create(&config, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_create(&config, &core));
    TEST_ASSERT_NOT_NULL(core);
    TEST_ASSERT_EQUAL(init_count_before + 1, s_backend_init_count);
    TEST_ASSERT_EQUAL(CLAW_CORE_AGENT_LOOP_PHASE_IDLE, claw_core_get_agent_loop_phase(core));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_context_provider(core, &provider));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_completion_observer(core,
                                                               test_completion_observer,
                                                               &observer_count));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_destroy(core));
    TEST_ASSERT_EQUAL(deinit_count_before + 1, s_backend_deinit_count);

    config = make_test_core_config(0, "started-destroy", false);
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_create(&config, &started_core));
    TEST_ASSERT_NOT_NULL(started_core);
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_start(started_core));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_destroy(started_core));
    TEST_ASSERT_EQUAL(deinit_count_before + 2, s_backend_deinit_count);
}

TEST_CASE("claw_core create destroy does not leak heap", "[claw_core][leak]")
{
    size_t before_free_8bit;
    size_t before_free_32bit;
    size_t after_free_8bit;
    size_t after_free_32bit;

    ensure_backend_registered();
    run_core_create_destroy_once(300, "core-leak-warmup");
    vTaskDelay(pdMS_TO_TICKS(100));

    before_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    before_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    for (size_t i = 0; i < TEST_CORE_LEAK_RUNS; i++) {
        char model[32];

        snprintf(model, sizeof(model), "core-leak-%u", (unsigned int)i);
        run_core_create_destroy_once(301 + (uint32_t)i, model);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    after_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    after_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);

    test_check_heap_delta(before_free_8bit,
                          after_free_8bit,
                          "8BIT",
                          TEST_CORE_LEAK_THRESHOLD,
                          "claw_core create/destroy memory leak");
    test_check_heap_delta(before_free_32bit,
                          after_free_32bit,
                          "32BIT",
                          TEST_CORE_LEAK_THRESHOLD,
                          "claw_core create/destroy memory leak");
}

TEST_CASE("claw_core isolates queues providers responses and media runtime per handle", "[claw_core]")
{
    claw_core_handle_t core_a = NULL;
    claw_core_handle_t core_b = NULL;
    claw_core_response_t response = {0};
    esp_err_t err;
    char *media_text = NULL;
    char *media_error = NULL;
    claw_media_asset_t media = {
        .kind = CLAW_MEDIA_ASSET_KIND_REMOTE_URL,
        .url = "https://example.invalid/image.jpg",
        .mime_type = "image/jpeg",
    };
    claw_llm_media_request_t media_request = {
        .system_prompt = "system",
        .user_prompt = "inspect",
        .media = &media,
        .media_count = 1,
    };
    claw_core_context_provider_t provider_a = {
        .name = "provider-a",
        .collect = test_static_messages_collect,
        .user_ctx = "provider-only-on-a",
    };
    claw_core_request_t request_a = {
        .request_id = 2001,
        .session_id = "shared-session",
        .user_text = "hello-a",
    };
    claw_core_request_t request_b = {
        .request_id = 2002,
        .session_id = "shared-session",
        .user_text = "hello-b",
    };
    claw_core_config_t config_a;
    claw_core_config_t config_b;

    ensure_backend_registered();
    config_a = make_test_core_config(101, "isolate-a", true);
    config_b = make_test_core_config(102, "isolate-b", true);

    TEST_ASSERT_EQUAL(ESP_OK, claw_core_create(&config_a, &core_a));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_create(&config_b, &core_b));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_add_context_provider(core_a, &provider_a));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_start(core_a));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_start(core_b));

    TEST_ASSERT_EQUAL(ESP_OK, claw_core_llm_infer_media(core_a, &media_request, &media_text, &media_error));
    TEST_ASSERT_NULL(media_error);
    TEST_ASSERT_EQUAL_STRING("media:isolate-a", media_text);
    free(media_text);
    media_text = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_llm_infer_media(core_b, &media_request, &media_text, &media_error));
    TEST_ASSERT_NULL(media_error);
    TEST_ASSERT_EQUAL_STRING("media:isolate-b", media_text);
    free(media_text);
    media_text = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, claw_core_submit(core_a, &request_a, 1000));
    err = claw_core_receive_for(core_b, request_a.request_id, &response, 50);
    TEST_ASSERT_TRUE(err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_receive_for(core_a, request_a.request_id, &response, 5000));
    TEST_ASSERT_EQUAL(request_a.request_id, response.request_id);
    TEST_ASSERT_EQUAL(CLAW_CORE_RESPONSE_STATUS_OK, response.status);
    TEST_ASSERT_EQUAL_STRING("isolate-a", response.text);
    claw_core_response_free(&response);
    assert_text_contains(s_isolate_a_messages, "hello-a");
    assert_text_contains(s_isolate_a_messages, "provider-only-on-a");

    TEST_ASSERT_EQUAL(ESP_OK, claw_core_submit(core_b, &request_b, 1000));
    TEST_ASSERT_EQUAL(ESP_OK, claw_core_receive_for(core_b, request_b.request_id, &response, 5000));
    TEST_ASSERT_EQUAL(request_b.request_id, response.request_id);
    TEST_ASSERT_EQUAL(CLAW_CORE_RESPONSE_STATUS_OK, response.status);
    TEST_ASSERT_EQUAL_STRING("isolate-b", response.text);
    claw_core_response_free(&response);
    assert_text_contains(s_isolate_b_messages, "hello-b");
    TEST_ASSERT_NULL(strstr(s_isolate_b_messages, "provider-only-on-a"));
}

TEST_CASE("claw_core handles active user interrupts with FIFO restarts", "[claw_core]")
{
    ensure_core_ready();

    test_reset_scenario(TEST_BACKEND_FINAL, 1001);
    s_request_start_interrupt_enabled = true;
    submit_and_expect_ok(1001, "s-before-build", "A");
    TEST_ASSERT_TRUE(s_saw_before_build_phase);
    TEST_ASSERT_TRUE(s_saw_in_llm_phase);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s_queue_full_err);
    TEST_ASSERT_EQUAL(1, s_backend_call_count);
    TEST_ASSERT_EQUAL(5, count_session_context_records("s-before-build", CLAW_CORE_CONTEXT_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-before-build", CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(0, "A", "B", "C", "D", "E");

    test_reset_scenario(TEST_BACKEND_FINAL, 1006);
    s_request_start_interrupt_enabled = true;
    s_fail_user_context_persist = true;
    submit_and_expect_ok(1006, "s-persist-fail", "A0");
    TEST_ASSERT_TRUE(s_saw_before_build_phase);
    TEST_ASSERT_TRUE(s_saw_in_llm_phase);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s_queue_full_err);
    TEST_ASSERT_EQUAL(1, s_backend_call_count);
    TEST_ASSERT_EQUAL(0, count_session_context_records("s-persist-fail", CLAW_CORE_CONTEXT_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-persist-fail", CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(0, "A0", "B", "C", "D", "E");

    test_reset_scenario(TEST_BACKEND_FINAL, 1005);
    s_interrupt_provider_enabled = true;
    submit_and_expect_ok(1005, "s-before-http", "A1");
    TEST_ASSERT_TRUE(s_saw_building_phase);
    TEST_ASSERT_TRUE(s_saw_in_llm_phase);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s_queue_full_err);
    TEST_ASSERT_EQUAL(1, s_backend_call_count);
    TEST_ASSERT_EQUAL(5, count_session_context_records("s-before-http", CLAW_CORE_CONTEXT_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-before-http", CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(0, "A1", "B", "C", "D", "E");

    test_reset_scenario(TEST_BACKEND_HTTP_ABORT, 1002);
    submit_and_expect_ok(1002, "s-http", "F");
    TEST_ASSERT_EQUAL(2, s_backend_call_count);
    TEST_ASSERT_EQUAL(2, count_session_context_records("s-http", CLAW_CORE_CONTEXT_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-http", CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(1, "F", "G", NULL, NULL, NULL);

    test_reset_scenario(TEST_BACKEND_AFTER_LLM_INTERRUPT, 1003);
    submit_and_expect_ok(1003, "s-after-llm", "A2");
    TEST_ASSERT_EQUAL(2, s_backend_call_count);
    TEST_ASSERT_EQUAL(0, s_tool_call_count);
    TEST_ASSERT_EQUAL(2, count_session_context_records("s-after-llm", CLAW_CORE_CONTEXT_RECORD_USER));
    TEST_ASSERT_EQUAL(0, count_session_context_records("s-after-llm", CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL));
    TEST_ASSERT_EQUAL(0, count_session_context_records("s-after-llm", CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT));
    assert_messages_contain_users(1, "A2", "H", NULL, NULL, NULL);

    test_reset_scenario(TEST_BACKEND_TOOL_INTERRUPT, 1004);
    submit_and_expect_ok(1004, "s-tool", "A3");
    TEST_ASSERT_EQUAL(2, s_backend_call_count);
    TEST_ASSERT_EQUAL(1, s_tool_call_count);
    TEST_ASSERT_EQUAL(2, count_session_context_records("s-tool", CLAW_CORE_CONTEXT_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-tool", CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL));
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-tool", CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT));
    assert_messages_contain_users(1, "A3", "I", NULL, NULL, NULL);
}

TEST_CASE("claw_core queues interrupt flagged requests without an insertable loop", "[claw_core]")
{
    ensure_core_ready();

    test_reset_scenario(TEST_BACKEND_FINAL, 1010);
    submit_interrupt_flagged_and_expect_ok(1010, "s-idle-fallback", "A4");
    TEST_ASSERT_EQUAL(1, s_backend_call_count);
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-idle-fallback", CLAW_CORE_CONTEXT_RECORD_USER));
    TEST_ASSERT_EQUAL(1, count_session_context_records("s-idle-fallback", CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL));
    assert_messages_contain_users(0, "A4", NULL, NULL, NULL, NULL);
}

void app_main(void)
{
    unity_run_menu();
}
