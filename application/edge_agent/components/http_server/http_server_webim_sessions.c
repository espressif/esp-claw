/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "claw_session_mgr.h"
#include "claw_agent_mgr.h"
#include "claw_memory.h"
#include "claw_event_router.h"
#include "esp_log.h"

#define SESSION_URI "/api/webim/sessions"
#define WEBIM_MESSAGE_MAX 8192

esp_err_t http_server_webim_error(httpd_req_t *req, esp_err_t err)
{
    const char *status = "500 Internal Server Error", *code = "storage_error";
    if (err == ESP_ERR_NOT_FOUND) { status = "404 Not Found"; code = "session_not_found"; }
    else if (err == ESP_ERR_INVALID_ARG) { status = "400 Bad Request"; code = "invalid_request"; }
    else if (err == ESP_ERR_INVALID_SIZE) { status = "413 Content Too Large"; code = "content_too_large"; }
    else if (err == ESP_ERR_INVALID_STATE) { status = "409 Conflict"; code = "session_busy"; }
    else if (err == ESP_ERR_NOT_SUPPORTED) { status = "409 Conflict"; code = "channel_unavailable"; }
    else if (err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) { status = "503 Service Unavailable"; code = "busy_or_full"; }
    cJSON *body = cJSON_CreateObject();
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    cJSON_AddStringToObject(body, "error", code);
    cJSON_AddStringToObject(body, "message", code);
    httpd_resp_set_status(req, status);
    ESP_LOGW("http_sessions", "%s: %s", req->uri, esp_err_to_name(err));
    return http_server_send_json_response(req, body);
}

static const char *reply_channel(const claw_session_info_t *info)
{
    return strcmp(info->channel, "local") == 0 ? "web" : info->channel;
}

static bool can_send(const claw_session_info_t *info)
{
    return info->agent_id == 0 && claw_event_router_channel_is_bound(reply_channel(info));
}

static cJSON *session_json(const claw_session_info_t *info)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return NULL;
    cJSON_AddStringToObject(item, "session", info->id);
    cJSON_AddStringToObject(item, "title", info->title);
    cJSON_AddStringToObject(item, "source", info->channel);
    cJSON_AddStringToObject(item, "chat_id", info->chat_id);
    cJSON_AddStringToObject(item, "alias", info->alias);
    cJSON_AddNumberToObject(item, "activity_order", info->activity);
    cJSON_AddStringToObject(item, "delivery_state", info->delivery_state);
    cJSON_AddStringToObject(item, "reply_channel", reply_channel(info));
    cJSON_AddBoolToObject(item, "can_send", can_send(info));
    cJSON_AddStringToObject(item, "run_state", claw_agent_mgr_session_state(info->session_id));
    cJSON_AddStringToObject(item, "boot_id", http_server_ctx()->boot_id);
    return item;
}

static esp_err_t query_number(httpd_req_t *req, const char *key, size_t fallback, size_t max, size_t *out)
{
    char buf[24];
    *out = fallback;
    if (http_server_query_get(req, key, buf, sizeof(buf)) != ESP_OK) return ESP_OK;
    if (!buf[0]) return ESP_ERR_INVALID_ARG;
    for (const char *p = buf; *p; p++) if (!isdigit((unsigned char)*p)) return ESP_ERR_INVALID_ARG;
    char *end;
    unsigned long value = strtoul(buf, &end, 10);
    if (*end || value > max) return ESP_ERR_INVALID_ARG;
    *out = value;
    return ESP_OK;
}

static esp_err_t sessions_handler(httpd_req_t *req)
{
    claw_session_info_t *info = calloc(1, sizeof(*info));
    cJSON *result = NULL;
    if (!info) return http_server_webim_error(req, ESP_ERR_NO_MEM);
    esp_err_t err = ESP_OK;
    if (req->method == HTTP_POST) {
        err = claw_session_mgr_catalog_create(info);
        if (err == ESP_OK) { result = session_json(info); httpd_resp_set_status(req, "201 Created"); }
    } else {
        size_t offset = 0, limit = 30;
        char source[16] = "all";
        http_server_query_get(req, "source", source, sizeof(source));
        err = query_number(req, "cursor", 0, 100000, &offset);
        if (err == ESP_OK) err = query_number(req, "limit", 30, 50, &limit);
        if (err == ESP_OK) err = claw_session_mgr_catalog_list(source, offset, limit, &result);
        cJSON *items = cJSON_GetObjectItem(result, "items");
        for (cJSON *item = items ? items->child : NULL; item;) {
            cJSON *next = item->next;
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "session"));
            err = claw_session_mgr_catalog_get(id, info);
            if (err != ESP_OK) break;
            if (!info->title[0]) {
                cJSON *history = NULL;
                esp_err_t read_err = claw_memory_read_session_messages(info->session_id, 0, 30, &history);
                if (read_err == ESP_OK && cJSON_GetNumberValue(cJSON_GetObjectItem(history, "total")) == 0) {
                    cJSON_Delete(history);
                    cJSON_Delete(cJSON_DetachItemViaPointer(items, item));
                    item = next;
                    continue;
                }
                cJSON *message;
                cJSON_ArrayForEach(message, cJSON_GetObjectItem(history, "messages")) {
                    const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(message, "role"));
                    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(message, "text"));
                    if (!role || strcmp(role, "user") != 0 || !text || !text[0]) continue;
                    size_t n = strnlen(text, sizeof(info->title) - 1);
                    while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
                    memcpy(info->title, text, n); info->title[n] = 0;
                    if (claw_session_mgr_catalog_rename(id, info->title) != ESP_OK) ESP_LOGW("http_sessions", "Cannot save history title");
                    cJSON_ReplaceItemInObject(item, "title", cJSON_CreateString(info->title));
                    break;
                }
                cJSON_Delete(history);
            }
            cJSON_AddStringToObject(item, "reply_channel", reply_channel(info));
            cJSON_AddBoolToObject(item, "can_send", can_send(info));
            cJSON_AddStringToObject(item, "run_state", claw_agent_mgr_session_state(info->session_id));
            item = next;
        }
        if (result) cJSON_AddStringToObject(result, "boot_id", http_server_ctx()->boot_id);
    }
    free(info);
    if (err != ESP_OK || !result) { cJSON_Delete(result); return http_server_webim_error(req, err != ESP_OK ? err : ESP_ERR_NO_MEM); }
    return http_server_send_json_response(req, result);
}

static esp_err_t session_handler(httpd_req_t *req)
{
    const char *path = req->uri + strlen(SESSION_URI) + 1;
    size_t length = strcspn(path, "/?");
    if (length != CLAW_SESSION_PUBLIC_ID_SIZE - 1) return http_server_webim_error(req, ESP_ERR_INVALID_ARG);
    char id[CLAW_SESSION_PUBLIC_ID_SIZE];
    memcpy(id, path, length); id[length] = 0;
    for (size_t i = 0; i < length; i++) if (!isxdigit((unsigned char)id[i])) return http_server_webim_error(req, ESP_ERR_INVALID_ARG);
    const char *suffix = path + length;
    bool messages = strncmp(suffix, "/messages", 9) == 0 && (suffix[9] == 0 || suffix[9] == '?');
    if (!messages && *suffix && *suffix != '?') return http_server_webim_error(req, ESP_ERR_NOT_FOUND);
    claw_session_info_t *info = calloc(1, sizeof(*info));
    if (!info) return http_server_webim_error(req, ESP_ERR_NO_MEM);
    esp_err_t err = claw_session_mgr_catalog_get(id, info);
    cJSON *result = NULL, *body = NULL;
    if (err == ESP_OK && messages && req->method == HTTP_GET) {
        size_t before = 0, limit = 30;
        err = query_number(req, "before", 0, 1000000, &before);
        if (err == ESP_OK) err = query_number(req, "limit", 30, 50, &limit);
        if (err == ESP_OK) err = claw_memory_read_session_messages(info->session_id, before, limit, &result);
        if (err == ESP_OK) {
            cJSON_AddStringToObject(result, "delivery_state", info->delivery_state);
            cJSON_AddStringToObject(result, "run_state", claw_agent_mgr_session_state(info->session_id));
            cJSON_AddStringToObject(result, "boot_id", http_server_ctx()->boot_id);
        }
    } else if (err == ESP_OK && !messages) {
        if (req->method == HTTP_PATCH) {
            err = http_server_parse_json_body(req, &body);
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(body, "title"));
            if (err == ESP_OK) err = claw_session_mgr_catalog_rename(id, title);
            if (err == ESP_OK) { strlcpy(info->title, title, sizeof(info->title)); result = session_json(info); }
        } else if (req->method == HTTP_DELETE) {
            err = claw_agent_mgr_delete_session(id);
            if (err == ESP_OK) { result = cJSON_CreateObject(); cJSON_AddBoolToObject(result, "ok", true); }
        } else result = session_json(info);
    } else if (err == ESP_OK) err = ESP_ERR_INVALID_ARG;
    free(info); cJSON_Delete(body);
    if (err != ESP_OK || !result) { cJSON_Delete(result); return http_server_webim_error(req, err != ESP_OK ? err : ESP_ERR_NO_MEM); }
    return http_server_send_json_response(req, result);
}

esp_err_t http_server_webim_send_session(httpd_req_t *req)
{
    cJSON *body = NULL;
    esp_err_t err = http_server_parse_json_body(req, &body);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(body, "session"));
    const char *message_id = cJSON_GetStringValue(cJSON_GetObjectItem(body, "client_message_id"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(body, "text"));
    cJSON *files = cJSON_GetObjectItem(body, "files");
    char *combined = calloc(1, WEBIM_MESSAGE_MAX + 1);
    claw_session_info_t *info = calloc(1, sizeof(*info));
    if (!combined || !info) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && (!id || strlen(id) != 32 || !message_id || !message_id[0] || strlen(message_id) > 64 || !text)) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && strlen(text) > WEBIM_MESSAGE_MAX) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) strlcpy(combined, text, WEBIM_MESSAGE_MAX + 1);
    if (err == ESP_OK && files && (!cJSON_IsArray(files) || cJSON_GetArraySize(files) > 4)) err = ESP_ERR_INVALID_ARG;
    cJSON *file;
    cJSON_ArrayForEach(file, files) {
        if (err != ESP_OK) break;
        const char *path = cJSON_GetStringValue(file);
        if (!path || strncmp(path, "/inbox/webim/", 13) != 0 || !http_server_path_is_safe(path)) { err = ESP_ERR_INVALID_ARG; break; }
        size_t used = strlen(combined);
        int n = snprintf(combined + used, WEBIM_MESSAGE_MAX + 1 - used, "\n/files%s", path);
        if (n < 0 || n >= WEBIM_MESSAGE_MAX + 1 - used) err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && !combined[0]) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) err = claw_session_mgr_catalog_get(id, info);
    if (err == ESP_OK && !can_send(info)) err = ESP_ERR_NOT_SUPPORTED;
    http_server_ctx_t *ctx = http_server_ctx();
    claw_core_message_receipt_t receipt = {0};
    bool duplicate = false;
    if (err == ESP_OK) {
        for (size_t i = 0; i < HTTP_WEBIM_RECEIPTS; i++) {
            http_webim_receipt_t *old = &ctx->webim_receipts[i];
            if (strcmp(old->message_id, message_id) != 0) continue;
            duplicate = true;
            if (strcmp(old->session, id) != 0 || !old->text || strcmp(old->text, combined) != 0) err = ESP_ERR_INVALID_STATE;
            else receipt.run_id = old->run_id;
            break;
        }
    }
    if (err == ESP_OK && !duplicate) {
        err = claw_agent_mgr_post_session_message(id, combined, message_id, strcmp(reply_channel(info), "web") != 0, &receipt);
        if (err == ESP_OK) {
            http_webim_receipt_t *slot = &ctx->webim_receipts[ctx->webim_receipt_next++ % HTTP_WEBIM_RECEIPTS];
            free(slot->text); slot->text = combined; combined = NULL;
            strlcpy(slot->message_id, message_id, sizeof(slot->message_id));
            strlcpy(slot->session, id, sizeof(slot->session));
            slot->run_id = receipt.run_id;
        }
    }
    cJSON *result = NULL;
    if (err == ESP_OK) {
        result = cJSON_CreateObject();
        cJSON_AddNumberToObject(result, "run_id", receipt.run_id);
        cJSON_AddStringToObject(result, "client_message_id", message_id);
        cJSON_AddStringToObject(result, "state", "queued");
        cJSON_AddStringToObject(result, "boot_id", ctx->boot_id);
        httpd_resp_set_status(req, "202 Accepted");
    }
    cJSON_Delete(body); free(combined); free(info);
    if (err != ESP_OK || !result) return http_server_webim_error(req, err != ESP_OK ? err : ESP_ERR_NO_MEM);
    return http_server_send_json_response(req, result);
}

esp_err_t http_server_register_webim_session_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = SESSION_URI, .method = HTTP_GET, .handler = sessions_handler},
        {.uri = SESSION_URI, .method = HTTP_POST, .handler = sessions_handler},
        {.uri = SESSION_URI "/*", .method = HTTP_GET, .handler = session_handler},
        {.uri = SESSION_URI "/*", .method = HTTP_PATCH, .handler = session_handler},
        {.uri = SESSION_URI "/*", .method = HTTP_DELETE, .handler = session_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
