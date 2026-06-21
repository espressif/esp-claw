/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "cap_im_local.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "http_webim";

#define WEB_IM_CHANNEL       "web"
#define WEBIM_CHAT_ID_MAX    88
#define WEBIM_FILE_PREFIX    "/inbox/webim/"
#define WEBIM_LINKS_MAX      4
#define WEBIM_URL_MAX        192
#define WEBIM_WS_MAX_CLIENTS 8

static httpd_handle_t      s_httpd;
static SemaphoreHandle_t   s_ws_mx;
static int                 s_ws_fds[WEBIM_WS_MAX_CLIENTS];
static size_t              s_ws_count;
static uint32_t            s_evt_seq;
static bool                s_webim_bound;

typedef struct {
    char *json;
    size_t len;
    int fds[WEBIM_WS_MAX_CLIENTS];
    size_t fd_count;
} webim_ws_broadcast_job_t;

static esp_err_t webim_ws_mx_ensure(void);
static void webim_ws_fd_remove(int fd);
static esp_err_t webim_ws_queue_json_to_fds(const int *fds, size_t fd_count, const char *json);

static void webim_ws_broadcast_job_run(void *arg)
{
    webim_ws_broadcast_job_t *job = arg;
    httpd_ws_frame_t pkt;

    if (!job) {
        return;
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = (uint8_t *)job->json;
    pkt.len = job->len;

    for (size_t i = 0; i < job->fd_count; i++) {
        esp_err_t send_err = httpd_ws_send_frame_async(s_httpd, job->fds[i], &pkt);

        if (send_err != ESP_OK) {
            webim_ws_fd_remove(job->fds[i]);
            ESP_LOGW(TAG, "WS async drop fd=%d (%s)", job->fds[i], esp_err_to_name(send_err));
        } else {
            ESP_LOGI(TAG, "WS async sent fd=%d ok", job->fds[i]);
        }
    }

    free(job->json);
    free(job);
}

static esp_err_t webim_ws_mx_ensure(void)
{
    if (s_ws_mx) {
        return ESP_OK;
    }
    s_ws_mx = xSemaphoreCreateMutex();
    return s_ws_mx ? ESP_OK : ESP_ERR_NO_MEM;
}

static void webim_ws_gc_locked(void)
{
    size_t write = 0;

    if (!s_httpd) {
        return;
    }
    for (size_t i = 0; i < s_ws_count; i++) {
        httpd_ws_client_info_t info = httpd_ws_get_fd_info(s_httpd, s_ws_fds[i]);

        if (info == HTTPD_WS_CLIENT_WEBSOCKET) {
            s_ws_fds[write++] = s_ws_fds[i];
        } else {
            ESP_LOGW(TAG, "WS gc: pruned stale fd=%d", s_ws_fds[i]);
        }
    }
    s_ws_count = write;
}

static void webim_ws_fd_add(int fd)
{
    size_t i;

    if (fd < 0 || webim_ws_mx_ensure() != ESP_OK) {
        return;
    }
    xSemaphoreTake(s_ws_mx, portMAX_DELAY);
    for (i = 0; i < s_ws_count; i++) {
        if (s_ws_fds[i] == fd) {
            xSemaphoreGive(s_ws_mx);
            return;
        }
    }
    if (s_ws_count >= WEBIM_WS_MAX_CLIENTS) {
        webim_ws_gc_locked();
    }
    if (s_ws_count < WEBIM_WS_MAX_CLIENTS) {
        s_ws_fds[s_ws_count++] = fd;
        ESP_LOGI(TAG, "WS client fd=%d (n=%u)", fd, (unsigned)s_ws_count);
    } else {
        ESP_LOGW(TAG, "WS client fd=%d rejected: max clients reached", fd);
    }
    xSemaphoreGive(s_ws_mx);
}

static void webim_ws_fd_remove(int fd)
{
    size_t i;

    if (fd < 0 || !s_ws_mx) {
        return;
    }
    xSemaphoreTake(s_ws_mx, portMAX_DELAY);
    for (i = 0; i < s_ws_count; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = s_ws_fds[s_ws_count - 1];
            s_ws_count--;
            ESP_LOGD(TAG, "WS client removed fd=%d (n=%u)", fd, (unsigned)s_ws_count);
            break;
        }
    }
    xSemaphoreGive(s_ws_mx);
}

void http_server_webim_ws_fd_remove(int fd)
{
    webim_ws_fd_remove(fd);
}

static void webim_ws_broadcast_json(const char *json)
{
    int local_fds[WEBIM_WS_MAX_CLIENTS];
    size_t local_count = 0;

    if (!json || !s_httpd || !s_ws_mx) {
        return;
    }

    xSemaphoreTake(s_ws_mx, portMAX_DELAY);
    webim_ws_gc_locked();
    local_count = s_ws_count;
    memcpy(local_fds, s_ws_fds, local_count * sizeof(int));
    xSemaphoreGive(s_ws_mx);

    if (local_count == 0) {
        ESP_LOGW(TAG, "WS broadcast skipped: no connected clients");
        return;
    }

    ESP_LOGI(TAG, "WS broadcast: %u client(s) len=%u", (unsigned)local_count, (unsigned)strlen(json));
    (void)webim_ws_queue_json_to_fds(local_fds, local_count, json);
}

static esp_err_t webim_ws_queue_json_to_fds(const int *fds, size_t fd_count, const char *json)
{
    webim_ws_broadcast_job_t *job = NULL;
    esp_err_t err;

    if (!json || !json[0] || !fds || fd_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    job = calloc(1, sizeof(*job));
    if (!job) {
        ESP_LOGW(TAG, "WS queue skipped: no memory for job");
        return ESP_ERR_NO_MEM;
    }

    job->json = strdup(json);
    if (!job->json) {
        ESP_LOGW(TAG, "WS queue skipped: no memory for payload");
        free(job);
        return ESP_ERR_NO_MEM;
    }

    job->len = strlen(job->json);
    job->fd_count = fd_count;
    memcpy(job->fds, fds, fd_count * sizeof(int));

    err = httpd_queue_work(s_httpd, webim_ws_broadcast_job_run, job);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS queue failed: %s", esp_err_to_name(err));
        free(job->json);
        free(job);
    }
    return err;
}

static esp_err_t webim_emit_outbound_json(const cap_im_local_message_t *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *links = NULL;
    char *payload = NULL;

    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    s_evt_seq++;
    cJSON_AddNumberToObject(root, "seq", (double)s_evt_seq);
    http_server_json_add_string(root, "chat_id", message->chat_id ? message->chat_id : "");
    http_server_json_add_string(root, "role", "assistant");
    http_server_json_add_string(root, "text", message->text ? message->text : "");
    cJSON_AddNumberToObject(root, "ts_ms", (double)(esp_timer_get_time() / 1000LL));

    if (message->link_url && message->link_url[0]) {
        links = cJSON_CreateArray();
        if (links) {
            cJSON *link = cJSON_CreateObject();
            if (link) {
                http_server_json_add_string(link, "url", message->link_url);
                http_server_json_add_string(link,
                                            "label",
                                            (message->link_label && message->link_label[0]) ? message->link_label : "");
                cJSON_AddItemToArray(links, link);
            }
            cJSON_AddItemToObject(root, "links", links);
        }
    }

    payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    webim_ws_broadcast_json(payload);
    free(payload);
    return ESP_OK;
}

static esp_err_t webim_outbound_cb(const cap_im_local_message_t *message, void *user_ctx)
{
    (void)user_ctx;
    if (!message || !message->chat_id || !message->chat_id[0]) {
        ESP_LOGD(TAG, "outbound_cb: dropped (no message or chat_id)");
        return ESP_OK;
    }
    if (strcmp(message->channel, WEB_IM_CHANNEL) != 0) {
        ESP_LOGD(TAG, "outbound_cb: dropped channel=%s (expected %s)",
                 message->channel, WEB_IM_CHANNEL);
        return ESP_OK;
    }
    return webim_emit_outbound_json(message);
}

esp_err_t http_server_webim_bind_im(void)
{
    esp_err_t err = cap_im_local_set_outbound_callback(webim_outbound_cb, NULL);

    if (err == ESP_OK) {
        s_webim_bound = true;
        ESP_LOGI(TAG, "Web IM outbound -> WebSocket (no server-side history)");
    } else {
        ESP_LOGW(TAG, "Web IM bind failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool webim_path_allowed_upload(const char *path)
{
    if (!http_server_path_is_safe(path)) {
        return false;
    }
    return strncmp(path, WEBIM_FILE_PREFIX, strlen(WEBIM_FILE_PREFIX)) == 0;
}

static void webim_build_file_url(const char *storage_path, char *out, size_t out_sz)
{
    if (!storage_path || storage_path[0] != '/') {
        strlcpy(out, "", out_sz);
        return;
    }
    snprintf(out, out_sz, "/files%s", storage_path);
}

static esp_err_t webim_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "bound", s_webim_bound);
    return http_server_send_json_response(req, root);
}

static esp_err_t webim_send_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    const char *chat_id = NULL;
    const char *text = NULL;
    cJSON *files = NULL;
    const char *emit_links[WEBIM_LINKS_MAX];
    char link_bufs[WEBIM_LINKS_MAX][WEBIM_URL_MAX];
    char link_lbl[WEBIM_LINKS_MAX][48];
    int nlinks = 0;
    esp_err_t err;

    if (!s_webim_bound) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Web IM not ready");
    }

    err = http_server_parse_json_body(req, &root);
    if (err != ESP_OK || !root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    chat_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "chat_id"));
    text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "text"));
    files = cJSON_GetObjectItemCaseSensitive(root, "files");

    if (!chat_id || !chat_id[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing chat_id");
    }
    if ((!text || !text[0]) && (!cJSON_IsArray(files) || cJSON_GetArraySize(files) == 0)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "text or files required");
    }

    if (cJSON_IsArray(files)) {
        int n = cJSON_GetArraySize(files);
        for (int i = 0; i < n && nlinks < WEBIM_LINKS_MAX; i++) {
            cJSON *it = cJSON_GetArrayItem(files, i);
            const char *p = cJSON_IsString(it) ? it->valuestring : NULL;
            const char *bn = NULL;

            if (!p || !webim_path_allowed_upload(p)) {
                cJSON_Delete(root);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid files path");
            }
            webim_build_file_url(p, link_bufs[nlinks], sizeof(link_bufs[nlinks]));
            bn = strrchr(p, '/');
            bn = (bn && bn[1]) ? bn + 1 : p;
            strlcpy(link_lbl[nlinks], bn, sizeof(link_lbl[nlinks]));
            emit_links[nlinks] = link_bufs[nlinks];
            nlinks++;
        }
    }

    err = cap_im_local_emit_user_message(WEB_IM_CHANNEL,
                                         chat_id,
                                         "web_user",
                                         NULL,
                                         text ? text : "",
                                         nlinks > 0 ? emit_links : NULL,
                                         (size_t)nlinks);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "emit_user_message failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "IM gateway not ready");
    }

    {
        cJSON *resp = cJSON_CreateObject();

        if (!resp) {
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(resp, "ok", true);
        return http_server_send_json_response(req, resp);
    }
}

static esp_err_t webim_ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t ws_pkt;
    httpd_ws_frame_t pong_pkt;
    uint8_t *buf = NULL;
    esp_err_t ret;
    int fd;

    fd = httpd_req_to_sockfd(req);

    /* First invocation is the HTTP GET upgrade handshake; no WS frame yet. */
    if (req->method == HTTP_GET) {
        webim_ws_fd_add(fd);
        ESP_LOGI(TAG, "WS client connected fd=%d", fd);
        return ESP_OK;
    }

    webim_ws_fd_add(fd);

    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws recv meta failed: %s", esp_err_to_name(ret));
        webim_ws_fd_remove(fd);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        webim_ws_fd_remove(fd);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        memset(&pong_pkt, 0, sizeof(pong_pkt));
        pong_pkt.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &pong_pkt);
    }

    if (ws_pkt.len > 0) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        free(buf);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t http_server_register_webim_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/webim/status", .method = HTTP_GET, .handler = webim_status_handler },
        { .uri = "/api/webim/send", .method = HTTP_POST, .handler = webim_send_handler },
        {
            .uri = "/ws/webim",
            .method = HTTP_GET,
            .handler = webim_ws_handler,
            .user_ctx = NULL,
            .is_websocket = true,
        },
    };

    s_httpd = server;
    (void)webim_ws_mx_ensure();

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
