/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_web_search.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "cap_web_search";

#define CAP_WEB_SEARCH_BUF_SIZE     (16 * 1024)
#define CAP_WEB_SEARCH_MAX_RESPONSE (64 * 1024)
#define CAP_WEB_SEARCH_RESULT_COUNT 5
#define CAP_WEB_SEARCH_KEY_SIZE     320

typedef enum {
    CAP_WEB_SEARCH_PROVIDER_NONE = 0,
    CAP_WEB_SEARCH_PROVIDER_BRAVE,
    CAP_WEB_SEARCH_PROVIDER_TAVILY,
    CAP_WEB_SEARCH_PROVIDER_BOCHA,
} cap_web_search_provider_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    esp_err_t error;
} cap_web_search_buf_t;

typedef struct {
    char brave_key[CAP_WEB_SEARCH_KEY_SIZE];
    char tavily_key[CAP_WEB_SEARCH_KEY_SIZE];
    char bocha_key[CAP_WEB_SEARCH_KEY_SIZE];
    cap_web_search_provider_t provider;
} cap_web_search_state_t;

static EXT_RAM_BSS_ATTR cap_web_search_state_t s_search = {0};

static const char *cap_web_search_provider_key(void)
{
    switch (s_search.provider) {
    case CAP_WEB_SEARCH_PROVIDER_BRAVE:
        return s_search.brave_key;
    case CAP_WEB_SEARCH_PROVIDER_TAVILY:
        return s_search.tavily_key;
    case CAP_WEB_SEARCH_PROVIDER_BOCHA:
        return s_search.bocha_key;
    default:
        return "";
    }
}

static esp_err_t cap_web_search_http_event_handler(esp_http_client_event_t *event)
{
    cap_web_search_buf_t *buf = NULL;
    size_t append_len;

    if (!event) {
        return ESP_OK;
    }

    buf = (cap_web_search_buf_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !buf || !buf->data || event->data_len <= 0) {
        return ESP_OK;
    }

    append_len = (size_t)event->data_len;
    // Preserve receive errors even if the HTTP client ignores callback failures.
    if (buf->error != ESP_OK) {
        return buf->error;
    }
    if (append_len > CAP_WEB_SEARCH_MAX_RESPONSE - buf->len) {
        buf->error = ESP_ERR_INVALID_SIZE;
        ESP_LOGE(TAG, "Search response exceeds %d bytes", CAP_WEB_SEARCH_MAX_RESPONSE);
        return buf->error;
    }
    if (buf->len + append_len + 1 > buf->cap) {
        size_t new_cap = buf->cap * 2;
        char *new_data = NULL;

        if (new_cap < buf->len + append_len + 1) {
            new_cap = buf->len + append_len + 1;
        }
        if (new_cap > CAP_WEB_SEARCH_MAX_RESPONSE + 1) {
            new_cap = CAP_WEB_SEARCH_MAX_RESPONSE + 1;
        }
        new_data = realloc(buf->data, new_cap);
        if (!new_data) {
            buf->error = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "Failed to grow search response buffer");
            return buf->error;
        }
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, event->data, append_len);
    buf->len += append_len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static size_t cap_web_search_url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    while (*src && pos < dst_size - 1) {
        unsigned char c = (unsigned char) * src;

        if ((c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = (char)c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            if (pos + 3 >= dst_size) {
                break;
            }
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
        src++;
    }

    dst[pos] = '\0';
    return pos;
}

static void cap_web_search_format_results(cJSON *results, const char *title_key, const char *text_key,
                                          const char *fallback_key, char *output, size_t output_size)
{
    cJSON *item = NULL;
    size_t offset = 0;
    int index = 0;

    output[0] = '\0';
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON_ArrayForEach(item, results) {
        cJSON *title = NULL;
        cJSON *url = NULL;
        cJSON *description = NULL;
        int written;

        if (index >= CAP_WEB_SEARCH_RESULT_COUNT || offset >= output_size - 1) {
            break;
        }

        title = cJSON_GetObjectItem(item, title_key);
        url = cJSON_GetObjectItem(item, "url");
        description = cJSON_GetObjectItem(item, text_key);
        if (fallback_key && (!cJSON_IsString(description) || !description->valuestring[0])) {
            description = cJSON_GetObjectItem(item, fallback_key);
        }
        written = snprintf(output + offset,
                           output_size - offset,
                           "%d. %s\n   %s\n   %s\n\n",
                           index + 1,
                           cJSON_IsString(title) ? title->valuestring : "(no title)",
                           cJSON_IsString(url) ? url->valuestring : "",
                           cJSON_IsString(description) ? description->valuestring : "");
        if (written < 0 || (size_t)written >= output_size - offset) {
            output[output_size - 1] = '\0';
            return;
        }

        offset += (size_t)written;
        index++;
    }
}

static char *cap_web_search_build_payload(const char *query)
{
    cJSON *root = NULL;
    char *payload = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    bool bocha = s_search.provider == CAP_WEB_SEARCH_PROVIDER_BOCHA;
    if (cJSON_AddStringToObject(root, "query", query) &&
            cJSON_AddNumberToObject(root, bocha ? "count" : "max_results", CAP_WEB_SEARCH_RESULT_COUNT) &&
            cJSON_AddBoolToObject(root, bocha ? "summary" : "include_answer", bocha) &&
            cJSON_AddStringToObject(root, bocha ? "freshness" : "search_depth", bocha ? "noLimit" : "basic")) {
        payload = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    return payload;
}

static esp_err_t cap_web_search_brave_direct(const char *url, cap_web_search_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = cap_web_search_http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
#ifdef CONFIG_HTTP_REUSE_ENABLE
        .keep_alive_enable = true,
#endif
    };
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;

    client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    err = esp_http_client_set_header(client, "Accept", "application/json");
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "X-Subscription-Token", s_search.brave_key);
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Brave search returned %d", status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_web_search_post(const char *url, const char *key, const char *payload, cap_web_search_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = cap_web_search_http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
#ifdef CONFIG_HTTP_REUSE_ENABLE
        .keep_alive_enable = true,
#endif
    };
    esp_http_client_handle_t client = NULL;
    char *auth = NULL;
    esp_err_t err;
    int status;

    size_t auth_size = strlen(key) + sizeof("Bearer ");
    auth = malloc(auth_size);
    if (!auth) {
        return ESP_ERR_NO_MEM;
    }

    client = esp_http_client_init(&config);
    if (!client) {
        free(auth);
        return ESP_FAIL;
    }

    snprintf(auth, auth_size, "Bearer %s", key);
    err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Accept", "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Authorization", auth);
    }
    if (err == ESP_OK) {
        err = esp_http_client_set_post_field(client, payload, strlen(payload));
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(auth);
    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Search provider %d returned HTTP %d", s_search.provider, status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t cap_web_search_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    cJSON *input = NULL;
    cJSON *query = NULL;
    cap_web_search_buf_t buf = {0};
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!cap_web_search_provider_key()[0]) {
        snprintf(output, output_size, "Error: selected search provider has no API key");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    query = cJSON_GetObjectItem(input, "query");
    if (!cJSON_IsString(query) || !query->valuestring || !query->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing query");
        return ESP_ERR_INVALID_ARG;
    }

    buf.data = calloc(1, CAP_WEB_SEARCH_BUF_SIZE);
    if (!buf.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    buf.cap = CAP_WEB_SEARCH_BUF_SIZE;

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY || s_search.provider == CAP_WEB_SEARCH_PROVIDER_BOCHA) {
        const char *url = s_search.provider == CAP_WEB_SEARCH_PROVIDER_BOCHA ?
                          "https://api.bochaai.com/v1/web-search" : "https://api.tavily.com/search";
        char *payload = cap_web_search_build_payload(query->valuestring);
        err = payload ? cap_web_search_post(url, cap_web_search_provider_key(), payload, &buf) : ESP_ERR_NO_MEM;
        free(payload);
    } else {
        char encoded_query[256];
        char url[512];

        cap_web_search_url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
        snprintf(url,
                 sizeof(url),
                 "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d",
                 encoded_query,
                 CAP_WEB_SEARCH_RESULT_COUNT);
        err = cap_web_search_brave_direct(url, &buf);
    }

    cJSON_Delete(input);
    if (buf.error != ESP_OK) {
        err = buf.error;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Search provider %d failed: %s", s_search.provider, esp_err_to_name(err));
        free(buf.data);
        snprintf(output, output_size, "Error: search request failed (%s)", esp_err_to_name(err));
        return err;
    }

    root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) {
        ESP_LOGE(TAG, "Invalid search response JSON");
        snprintf(output, output_size, "Error: failed to parse search results");
        return ESP_FAIL;
    }

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY) {
        cap_web_search_format_results(cJSON_GetObjectItem(root, "results"), "title", "content", NULL, output, output_size);
    } else if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_BOCHA) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *data = cJSON_GetObjectItem(root, "data");
        cJSON *web = cJSON_GetObjectItem(data, "webPages");
        cJSON *results = cJSON_GetObjectItem(web, "value");
        if (!cJSON_IsNumber(code) || code->valueint != 200 || !cJSON_IsObject(data) ||
                (web && !cJSON_IsArray(results))) {
            ESP_LOGE(TAG, "Invalid Bocha response, code=%d", cJSON_IsNumber(code) ? code->valueint : -1);
            snprintf(output, output_size, "Error: Bocha search failed (code=%d)", cJSON_IsNumber(code) ? code->valueint : -1);
            err = ESP_FAIL;
        } else {
            cap_web_search_format_results(results, "name", "summary", "snippet", output, output_size);
        }
    } else {
        cJSON *web = cJSON_GetObjectItem(root, "web");
        cap_web_search_format_results(cJSON_GetObjectItem(web, "results"), "title", "description", NULL, output, output_size);
    }
    cJSON_Delete(root);
    return err;
}

static const claw_cap_descriptor_t s_web_search_descriptors[] = {
    {
        .id = "web_search",
        .name = "web_search",
        .family = "system",
        .description = "Search the web with the configured provider and return concise formatted results.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}",
        .execute = cap_web_search_execute,
    },
};

static const claw_cap_group_t s_web_search_group = {
    .group_id = "cap_web_search",
    .descriptors = s_web_search_descriptors,
    .descriptor_count = sizeof(s_web_search_descriptors) / sizeof(s_web_search_descriptors[0]),
};

esp_err_t cap_web_search_register_group(void)
{
    if (claw_cap_group_exists(s_web_search_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_web_search_group);
}

esp_err_t cap_web_search_set_provider(const char *provider)
{
    if (!provider) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(provider, "bocha") == 0) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_BOCHA;
    } else if (strcmp(provider, "tavily") == 0) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_TAVILY;
    } else if (strcmp(provider, "brave") == 0) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_BRAVE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Search provider: %s", provider);
    return ESP_OK;
}

esp_err_t cap_web_search_set_brave_key(const char *api_key)
{
    if (!api_key || strlen(api_key) >= sizeof(s_search.brave_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.brave_key, api_key, sizeof(s_search.brave_key));
    return ESP_OK;
}

esp_err_t cap_web_search_set_tavily_key(const char *api_key)
{
    if (!api_key || strlen(api_key) >= sizeof(s_search.tavily_key)) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.tavily_key, api_key, sizeof(s_search.tavily_key));
    return ESP_OK;
}

esp_err_t cap_web_search_set_bocha_key(const char *api_key)
{
    if (!api_key || strlen(api_key) >= sizeof(s_search.bocha_key)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_search.bocha_key, api_key, sizeof(s_search.bocha_key));
    return ESP_OK;
}
