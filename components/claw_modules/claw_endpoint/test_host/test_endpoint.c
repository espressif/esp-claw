/* Host regression tests execute the production endpoint, HTTP and LLM backends. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "claw_endpoint.h"
#include "claw_http.h"
#include "llm/claw_llm_runtime.h"
#include "llm/claw_llm_http_transport.h"
#include "llm/media/claw_media_pipeline.h"

static int64_t now_ms;
static int64_t cancel_at;
static bool cancelled;
static unsigned http_attempts;
static unsigned acquires;
static int64_t acquire_deadline;
static bool reject_once, reject_always, fail_acquire, cancel_in_acquire;
static bool redirect_once, redirect_always, stall_retry_read;
static unsigned first_attempt_ms;
static const char *reply;
static char last_url[256], last_auth[128], last_key[128], last_version[64];
static enum { NORMAL, STALL_OPEN, STALL_WRITE, STALL_HEADERS, STALL_READ } stall;

struct mock_http_client {
    char *url;
    char auth[128], key[128], version[64];
    const char *reply;
    size_t read;
    int status;
    int timeout;
};

int64_t esp_timer_get_time(void) { return now_ms * 1000; }
const char *esp_err_to_name(esp_err_t err) { return "mock-error"; }
esp_err_t esp_crt_bundle_attach(void *ctx) { return ESP_OK; }
void vTaskDelay(uint32_t ticks)
{
    now_ms += ticks;
    if (cancel_at && now_ms >= cancel_at) cancelled = true;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(config->is_async && !config->keep_alive_enable);
    struct mock_http_client *client = calloc(1, sizeof(*client));
    assert(client);
    client->url = strdup(config->url);
    client->timeout = config->timeout_ms;
    return client;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    strlcpy(last_url, client->url, sizeof(last_url));
    strlcpy(last_auth, client->auth, sizeof(last_auth));
    strlcpy(last_key, client->key, sizeof(last_key));
    strlcpy(last_version, client->version, sizeof(last_version));
    free(client->url);
    free(client);
    return ESP_OK;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, int m) { return ESP_OK; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int timeout)
{
    assert(timeout > 0);
    c->timeout = timeout;
    return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *name, const char *value)
{
    if (!strcasecmp(name, "Authorization")) strlcpy(c->auth, value, sizeof(c->auth));
    if (!strcasecmp(name, "x-api-key")) strlcpy(c->key, value, sizeof(c->key));
    if (!strcasecmp(name, "anthropic-version")) strlcpy(c->version, value, sizeof(c->version));
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int size)
{
    if (stall == STALL_OPEN) return ESP_ERR_HTTP_EAGAIN;
    http_attempts++;
    if (http_attempts == 1) vTaskDelay(first_attempt_ms);
    c->status = reject_always || (reject_once && http_attempts == 1) ? 401 : 200;
    if (redirect_always || (redirect_once && http_attempts == 1)) c->status = 302;
    c->reply = c->status == 401 ? "{\"error\":{\"message\":\"rejected\"}}" : reply;
    if (c->status == 302) c->reply = "redirect response body";
    return ESP_OK;
}
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c)
{
    free(c->url); c->url = strdup("https://redirect.test/v1/chat/completions"); return ESP_OK;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    c->read = 0; c->status = 0; return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int size)
{
    return stall == STALL_WRITE ? 0 : (size > 13 ? 13 : size);
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    return stall == STALL_HEADERS ? -ESP_ERR_HTTP_EAGAIN : (int64_t)strlen(c->reply);
}
int esp_http_client_read(esp_http_client_handle_t c, char *data, int size)
{
    if (stall == STALL_READ || (stall_retry_read && http_attempts > 1)) return -ESP_ERR_HTTP_EAGAIN;
    size_t count = strlen(c->reply) - c->read;
    if (count > (size_t)size) count = size;
    memcpy(data, c->reply + c->read, count);
    c->read += count;
    return count;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c)
{
    /* Simulate fetch_headers reading the complete response into the SDK cache.
     * The transport must still drain it, even though this predicate is true. */
    return stall != STALL_READ && !(stall_retry_read && http_attempts > 1);
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }

esp_err_t claw_media_prepare_asset(const claw_media_asset_t *asset,
    const claw_llm_model_profile_t *profile, size_t limit, claw_media_prepared_t *out, char **error)
{
    return ESP_ERR_NOT_SUPPORTED;
}
void claw_media_prepared_free(claw_media_prepared_t *prepared) { free(prepared->payload); }

typedef struct { uint32_t generation; } dynamic_t;
static bool dynamic_configured(const claw_endpoint_config_t *config) { return true; }
static esp_err_t dynamic_create(const claw_endpoint_config_t *config, void **out)
{
    dynamic_t *ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    ctx->generation = 1;
    *out = ctx;
    return ESP_OK;
}
static esp_err_t dynamic_acquire(void *ctx, const claw_endpoint_request_t *request, claw_endpoint_resolved_t *out)
{
    dynamic_t *state = ctx;
    if (acquires++) assert(request->deadline_ms == acquire_deadline);
    else acquire_deadline = request->deadline_ms;
    char key[32];
    snprintf(key, sizeof(key), "dynamic-%u", state->generation);
    assert(claw_endpoint_resolved_set_url(out, "https://dynamic.test/v1") == ESP_OK);
    assert(claw_endpoint_resolved_add_header(out, "X-API-Key", key) == ESP_OK);
    out->credential_id = state->generation;
    if (cancel_in_acquire) cancelled = true;
    return fail_acquire ? ESP_FAIL : ESP_OK;
}
static bool dynamic_report(void *ctx, const claw_endpoint_resolved_t *resolved, claw_endpoint_outcome_t outcome)
{
    dynamic_t *state = ctx;
    if (outcome != CLAW_ENDPOINT_OUTCOME_REJECTED) return false;
    if (state->generation == resolved->credential_id) state->generation++;
    return true;
}
static const claw_endpoint_provider_ops_t dynamic_ops = {
    .is_configured = dynamic_configured, .create = dynamic_create,
    .acquire = dynamic_acquire, .report = dynamic_report, .destroy = free,
};
static void reset(void)
{
    now_ms = cancel_at = acquire_deadline = 0;
    cancelled = reject_once = reject_always = fail_acquire = cancel_in_acquire = false;
    redirect_once = redirect_always = stall_retry_read = false;
    first_attempt_ms = 0;
    http_attempts = acquires = 0;
    stall = NORMAL;
    reply = "{\"ok\":true}";
    last_url[0] = last_auth[0] = last_key[0] = last_version[0] = 0;
}
static claw_endpoint_handle_t dynamic_endpoint(void)
{
    claw_endpoint_handle_t endpoint = NULL;
    assert(claw_endpoint_create("test-dynamic", &(claw_endpoint_config_t){0}, &endpoint) == ESP_OK);
    return endpoint;
}
static esp_err_t post(claw_endpoint_handle_t endpoint, const claw_llm_http_header_t *headers, size_t count)
{
    claw_llm_http_json_request_t request = {
        .body = "{}", .timeout_ms = 20, .abort_flag = &cancelled,
        .headers = headers, .header_count = count,
    };
    claw_llm_http_response_t response = {0};
    char *error = NULL;
    esp_err_t err = claw_llm_http_post_endpoint(endpoint, "/chat/completions", &request, &response, &error);
    if (err == ESP_OK) assert(!strcmp(response.body, reply));
    claw_llm_http_response_free(&response);
    free(error);
    return err;
}
static void test_static_auth(void)
{
    const char *schemes[] = { "bearer", "api-key", "none" };
    for (size_t i = 0; i < 3; i++) {
        reset();
        char key[] = "secret";
        char url[] = "https://static.test/v1";
        claw_endpoint_config_t config = {.base_url = url, .api_key = i == 2 ? NULL : key, .auth_type = schemes[i]};
        claw_endpoint_handle_t endpoint = NULL;
        assert(claw_endpoint_is_configured(NULL, &config));
        assert(claw_endpoint_create(NULL, &config, &endpoint) == ESP_OK);
        key[0] = url[0] = 'X';
        assert(post(endpoint, NULL, 0) == ESP_OK);
        assert(!strcmp(last_url, "https://static.test/v1/chat/completions"));
        assert(!strcmp(last_auth, i == 0 ? "Bearer secret" : ""));
        assert(!strcmp(last_key, i == 1 ? "secret" : ""));
        claw_endpoint_delete(endpoint);
    }
    claw_endpoint_config_t invalid = {.base_url = "https://test", .api_key = "key", .auth_type = "invalid"};
    claw_endpoint_handle_t endpoint = NULL;
    assert(!claw_endpoint_is_configured(NULL, &invalid));
    assert(claw_endpoint_create(NULL, &invalid, &endpoint) == ESP_ERR_INVALID_ARG);
    assert(endpoint == NULL);
}
static void test_runtime_protocols(void)
{
    const char *backends[] = {"openai_compatible", "openai_responses", "anthropic_compatible"};
    const char *replies[] = {
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello\"}}]}",
        "{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"hello\"}]}]}",
        "{\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}",
    };
    for (size_t i = 0; i < 3; i++) {
        reset();
        reply = replies[i];
        claw_llm_runtime_config_t config = {
            .backend_type = backends[i], .endpoint_provider = "test-dynamic",
            .model = "test", .timeout_ms = 100,
        };
        assert(claw_llm_runtime_endpoint_is_configured(&config));
        claw_llm_runtime_t *runtime = NULL;
        char *error = NULL;
        assert(claw_llm_runtime_init(&runtime, &config, &error) == ESP_OK);
        claw_llm_chat_request_t request = {.system_prompt = "test", .messages = cJSON_Parse("[{\"role\":\"user\",\"content\":\"Hi\"}]")};
        claw_llm_response_t response = {0};
        esp_err_t result = claw_llm_runtime_chat(runtime, &request, &response, &error);
        if (result != ESP_OK) fprintf(stderr, "%s: %s (%d)\n", backends[i], error ? error : "no detail", result);
        assert(result == ESP_OK);
        assert(!strcmp(response.text, "hello"));
        assert(!strcmp(last_key, "dynamic-1"));
        assert(!last_auth[0]);
        if (i == 2) assert(!strcmp(last_version, "2023-06-01"));
        claw_llm_response_free(&response);
        cJSON_Delete(request.messages);
        free(error);
        claw_llm_runtime_deinit(runtime);
    }
}
static void test_retry_cancel_and_cleanup(void)
{
    reset();
    claw_endpoint_handle_t endpoint = dynamic_endpoint();
    reject_once = true;
    assert(post(endpoint, NULL, 0) == ESP_OK);
    assert(http_attempts == 2 && acquires == 2 && !strcmp(last_key, "dynamic-2"));
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint(); reject_once = true;
    first_attempt_ms = 6; stall_retry_read = true;
    assert(post(endpoint, NULL, 0) == ESP_ERR_TIMEOUT);
    assert(http_attempts == 2 && acquires == 2 && now_ms == 20);
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint(); redirect_once = true;
    assert(post(endpoint, NULL, 0) == ESP_OK);
    assert(http_attempts == 2 && acquires == 1);
    assert(!strcmp(last_url, "https://redirect.test/v1/chat/completions"));
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint(); redirect_always = true;
    assert(post(endpoint, NULL, 0) == ESP_ERR_HTTP_MAX_REDIRECT);
    assert(http_attempts == 11 && acquires == 1);
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint(); reject_always = true;
    assert(post(endpoint, NULL, 0) == CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED);
    assert(http_attempts == 2 && acquires == 2);
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint();
    claw_llm_http_header_t conflict = {.name = "x-api-key", .value = "backend-key"};
    assert(post(endpoint, &conflict, 1) == ESP_ERR_INVALID_ARG);
    assert(http_attempts == 0);
    claw_endpoint_delete(endpoint);

    for (int mode = STALL_OPEN; mode <= STALL_READ; mode++) {
        reset(); endpoint = dynamic_endpoint(); stall = mode;
        assert(post(endpoint, NULL, 0) == ESP_ERR_TIMEOUT);
        assert(now_ms == 20 && http_attempts <= 1);
        claw_endpoint_delete(endpoint);
    }
    reset(); endpoint = dynamic_endpoint(); stall = STALL_READ; cancel_at = 7;
    assert(post(endpoint, NULL, 0) == CLAW_ENDPOINT_ERR_CANCELLED);
    assert(now_ms == 7);
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint(); cancelled = true;
    assert(post(endpoint, NULL, 0) == CLAW_ENDPOINT_ERR_CANCELLED);
    assert(acquires == 0 && http_attempts == 0);
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint(); cancel_in_acquire = true;
    assert(post(endpoint, NULL, 0) == CLAW_ENDPOINT_ERR_CANCELLED);
    assert(http_attempts == 0);
    claw_endpoint_delete(endpoint);

    reset(); endpoint = dynamic_endpoint(); fail_acquire = true;
    claw_endpoint_request_t request;
    claw_endpoint_request_init(&request, 100, NULL, NULL);
    claw_endpoint_resolved_t resolved = {0};
    assert(claw_endpoint_acquire(endpoint, &request, &resolved) == ESP_FAIL);
    assert(!resolved.url && !resolved.header_count);
    claw_endpoint_resolved_free(&resolved);
    claw_endpoint_delete(endpoint);
}
int main(void)
{
    assert(claw_endpoint_register_provider("test-dynamic", &dynamic_ops) == ESP_OK);
    test_static_auth();
    test_runtime_protocols();
    test_retry_cancel_and_cleanup();
    puts("PASS: static auth, all three protocols with dynamic credentials, bounded retry/redirects, cancellation, shared deadline, ownership");
    return 0;
}
