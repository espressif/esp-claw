/* SPDX-License-Identifier: CC0-1.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_llm_config.h"
#include "claw_cap.h"
#include "esp_http_client.h"
#include "llm/backends/claw_llm_backend_anthropic.h"
#include "llm/media/claw_media_pipeline.h"

static cap_llm_config_t saved;
static const claw_cap_group_t *config_group;
static const char *expected_model;
static const char *expected_url;
static bool expect_bearer;
static bool expect_media;
static unsigned requests;

struct mock_client {
    esp_http_client_config_t config;
    cJSON *headers;
    cJSON *body;
    int method;
};

static void assert_string(cJSON *object, const char *key, const char *expected)
{
    cJSON *value = cJSON_GetObjectItem(object, key);
    assert(cJSON_IsString(value));
    assert(strcmp(value->valuestring, expected) == 0);
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t copied = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copied);
        dst[copied] = '\0';
    }
    return len;
}

const char *esp_err_to_name(esp_err_t err) { (void)err; return "test error"; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
bool claw_cap_group_exists(const char *id) { (void)id; return false; }
esp_err_t claw_cap_register_group(const claw_cap_group_t *group)
{
    config_group = group;
    return ESP_OK;
}

static esp_err_t get_config(cap_llm_config_t *config, void *ctx)
{
    (void)ctx;
    *config = saved;
    return ESP_OK;
}

static esp_err_t apply_config(const cap_llm_config_t *config, void *ctx)
{
    (void)ctx;
    saved = *config;
    return ESP_OK;
}

static void command(const char *text)
{
    char output[2048];
    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "command", text);
    char *json = cJSON_PrintUnformatted(input);
    assert(config_group->descriptors[0].execute(json, NULL, output, sizeof(output)) == ESP_OK);
    free(json);
    cJSON_Delete(input);
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct mock_client *client = calloc(1, sizeof(*client));
    assert(client);
    client->config = *config;
    client->headers = cJSON_CreateObject();
    return client;
}

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method)
{
    client->method = method;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value)
{
    cJSON_AddStringToObject(client->headers, name, value);
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int len)
{
    assert((int)strlen(body) == len);
    client->body = cJSON_Parse(body);
    assert(client->body);
    return ESP_OK;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    assert(client->method == HTTP_METHOD_POST);
    assert(strcmp(client->config.url, expected_url) == 0);
    assert_string(client->headers, "Content-Type", "application/json");
    assert_string(client->headers, "anthropic-version", "2023-06-01");
    if (expect_bearer) {
        assert_string(client->headers, "Authorization", "Bearer fixture-token");
        assert(!cJSON_HasObjectItem(client->headers, "x-api-key"));
    } else {
        assert_string(client->headers, "x-api-key", "fixture-token");
        assert(!cJSON_HasObjectItem(client->headers, "Authorization"));
    }
    assert_string(client->body, "model", expected_model);
    assert(cJSON_GetObjectItem(client->body, "max_tokens")->valueint == 4096);
    /* Omission keeps Flash adaptive/max and M3's documented thinking default. */
    assert(!cJSON_HasObjectItem(client->body, "thinking"));
    assert(!cJSON_HasObjectItem(client->body, "output_config"));
    assert(!cJSON_HasObjectItem(client->body, "service_tier"));
    cJSON *messages = cJSON_GetObjectItem(client->body, "messages");
    assert(cJSON_IsArray(messages));
    if (expect_media) {
        cJSON *content = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "content");
        cJSON *image = cJSON_GetArrayItem(content, 1);
        assert_string(image, "type", "image");
        cJSON *source = cJSON_GetObjectItem(image, "source");
        assert_string(source, "type", "base64");
        assert_string(source, "media_type", "image/png");
        assert_string(source, "data", "cGl4ZWw=");
    } else {
        assert(cJSON_GetArraySize(messages) == 3);
        cJSON *content = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 1), "content");
        assert_string(cJSON_GetArrayItem(content, 0), "thinking", "keep reasoning");
        assert_string(cJSON_GetArrayItem(content, 0), "signature", "keep signature");
        assert_string(cJSON_GetArrayItem(content, 1), "id", "call-1");
        cJSON *result = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 2), "content");
        assert_string(cJSON_GetArrayItem(result, 0), "tool_use_id", "call-1");
        assert(cJSON_GetArraySize(cJSON_GetObjectItem(client->body, "tools")) == 1);
    }
    char response[] = "{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}";
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_DATA, .data = response,
        .data_len = (int)strlen(response), .user_data = client->config.user_data,
    };
    requests++;
    return client->config.event_handler(&event);
}

int esp_http_client_get_status_code(esp_http_client_handle_t client) { (void)client; return 200; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    cJSON_Delete(client->headers);
    cJSON_Delete(client->body);
    free(client);
    return ESP_OK;
}

esp_err_t claw_media_prepare_asset(const claw_media_asset_t *asset,
                                  const claw_llm_model_profile_t *profile,
                                  size_t max_bytes, claw_media_prepared_t *prepared,
                                  char **error)
{
    (void)asset; (void)profile; (void)max_bytes; (void)error;
    prepared->kind = CLAW_MEDIA_PREPARED_KIND_DATA_URL;
    prepared->payload = strdup("data:image/png;base64,cGl4ZWw=");
    return ESP_OK;
}
void claw_media_prepared_free(claw_media_prepared_t *prepared) { free(prepared->payload); }
void claw_llm_response_free(claw_llm_response_t *response)
{
    free(response->text);
    free(response->reasoning_content);
    free(response->raw_message_json);
    assert(response->tool_call_count == 0);
}

static void send_requests(const char *model, const char *url, bool bearer)
{
    expected_model = model;
    expected_url = url;
    expect_bearer = bearer;
    const claw_llm_backend_registration_t *registration = claw_llm_backend_anthropic_registration();
    const claw_llm_backend_vtable_t *backend = registration->vtable;
    claw_llm_model_profile_t profile = {
        .chat_path = registration->defaults.chat_path, .supports_tools = true, .supports_vision = true,
    };
    claw_llm_runtime_config_t config = {
        .api_key = saved.api_key, .base_url = saved.base_url, .model = saved.model,
        .auth_type = saved.auth_type, .max_tokens = 4096, .timeout_ms = 5000,
    };
    void *ctx = NULL;
    char *error = NULL;
    assert(backend->init(&config, &profile, &ctx, &error) == ESP_OK);
    cJSON *messages = cJSON_Parse("["
        "{\"role\":\"user\",\"content\":\"hello\"},"
        "{\"role\":\"assistant\",\"content\":["
        "{\"type\":\"thinking\",\"thinking\":\"keep reasoning\",\"signature\":\"keep signature\"},"
        "{\"type\":\"tool_use\",\"id\":\"call-1\",\"name\":\"lookup\",\"input\":{}}]},"
        "{\"role\":\"tool\",\"tool_call_id\":\"call-1\",\"content\":\"done\"}]");
    claw_llm_chat_request_t request = {
        .system_prompt = "test", .messages = messages,
        .tools_json = "[{\"type\":\"function\",\"function\":{\"name\":\"lookup\",\"parameters\":{\"type\":\"object\"}}}]",
    };
    claw_llm_response_t response = {0};
    expect_media = false;
    assert(backend->chat(ctx, &profile, &request, &response, &error) == ESP_OK);
    assert(strcmp(response.text, "ok") == 0);
    claw_llm_response_free(&response);
    cJSON_Delete(messages);
    claw_media_asset_t asset = {0};
    claw_llm_media_request_t media = { .user_prompt = "describe", .media = &asset, .media_count = 1 };
    char *text = NULL;
    expect_media = true;
    assert(backend->infer_media(ctx, &profile, &media, &text, &error) == ESP_OK);
    assert(strcmp(text, "ok") == 0);
    free(text);
    free(error);
    backend->deinit(ctx);
}

int main(void)
{
    cap_llm_config_provider_t provider = { .get_config = get_config, .apply_config = apply_config };
    assert(cap_llm_config_set_provider(&provider) == ESP_OK);
    assert(cap_llm_config_register_group() == ESP_OK);
    const char *presets[] = { "minimax", "minimax_cn" };
    const char *urls[] = {
        "https://api.minimax.io/anthropic/v1/messages",
        "https://api.minimax.cn/anthropic/v1/messages",
    };
    const char *models[] = { "MiniMax-M3", "MiniMax-M2.7", "custom-model" };
    char setup[256];
    for (size_t i = 0; i < 2; i++) {
        snprintf(setup, sizeof(setup), "setup %s fixture-token", presets[i]);
        command(setup);
        send_requests("MiniMax-M3.1-Flash", urls[i], true);
        for (size_t j = 0; j < 3; j++) {
            snprintf(setup, sizeof(setup), "setup %s fixture-token %s", presets[i], models[j]);
            command(setup);
            cap_llm_config_t before = saved;
            command("status");
            assert(memcmp(&before, &saved, sizeof(saved)) == 0);
            send_requests(models[j], urls[i], true);
        }
    }
    command("setup custom backend=anthropic_compatible url=https://custom.invalid/v1 model=saved-custom token=fixture-token");
    send_requests("saved-custom", "https://custom.invalid/v1/messages", true);
    command("setup anthropic fixture-token");
    send_requests("claude-3-5-haiku-latest", "https://api.anthropic.com/v1/messages", false);
    assert(requests == 20);
    printf("PASS: %u offline HTTP requests (regional defaults, M3, saved legacy/custom, tool/thinking history, media, native auth)\n", requests);
    return 0;
}
