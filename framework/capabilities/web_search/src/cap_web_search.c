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
#define CAP_WEB_SEARCH_RESULT_COUNT 5

typedef enum {
    CAP_WEB_SEARCH_PROVIDER_NONE = 0,
    CAP_WEB_SEARCH_PROVIDER_BRAVE,
    CAP_WEB_SEARCH_PROVIDER_TAVILY,
} cap_web_search_provider_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cap_web_search_buf_t;

typedef struct {
    char brave_key[128];
    char tavily_key[128];
    cap_web_search_provider_t provider;
} cap_web_search_state_t;

static EXT_RAM_BSS_ATTR cap_web_search_state_t s_search = {0};

static void cap_web_search_refresh_provider(void)
{
    if (s_search.tavily_key[0]) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_TAVILY;
    } else if (s_search.brave_key[0]) {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_BRAVE;
    } else {
        s_search.provider = CAP_WEB_SEARCH_PROVIDER_NONE;
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
    if (buf->len + append_len + 1 > buf->cap) {
        size_t new_cap = buf->cap * 2;
        char *new_data = NULL;

        if (new_cap < buf->len + append_len + 1) {
            new_cap = buf->len + append_len + 1;
        }
        new_data = realloc(buf->data, new_cap);
        if (!new_data) {
            return ESP_ERR_NO_MEM;
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

static void cap_web_search_format_brave_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = NULL;
    cJSON *results = NULL;
    cJSON *item = NULL;
    size_t offset = 0;
    int index = 0;

    web = cJSON_GetObjectItem(root, "web");
    results = web ? cJSON_GetObjectItem(web, "results") : NULL;
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

        title = cJSON_GetObjectItem(item, "title");
        url = cJSON_GetObjectItem(item, "url");
        description = cJSON_GetObjectItem(item, "description");
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

static void cap_web_search_format_tavily_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *results = NULL;
    cJSON *item = NULL;
    size_t offset = 0;
    int index = 0;

    results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON_ArrayForEach(item, results) {
        cJSON *title = NULL;
        cJSON *url = NULL;
        cJSON *content = NULL;
        int written;

        if (index >= CAP_WEB_SEARCH_RESULT_COUNT || offset >= output_size - 1) {
            break;
        }

        title = cJSON_GetObjectItem(item, "title");
        url = cJSON_GetObjectItem(item, "url");
        content = cJSON_GetObjectItem(item, "content");
        written = snprintf(output + offset,
                           output_size - offset,
                           "%d. %s\n   %s\n   %s\n\n",
                           index + 1,
                           cJSON_IsString(title) ? title->valuestring : "(no title)",
                           cJSON_IsString(url) ? url->valuestring : "",
                           cJSON_IsString(content) ? content->valuestring : "");
        if (written < 0 || (size_t)written >= output_size - offset) {
            output[output_size - 1] = '\0';
            return;
        }

        offset += (size_t)written;
        index++;
    }
}

static char *cap_web_search_build_tavily_payload(const char *query)
{
    cJSON *root = NULL;
    char *payload = NULL;

    root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "query", query);
    cJSON_AddNumberToObject(root, "max_results", CAP_WEB_SEARCH_RESULT_COUNT);
    cJSON_AddBoolToObject(root, "include_answer", false);
    cJSON_AddStringToObject(root, "search_depth", "basic");
    payload = cJSON_PrintUnformatted(root);
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
    };
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;

    client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search.brave_key);
    err = esp_http_client_perform(client);
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

static esp_err_t cap_web_search_tavily_direct(const char *query, cap_web_search_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/search",
        .event_handler = cap_web_search_http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = NULL;
    char auth[192];
    char *payload = NULL;
    esp_err_t err;
    int status;

    payload = cap_web_search_build_tavily_payload(query);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        return ESP_FAIL;
    }

    snprintf(auth, sizeof(auth), "Bearer %s", s_search.tavily_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, payload, strlen(payload));
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);
    if (err != ESP_OK) {
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Tavily search returned %d", status);
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

    cap_web_search_refresh_provider();
    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_NONE) {
        snprintf(output, output_size, "Error: no search provider credentials configured");
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

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY) {
        err = cap_web_search_tavily_direct(query->valuestring, &buf);
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
    if (err != ESP_OK) {
        free(buf.data);
        snprintf(output, output_size, "Error: search request failed (%s)", esp_err_to_name(err));
        return err;
    }

    root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) {
        snprintf(output, output_size, "Error: failed to parse search results");
        return ESP_FAIL;
    }

    if (s_search.provider == CAP_WEB_SEARCH_PROVIDER_TAVILY) {
        cap_web_search_format_tavily_results(root, output, output_size);
    } else {
        cap_web_search_format_brave_results(root, output, output_size);
    }
    cJSON_Delete(root);
    return ESP_OK;
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

    cap_web_search_refresh_provider();
    return claw_cap_register_group(&s_web_search_group);
}

esp_err_t cap_web_search_set_brave_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.brave_key, api_key, sizeof(s_search.brave_key));
    cap_web_search_refresh_provider();
    return ESP_OK;
}

esp_err_t cap_web_search_set_tavily_key(const char *api_key)
{
    if (!api_key) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.tavily_key, api_key, sizeof(s_search.tavily_key));
    cap_web_search_refresh_provider();
    return ESP_OK;
}
