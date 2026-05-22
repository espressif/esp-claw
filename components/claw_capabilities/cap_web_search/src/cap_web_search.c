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
#include <strings.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "cap_web_search";

#define CAP_WEB_SEARCH_BUF_SIZE                (16 * 1024)
#define CAP_WEB_SEARCH_RESULT_COUNT            5
#define CAP_WEB_SEARCH_HTTP_TIMEOUT_MS_DEFAULT 15000
#define CAP_WEB_SEARCH_HTTP_MAX_BODY_DEFAULT   (16 * 1024)
#define CAP_WEB_SEARCH_HTTP_MAX_BODY_LIMIT     ((64 * 1024) - 1)
#define CAP_WEB_SEARCH_HTTP_ALLOWLIST_MAX      320
#define CAP_WEB_SEARCH_REDIRECT_URL_MAX        512

typedef enum {
    CAP_WEB_SEARCH_PROVIDER_NONE = 0,
    CAP_WEB_SEARCH_PROVIDER_BRAVE,
    CAP_WEB_SEARCH_PROVIDER_TAVILY,
} cap_web_search_provider_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    /* Maximum captured response body length; 0 means unlimited. */
    size_t max_len;
    /* True when response body exceeds max_len and was truncated. */
    bool truncated;
    /* When true, HTTP_EVENT_REDIRECT validates the Location URL against the allowlist. */
    bool check_redirect_allowlist;
    /* Stores the most recently received Location header value for redirect validation. */
    char redirect_location[CAP_WEB_SEARCH_REDIRECT_URL_MAX];
} cap_web_search_buf_t;

typedef struct {
    char brave_key[128];
    char tavily_key[128];
    char http_allowlist[CAP_WEB_SEARCH_HTTP_ALLOWLIST_MAX];
    cap_web_search_provider_t provider;
} cap_web_search_state_t;

static cap_web_search_state_t s_search = {0};

static bool cap_web_search_extract_url_host(const char *url, char *host, size_t host_size);
static bool cap_web_search_host_allowed(const char *host);

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

    switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (buf && buf->check_redirect_allowlist &&
                event->header_key && event->header_value &&
                strcasecmp(event->header_key, "Location") == 0) {
            strlcpy(buf->redirect_location, event->header_value, sizeof(buf->redirect_location));
        }
        break;

    case HTTP_EVENT_REDIRECT:
        if (buf && buf->check_redirect_allowlist) {
            char host[128] = {0};

            if (!buf->redirect_location[0] ||
                    !cap_web_search_extract_url_host(buf->redirect_location, host, sizeof(host)) ||
                    !cap_web_search_host_allowed(host)) {
                ESP_LOGW(TAG, "Redirect to '%s' blocked by allowlist", buf->redirect_location);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "Redirect to host '%s' allowed by allowlist", host);
            buf->redirect_location[0] = '\0';
            return esp_http_client_set_redirection(event->client);
        }
        break;

    case HTTP_EVENT_ON_DATA:
        if (!buf || !buf->data || event->data_len <= 0) {
            break;
        }
        append_len = (size_t)event->data_len;
        if (buf->max_len > 0 && buf->len + append_len > buf->max_len) {
            if (buf->len >= buf->max_len) {
                buf->truncated = true;
                break;
            }
            append_len = buf->max_len - buf->len;
            buf->truncated = true;
        }
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
        break;

    default:
        break;
    }

    return ESP_OK;
}

static bool cap_web_search_is_valid_http_method(const char *method,
                                                esp_http_client_method_t *out_method)
{
    if (!method || !method[0]) {
        *out_method = HTTP_METHOD_GET;
        return true;
    }

    if (strcasecmp(method, "GET") == 0) {
        *out_method = HTTP_METHOD_GET;
        return true;
    }
    if (strcasecmp(method, "POST") == 0) {
        *out_method = HTTP_METHOD_POST;
        return true;
    }
    if (strcasecmp(method, "PUT") == 0) {
        *out_method = HTTP_METHOD_PUT;
        return true;
    }
    if (strcasecmp(method, "PATCH") == 0) {
        *out_method = HTTP_METHOD_PATCH;
        return true;
    }
    if (strcasecmp(method, "DELETE") == 0) {
        *out_method = HTTP_METHOD_DELETE;
        return true;
    }
    if (strcasecmp(method, "HEAD") == 0) {
        *out_method = HTTP_METHOD_HEAD;
        return true;
    }

    return false;
}

static bool cap_web_search_extract_url_host(const char *url, char *host, size_t host_size)
{
    const char *scheme = NULL;
    const char *authority = NULL;
    const char *authority_end = NULL;
    const char *start = NULL;
    const char *at = NULL;
    const char *host_end = NULL;
    size_t host_len = 0;

    if (!url || !host || host_size == 0) {
        return false;
    }
    host[0] = '\0';

    scheme = strstr(url, "://");
    if (!scheme || (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0)) {
        return false;
    }
    authority = scheme + 3;
    authority_end = authority + strcspn(authority, "/?#");
    if (authority >= authority_end) {
        return false;
    }

    start = authority;
    for (const char *p = authority; p < authority_end; p++) {
        if (*p == '@') {
            at = p;
        }
    }
    if (at) {
        start = at + 1;
    }

    if (start >= authority_end) {
        return false;
    }

    if (*start == '[') {
        const char *closing = NULL;
        for (const char *p = start + 1; p < authority_end; p++) {
            if (*p == ']') {
                closing = p;
                break;
            }
        }
        if (!closing || closing == start + 1) {
            return false;
        }
        host_len = (size_t)(closing - (start + 1));
        if (host_len + 1 > host_size) {
            return false;
        }
        memcpy(host, start + 1, host_len);
        host[host_len] = '\0';
        return true;
    }

    host_end = start;
    while (host_end < authority_end && *host_end != ':') {
        host_end++;
    }
    if (host_end <= start) {
        return false;
    }
    host_len = (size_t)(host_end - start);
    if (host_len + 1 > host_size) {
        return false;
    }
    memcpy(host, start, host_len);
    host[host_len] = '\0';
    return true;
}

static bool cap_web_search_host_matches_allowlist_token(const char *host, const char *token)
{
    size_t host_len;
    size_t token_len;

    if (!host || !token || !host[0] || !token[0]) {
        return false;
    }

    host_len = strlen(host);
    token_len = strlen(token);

    // Only "*": matches any host
    if (token_len == 1 && token[0] == '*') {
        return true;
    }

    if (token_len >= 3 && token[0] == '*' && token[1] == '.') {
        const char *base = token + 2;   /* "example.com" */
        const char *suffix = token + 1; /* ".example.com" */
        size_t suffix_len = token_len - 1;
        /* *.example.com matches example.com itself as well as any subdomain. */
        if (strcasecmp(host, base) == 0) {
            return true;
        }
        return host_len > suffix_len &&
               strcasecmp(host + host_len - suffix_len, suffix) == 0;
    }

    if (token[0] == '.') {
        const char *exact = token + 1;
        size_t token_with_dot_len = token_len;
        if (strcasecmp(host, exact) == 0) {
            return true;
        }
        return host_len > token_with_dot_len &&
               strcasecmp(host + host_len - token_with_dot_len, token) == 0;
    }

    return strcasecmp(host, token) == 0;
}

static bool cap_web_search_host_allowed(const char *host)
{
    char allowlist_copy[CAP_WEB_SEARCH_HTTP_ALLOWLIST_MAX];
    char *saveptr = NULL;
    char *token = NULL;

    if (!host || !host[0] || !s_search.http_allowlist[0]) {
        return false;
    }

    strlcpy(allowlist_copy, s_search.http_allowlist, sizeof(allowlist_copy));
    for (token = strtok_r(allowlist_copy, ",", &saveptr);
            token;
            token = strtok_r(NULL, ",", &saveptr)) {
        char *start = token;
        char *end = NULL;

        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
            start++;
        }
        end = start + strlen(start);
        while (end > start &&
                (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
            end--;
        }
        *end = '\0';
        if (!start[0]) {
            continue;
        }
        /* Safe in-place normalization on stack copy for [IPv6] allowlist tokens. */
        if (start[0] == '[' && end > start + 1 && end[-1] == ']') {
            start++;
            end[-1] = '\0';
        }
        if (cap_web_search_host_matches_allowlist_token(host, start)) {
            return true;
        }
    }

    return false;
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

static void cap_web_search_format_brave_results(cJSON *root,
                                                char *output,
                                                size_t output_size)
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

static void cap_web_search_format_tavily_results(cJSON *root,
                                                 char *output,
                                                 size_t output_size)
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

static esp_err_t cap_web_search_brave_direct(const char *url,
                                             cap_web_search_buf_t *buf)
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

static esp_err_t cap_web_search_tavily_direct(const char *query,
                                              cap_web_search_buf_t *buf)
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

static esp_err_t cap_web_search_http_request_execute(const char *input_json,
                                                     const claw_cap_call_context_t *ctx,
                                                     char *output,
                                                     size_t output_size)
{
    cJSON *input = NULL;
    cJSON *url_item = NULL;
    cJSON *method_item = NULL;
    cJSON *headers_item = NULL;
    cJSON *body_item = NULL;
    cJSON *timeout_item = NULL;
    cJSON *max_body_item = NULL;
    cap_web_search_buf_t buf = {0};
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client = NULL;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    esp_err_t err = ESP_OK;
    int status = 0;
    int timeout_ms = CAP_WEB_SEARCH_HTTP_TIMEOUT_MS_DEFAULT;
    int max_body_bytes = CAP_WEB_SEARCH_HTTP_MAX_BODY_DEFAULT;
    char host[128] = {0};

    (void)ctx;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_search.http_allowlist[0]) {
        snprintf(output, output_size,
                 "Error: HTTP allowlist is empty; configure search_http_allowlist first");
        return ESP_ERR_INVALID_STATE;
    }

    input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    url_item = cJSON_GetObjectItem(input, "url");
    method_item = cJSON_GetObjectItem(input, "method");
    headers_item = cJSON_GetObjectItem(input, "headers");
    body_item = cJSON_GetObjectItem(input, "body");
    timeout_item = cJSON_GetObjectItem(input, "timeout_ms");
    max_body_item = cJSON_GetObjectItem(input, "max_body_bytes");

    if (!cJSON_IsString(url_item) || !url_item->valuestring || !url_item->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing url");
        return ESP_ERR_INVALID_ARG;
    }
    if (strncasecmp(url_item->valuestring, "http://", 7) != 0 &&
            strncasecmp(url_item->valuestring, "https://", 8) != 0) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: url must start with http:// or https://");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_web_search_extract_url_host(url_item->valuestring, host, sizeof(host))) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: invalid url host");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_web_search_host_allowed(host)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: host '%s' is not in allowlist", host);
        return ESP_ERR_INVALID_STATE;
    }
    if (method_item && (!cJSON_IsString(method_item) ||
                        !cap_web_search_is_valid_http_method(method_item->valuestring, &method))) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: method must be GET/POST/PUT/PATCH/DELETE/HEAD");
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_item) {
        if (!cJSON_IsNumber(timeout_item) ||
                timeout_item->valueint <= 0 ||
                timeout_item->valueint > 120000) {
            cJSON_Delete(input);
            snprintf(output, output_size, "Error: timeout_ms must be an integer in [1, 120000]");
            return ESP_ERR_INVALID_ARG;
        }
        timeout_ms = timeout_item->valueint;
    }
    if (max_body_item) {
        if (!cJSON_IsNumber(max_body_item) ||
                max_body_item->valueint <= 0 ||
                max_body_item->valueint > CAP_WEB_SEARCH_HTTP_MAX_BODY_LIMIT) {
            cJSON_Delete(input);
            snprintf(output,
                     output_size,
                     "Error: max_body_bytes must be an integer in [1, %d]",
                     CAP_WEB_SEARCH_HTTP_MAX_BODY_LIMIT);
            return ESP_ERR_INVALID_ARG;
        }
        max_body_bytes = max_body_item->valueint;
    }
    if (body_item && !cJSON_IsString(body_item)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: body must be a string");
        return ESP_ERR_INVALID_ARG;
    }
    if (headers_item && !cJSON_IsObject(headers_item)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: headers must be an object");
        return ESP_ERR_INVALID_ARG;
    }
    if (body_item &&
            (method == HTTP_METHOD_GET || method == HTTP_METHOD_HEAD) &&
            body_item->valuestring &&
            body_item->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: GET/HEAD does not accept body");
        return ESP_ERR_INVALID_ARG;
    }

    buf.cap = ((size_t)max_body_bytes + 1 < 4096) ? (size_t)max_body_bytes + 1 : 4096;
    buf.data = calloc(1, buf.cap);
    if (!buf.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    buf.max_len = (size_t)max_body_bytes;
    buf.check_redirect_allowlist = true;

    config.url = url_item->valuestring;
    config.event_handler = cap_web_search_http_event_handler;
    config.user_data = &buf;
    config.timeout_ms = timeout_ms;
    config.buffer_size = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;

    client = esp_http_client_init(&config);
    if (!client) {
        cJSON_Delete(input);
        free(buf.data);
        snprintf(output, output_size, "Error: failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, method);
    if (headers_item) {
        cJSON *header = NULL;

        cJSON_ArrayForEach(header, headers_item) {
            if (header->string && cJSON_IsString(header) && header->valuestring) {
                esp_http_client_set_header(client, header->string, header->valuestring);
            }
        }
    }
    if (body_item && body_item->valuestring) {
        esp_http_client_set_post_field(client, body_item->valuestring, strlen(body_item->valuestring));
    }

    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_Delete(input);

    if (err != ESP_OK) {
        free(buf.data);
        snprintf(output, output_size, "Error: http request failed (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output,
             output_size,
             "HTTP %d%s\n%s",
             status,
             buf.truncated ? " (body truncated)" : "",
             buf.data ? buf.data : "");
    free(buf.data);
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
    {
        .id = "http_request",
        .name = "http_request",
        .family = "system",
        .description = "Call a standard HTTP endpoint with allowlist protection.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},"
        "\"method\":{\"type\":\"string\",\"enum\":[\"GET\",\"POST\",\"PUT\",\"PATCH\",\"DELETE\",\"HEAD\"]},"
        "\"headers\":{\"type\":\"object\",\"additionalProperties\":{\"type\":\"string\"}},"
        "\"body\":{\"type\":\"string\"},"
        "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120000},"
        "\"max_body_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65535}},"
        "\"required\":[\"url\"]}",
        .execute = cap_web_search_http_request_execute,
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

esp_err_t cap_web_search_set_http_allowlist(const char *allowlist_csv)
{
    if (!allowlist_csv) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_search.http_allowlist, allowlist_csv, sizeof(s_search.http_allowlist));
    return ESP_OK;
}
