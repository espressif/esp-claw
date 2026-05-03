/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

/*
 * Field catalogue
 *
 * Every NVS-backed config field is described here along with the group it
 * belongs to. The WebUI addresses fields by `name`; the C side stores them
 * in `app_config_t` at the given offset. Keeping the table in one place
 * lets both the GET (partial read) and POST (partial write) handlers share
 * the same serialisation logic.
 */

#define CONFIG_FIELD(group, field) { \
    #field, (group), \
    offsetof(app_config_t, field), \
    sizeof(((app_config_t *)0)->field) \
}

typedef struct {
    const char *name;
    const char *group;
    size_t offset;
    size_t size;
} config_field_def_t;

static const config_field_def_t CONFIG_FIELDS[] = {
    CONFIG_FIELD("wifi",         wifi_ssid),
    CONFIG_FIELD("wifi",         wifi_password),

    CONFIG_FIELD("llm",          llm_api_key),
    CONFIG_FIELD("llm",          llm_backend_type),
    CONFIG_FIELD("llm",          llm_profile),
    CONFIG_FIELD("llm",          llm_model),
    CONFIG_FIELD("llm",          llm_base_url),
    CONFIG_FIELD("llm",          llm_auth_type),
    CONFIG_FIELD("llm",          llm_timeout_ms),
    CONFIG_FIELD("llm",          llm_max_tokens),

    CONFIG_FIELD("im",           qq_app_id),
    CONFIG_FIELD("im",           qq_app_secret),
    CONFIG_FIELD("im",           feishu_app_id),
    CONFIG_FIELD("im",           feishu_app_secret),
    CONFIG_FIELD("im",           tg_bot_token),
    CONFIG_FIELD("im",           wechat_token),
    CONFIG_FIELD("im",           wechat_base_url),
    CONFIG_FIELD("im",           wechat_cdn_base_url),
    CONFIG_FIELD("im",           wechat_account_id),

    CONFIG_FIELD("search",       search_brave_key),
    CONFIG_FIELD("search",       search_tavily_key),

    CONFIG_FIELD("capabilities", enabled_cap_groups),
    CONFIG_FIELD("capabilities", llm_visible_cap_groups),

    CONFIG_FIELD("skills",       enabled_lua_modules),

    CONFIG_FIELD("time",         time_timezone),
};

static const size_t CONFIG_FIELD_COUNT = sizeof(CONFIG_FIELDS) / sizeof(CONFIG_FIELDS[0]);

static const char *TAG = "http_config_api";

/* ── Helpers ────────────────────────────────────────────────────────── */

static bool csv_contains(const char *csv, const char *token)
{
    if (!csv || !token) {
        return false;
    }
    size_t token_len = strlen(token);
    const char *cursor = csv;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == ',') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        const char *comma = strchr(cursor, ',');
        size_t seg_len = comma ? (size_t)(comma - cursor) : strlen(cursor);
        while (seg_len > 0 && cursor[seg_len - 1] == ' ') {
            seg_len--;
        }
        if (seg_len == token_len && strncmp(cursor, token, token_len) == 0) {
            return true;
        }
        cursor += seg_len;
        if (comma) {
            cursor = comma + 1;
        }
    }
    return false;
}

static bool field_matches_filter(const config_field_def_t *field,
                                 const char *groups_csv,
                                 const char *fields_csv)
{
    if (!groups_csv && !fields_csv) {
        return true;
    }
    if (groups_csv && csv_contains(groups_csv, field->group)) {
        return true;
    }
    if (fields_csv && csv_contains(fields_csv, field->name)) {
        return true;
    }
    return false;
}

static const char *field_value(const app_config_t *config, const config_field_def_t *field)
{
    return ((const char *)config) + field->offset;
}

static char *field_mutable(app_config_t *config, const config_field_def_t *field)
{
    return ((char *)config) + field->offset;
}

static bool is_positive_decimal_string(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    if (!cursor || !cursor[0]) {
        return false;
    }
    if (cursor[0] == '0') {
        return false;
    }

    while (*cursor) {
        if (!isdigit(*cursor)) {
            return false;
        }
        cursor++;
    }

    return true;
}

static esp_err_t emit_config(httpd_req_t *req,
                             const app_config_t *config,
                             const char *groups_csv,
                             const char *fields_csv,
                             cJSON *extra_meta)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++) {
        const config_field_def_t *field = &CONFIG_FIELDS[i];
        if (!field_matches_filter(field, groups_csv, fields_csv)) {
            continue;
        }
        http_server_json_add_string(root, field->name, field_value(config, field));
    }

    if (extra_meta) {
        cJSON_AddItemToObject(root, "_meta", extra_meta);
    }

    return http_server_send_json_response(req, root);
}

/* ── Handlers ───────────────────────────────────────────────────────── */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    app_config_t *config = NULL;
    esp_err_t err;

    config = calloc(1, sizeof(*config));
    if (!config) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    err = ctx->services.load_config(config);
    if (err != ESP_OK) {
        free(config);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load config");
    }

    char filter_buf[192];
    char *groups_csv = NULL;
    char *fields_csv = NULL;
    bool include_meta = false;

    if (http_server_query_get(req, "groups", filter_buf, sizeof(filter_buf)) == ESP_OK) {
        groups_csv = strdup(filter_buf);
    }
    if (http_server_query_get(req, "fields", filter_buf, sizeof(filter_buf)) == ESP_OK) {
        fields_csv = strdup(filter_buf);
    }
    if (http_server_query_get(req, "meta", filter_buf, sizeof(filter_buf)) == ESP_OK) {
        include_meta = (filter_buf[0] == '1' || strcmp(filter_buf, "true") == 0);
    }

    cJSON *meta = NULL;
    if (include_meta) {
        meta = cJSON_CreateObject();
        cJSON *groups = meta ? cJSON_CreateArray() : NULL;
        cJSON *fields = meta ? cJSON_CreateArray() : NULL;
        if (meta && groups && fields) {
            for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++) {
                const config_field_def_t *field = &CONFIG_FIELDS[i];
                cJSON *entry = cJSON_CreateObject();
                if (entry) {
                    cJSON_AddStringToObject(entry, "name", field->name);
                    cJSON_AddStringToObject(entry, "group", field->group);
                    cJSON_AddItemToArray(fields, entry);
                }
            }

            for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++) {
                const char *name = CONFIG_FIELDS[i].group;
                bool seen = false;
                cJSON *existing = NULL;
                cJSON_ArrayForEach(existing, groups) {
                    if (cJSON_IsString(existing) && strcmp(existing->valuestring, name) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    cJSON_AddItemToArray(groups, cJSON_CreateString(name));
                }
            }

            cJSON_AddItemToObject(meta, "groups", groups);
            cJSON_AddItemToObject(meta, "fields", fields);
        } else {
            cJSON_Delete(groups);
            cJSON_Delete(fields);
            cJSON_Delete(meta);
            meta = NULL;
        }
    }

    esp_err_t send_err = emit_config(req, config, groups_csv, fields_csv, meta);
    free(config);
    free(groups_csv);
    free(fields_csv);
    return send_err;
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    app_config_t *config = NULL;
    esp_err_t err;

    config = calloc(1, sizeof(*config));
    if (!config) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    err = ctx->services.load_config(config);
    if (err != ESP_OK) {
        free(config);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load config");
    }

    cJSON *root = NULL;
    err = http_server_parse_json_body(req, &root);
    if (err != ESP_OK) {
        free(config);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    /* Partial writes: only fields present in the JSON body are applied.
     * Empty string is a valid value (lets the client clear a slot). */
    size_t applied_count = 0;

    for (size_t i = 0; i < CONFIG_FIELD_COUNT; i++) {
        const config_field_def_t *field = &CONFIG_FIELDS[i];
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field->name);
        if (!cJSON_IsString(item)) {
            continue;
        }
        if (strcmp(field->name, "llm_max_tokens") == 0 &&
                !is_positive_decimal_string(item->valuestring)) {
            cJSON_Delete(root);
            free(config);
            return httpd_resp_send_err(req,
                                       HTTPD_400_BAD_REQUEST,
                                       "llm_max_tokens must be a positive integer");
        }
        strlcpy(field_mutable(config, field), item->valuestring, field->size);
        applied_count++;
    }

    cJSON_Delete(root);

    if (applied_count == 0) {
        free(config);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Request did not contain any recognised fields");
    }

    err = ctx->services.save_config(config);
    free(config);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
    }

    ESP_LOGI(TAG, "Saved %u config field%s", (unsigned)applied_count, applied_count == 1 ? "" : "s");

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "applied", (double)applied_count);
    http_server_json_add_string(resp, "message",
                                "Saved. Restart the device to apply Wi-Fi, core LLM, capability, and Lua module changes.");
    return http_server_send_json_response(req, resp);
}

esp_err_t http_server_register_config_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/config", .method = HTTP_GET,  .handler = config_get_handler  },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
