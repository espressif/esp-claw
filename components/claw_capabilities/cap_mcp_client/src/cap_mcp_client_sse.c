/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCP Streamable-HTTP / SSE transport client.
 * Uses esp_http_client_perform() with event handler to capture the
 * SSE response stream that the server sends as the HTTP response body.
 */
#include "cap_mcp_client_sse.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "mcp_client_sse";

#define SSE_BUF_SIZE           8192
#define SSE_GET_TIMEOUT_MS     5000   /* SSE GET must complete quickly */
#define SSE_POST_TIMEOUT_MS    5000   /* SSE POST must complete quickly */

/* ─── Event-handler based capture ──────────────────────────────────── */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sse_cap_t;

static esp_err_t sse_cap_handler(esp_http_client_event_t *event)
{
    sse_cap_t *c = (sse_cap_t *)event->user_data;
    if (!c || event->event_id != HTTP_EVENT_ON_DATA || !c->buf) return ESP_OK;

    /* Grow buffer if needed */
    if (c->len + event->data_len + 1 > c->cap) {
        size_t need = c->len + event->data_len + 1;
        size_t nc = (c->cap < need) ? need : c->cap * 2;
        char *nb = realloc(c->buf, nc);
        if (!nb) return ESP_ERR_NO_MEM;
        c->buf = nb;
        c->cap = nc;
    }
    memcpy(c->buf + c->len, event->data, event->data_len);
    c->len += event->data_len;
    c->buf[c->len] = '\0';
    return ESP_OK;
}

/* ─── SSE line parser ──────────────────────────────────────────────── */

/** Return length of next line (without \r\n), advance *cursor past \n. */
static int sse_next_line(const char **cursor)
{
    const char *s = *cursor;
    if (!s || !*s) return -1;
    const char *nl = strchr(s, '\n');
    int len;
    if (nl) {
        len = (int)(nl - s);
        if (len > 0 && nl[-1] == '\r') len--;
        *cursor = nl + 1;
    } else {
        len = (int)strlen(s);
        *cursor = s + len;
    }
    return len;
}

/**
 * @brief Extract the data payload of the first SSE event matching @p event_name.
 */
static esp_err_t sse_find_event(const char *body, size_t body_len,
                                const char *event_name,
                                char *data_out, size_t data_size)
{
    const char *cur = body;
    char ev[64] = "";
    char dt[SSE_BUF_SIZE] = "";
    bool in = false;

    data_out[0] = '\0';

    while (cur < body + body_len) {
        const char *line_start = cur;
        int len = sse_next_line(&cur);
        if (len < 0) break;

        if (len == 0) {
            if (in && strcmp(ev, event_name) == 0) {
                strlcpy(data_out, dt, data_size);
                return ESP_OK;
            }
            ev[0] = '\0'; dt[0] = '\0'; in = false;
            continue;
        }
        in = true;
        if (len > 6 && strncmp(line_start, "event:", 6) == 0) {
            const char *v = line_start + 6;
            while (*v == ' ' || *v == '\t') v++;
            strlcpy(ev, v, sizeof(ev));
        } else if (len > 5 && strncmp(line_start, "data:", 5) == 0) {
            const char *v = line_start + 5;
            while (*v == ' ' || *v == '\t') v++;
            if (dt[0]) strlcat(dt, "\n", sizeof(dt));
            strlcat(dt, v, sizeof(dt));
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ─── Public API ───────────────────────────────────────────────────── */

esp_err_t cap_mcp_sse_connect(const char *sse_url,
                              const char *auth_token,
                              cap_mcp_sse_ctx_t **ctx_out,
                              char *post_url_out,
                              size_t post_url_size)
{
    (void)ctx_out;
    if (!sse_url || !post_url_out || post_url_size == 0)
        return ESP_ERR_INVALID_ARG;
    post_url_out[0] = '\0';

    sse_cap_t c = { .buf = calloc(1, 2048), .len = 0, .cap = 2048 };
    if (!c.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = sse_url,
        .method = HTTP_METHOD_GET,
        .event_handler = sse_cap_handler,
        .user_data = &c,
        .timeout_ms = SSE_GET_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { free(c.buf); return ESP_ERR_NO_MEM; }

    esp_http_client_set_header(cl, "Accept", "text/event-stream");
    if (auth_token && auth_token[0]) {
        char bh[160];
        snprintf(bh, sizeof(bh), "Bearer %s", auth_token);
        esp_http_client_set_header(cl, "Authorization", bh);
    }

    esp_err_t err = esp_http_client_perform(cl);
    int st = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);

    /* Accept timeout as success if we captured SSE data */
    if (err == ESP_ERR_HTTP_EAGAIN && c.len > 0) {
        ESP_LOGI(TAG, "SSE GET timeout but captured %u bytes", (unsigned)c.len);
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSE GET err=%s len=%u", esp_err_to_name(err), (unsigned)c.len);
        free(c.buf); return err;
    }
    if (st != 200 && st != 0) {  /* st=0 when timeout */
        ESP_LOGW(TAG, "SSE GET HTTP %d", st);
        free(c.buf); return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "SSE GET ok, body=%u bytes", (unsigned)c.len);

    /* Extract endpoint event */
    err = sse_find_event(c.buf, c.len, "endpoint", post_url_out, post_url_size);
    if (err != ESP_OK) {
        const char *ds = strstr(c.buf, "data:");
        if (ds) {
            ds += 5; while (*ds == ' ') ds++;
            const char *nl = strchr(ds, '\n');
            size_t cp = nl ? (size_t)(nl - ds) : strlen(ds);
            if (cp >= post_url_size) cp = post_url_size - 1;
            memcpy(post_url_out, ds, cp);
            post_url_out[cp] = '\0';
            err = ESP_OK;
        }
    }

    if (post_url_out[0])
        ESP_LOGI(TAG, "SSE endpoint: %s", post_url_out);
    else
        ESP_LOGW(TAG, "SSE body: %.*s", (int)(c.len > 200 ? 200 : c.len), c.buf);

    free(c.buf);
    return (post_url_out[0] ? ESP_OK : ESP_ERR_NOT_FOUND);
}

esp_err_t cap_mcp_sse_request(cap_mcp_sse_ctx_t *ctx,
                              const char *post_url,
                              const char *auth_token,
                              const char *json_rpc_body,
                              char *response,
                              size_t response_size)
{
    (void)ctx;
    if (!post_url || !json_rpc_body || !response || response_size == 0)
        return ESP_ERR_INVALID_ARG;
    response[0] = '\0';

    sse_cap_t c = { .buf = calloc(1, 2048), .len = 0, .cap = 2048 };
    if (!c.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = post_url,
        .method = HTTP_METHOD_POST,
        .event_handler = sse_cap_handler,
        .user_data = &c,
        .timeout_ms = SSE_POST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { free(c.buf); return ESP_ERR_NO_MEM; }

    esp_http_client_set_header(cl, "Content-Type", "application/json");
    esp_http_client_set_header(cl, "Accept", "application/json, text/event-stream");
    if (auth_token && auth_token[0]) {
        char bh[160];
        snprintf(bh, sizeof(bh), "Bearer %s", auth_token);
        esp_http_client_set_header(cl, "Authorization", bh);
    }
    esp_http_client_set_post_field(cl, json_rpc_body, (int)strlen(json_rpc_body));

    esp_err_t err = esp_http_client_perform(cl);
    int st = esp_http_client_get_status_code(cl);
    esp_http_client_cleanup(cl);

    /* Accept timeout as success if we captured SSE data */
    if (err == ESP_ERR_HTTP_EAGAIN && c.len > 0) {
        ESP_LOGD(TAG, "SSE POST timeout but captured %u bytes", (unsigned)c.len);
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSE POST err=%s len=%u", esp_err_to_name(err), (unsigned)c.len);
        free(c.buf); return err;
    }
    if (st != 200 && st != 0) {
        ESP_LOGW(TAG, "SSE POST HTTP %d", st);
        free(c.buf); return ESP_ERR_INVALID_RESPONSE;
    }

    /* Extract "message" event (JSON-RPC response) */
    err = sse_find_event(c.buf, c.len, "message", response, response_size);
    if (err != ESP_OK) {
        if (c.len > 0 && c.buf[0] == '{') {
            strlcpy(response, c.buf, response_size);
            err = ESP_OK;
        } else {
            ESP_LOGW(TAG, "SSE body: %.*s",
                     (int)(c.len > 256 ? 256 : c.len), c.buf);
        }
    }

    free(c.buf);
    return err;
}

void cap_mcp_sse_disconnect(cap_mcp_sse_ctx_t *ctx)
{
    free(ctx);
}
