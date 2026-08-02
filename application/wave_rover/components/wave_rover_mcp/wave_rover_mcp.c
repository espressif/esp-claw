/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_mcp.h"
#include "wave_rover_mcp_web.h"
#include "wave_rover_mcp_metrics.h"
#include "wave_rover_hal.h"
#include "wave_rover_config.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "wr_mcp";

#define MCP_ENDPOINT   "mcp"
#define MCP_MAX_BODY   (8 * 1024)

/* Forward declarations from wave_rover_mcp_tools.c */
esp_err_t wr_mcp_register_all_tools(esp_mcp_t *mcp);
void      wr_mcp_tools_set_config(const wave_rover_config_t *cfg);
void      wr_mcp_tools_set_config_mut(wave_rover_config_t *cfg);
void      wr_mcp_tools_set_power_mgr(wr_power_mgr_handle_t pm);

static esp_mcp_t                    *s_mcp    = NULL;
static esp_mcp_mgr_handle_t          s_mgr    = 0;
static httpd_handle_t                s_httpd  = NULL;
static bool                          s_running = false;
static TimerHandle_t                 s_keepalive_timer = NULL;
static uint32_t                      s_last_cmd_ms = 0;
static const wave_rover_config_t    *s_cfg    = NULL;
static wr_power_mgr_handle_t         s_power_mgr = NULL;

/* ------------------------------------------------------------------ */
/* Null transport (no-op function table so the SDK engine works        */
/* without starting its own HTTP server)                               */
/* ------------------------------------------------------------------ */

static esp_err_t null_transport_init(esp_mcp_mgr_handle_t h, esp_mcp_transport_handle_t *t)
{
    (void)h;
    *t = (esp_mcp_transport_handle_t)1;
    return ESP_OK;
}

static esp_err_t null_transport_noop_h(esp_mcp_transport_handle_t t)
{
    (void)t;
    return ESP_OK;
}

static esp_err_t null_transport_start(esp_mcp_transport_handle_t t, void *c)
{
    (void)t; (void)c;
    return ESP_OK;
}

static uint8_t s_null_cfg_sentinel = 0;  /* non-NULL sentinel required by SDK */

static esp_err_t null_transport_create_cfg(const void *c, void **o)
{
    (void)c;
    *o = &s_null_cfg_sentinel;  /* SDK asserts non-NULL after create_config */
    return ESP_OK;
}

static esp_err_t null_transport_delete_cfg(void *c)
{
    (void)c;  /* sentinel is not heap-allocated, nothing to free */
    return ESP_OK;
}

static esp_err_t null_transport_reg_ep(esp_mcp_transport_handle_t t,
                                        const char *n, void *d)
{
    (void)t; (void)n; (void)d;
    return ESP_OK;
}

static esp_err_t null_transport_unreg_ep(esp_mcp_transport_handle_t t,
                                          const char *n)
{
    (void)t; (void)n;
    return ESP_OK;
}

static const esp_mcp_transport_t s_null_transport = {
    .init                = null_transport_init,
    .deinit              = null_transport_noop_h,
    .start               = null_transport_start,
    .stop                = null_transport_noop_h,
    .create_config       = null_transport_create_cfg,
    .delete_config       = null_transport_delete_cfg,
    .register_endpoint   = null_transport_reg_ep,
    .unregister_endpoint = null_transport_unreg_ep,
    .request             = NULL,
};

/* ------------------------------------------------------------------ */
/* Resources                                                           */
/* ------------------------------------------------------------------ */

static const char * const s_resource_uris[] = {
    "rover://status", "rover://config", "rover://wifi",
    "rover://power",  "rover://ups",    "rover://imu",
    "rover://display","rover://logs/recent",
};
#define NUM_RESOURCES (sizeof(s_resource_uris) / sizeof(s_resource_uris[0]))

static cJSON *build_resources_list(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr    = cJSON_AddArrayToObject(result, "resources");
    for (size_t i = 0; i < NUM_RESOURCES; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "uri",      s_resource_uris[i]);
        cJSON_AddStringToObject(r, "mimeType", "application/json");
        cJSON_AddItemToArray(arr, r);
    }
    return result;
}

static cJSON *read_resource(const char *uri)
{
    cJSON *out = cJSON_CreateObject();
    if (!uri) { cJSON_AddStringToObject(out, "error", "no uri"); return out; }

    if (strcmp(uri, "rover://status") == 0) {
        wr_motor_state_t ms = {0};
        wr_motor_get_state(&ms);
        cJSON_AddBoolToObject(out,   "ok",            true);
        cJSON_AddNumberToObject(out, "left_motor",    ms.left);
        cJSON_AddNumberToObject(out, "right_motor",   ms.right);
        cJSON_AddBoolToObject(out,   "emergency_stop",ms.emergency_stop);
    } else if (strcmp(uri, "rover://power") == 0 ||
               strcmp(uri, "rover://ups")   == 0) {
        wr_power_status_t ps = {0};
        wr_power_get_status(&ps);
        cJSON_AddBoolToObject(out,   "ok",         true);
        cJSON_AddBoolToObject(out,   "present",     ps.present);
        cJSON_AddNumberToObject(out, "voltage_v",   ps.load_voltage_v);
        cJSON_AddNumberToObject(out, "current_ma",  ps.current_ma);
        cJSON_AddBoolToObject(out,   "low_battery", ps.low_battery);
    } else if (strcmp(uri, "rover://imu") == 0) {
        wr_imu_sample_t is = {0};
        wr_imu_get_sample(&is);
        cJSON_AddBoolToObject(out, "ok",      true);
        cJSON_AddBoolToObject(out, "present", is.present);
    } else if (strcmp(uri, "rover://config") == 0) {
        cJSON_AddBoolToObject(out, "ok", true);
        if (s_cfg) {
            cJSON_AddStringToObject(out, "hostname",     s_cfg->hostname);
            cJSON_AddNumberToObject(out, "mcp_port",     s_cfg->mcp_port);
            cJSON_AddBoolToObject(out,   "auth_enabled", s_cfg->auth_enabled);
            cJSON_AddNumberToObject(out, "max_speed",    (double)s_cfg->max_speed);
            cJSON_AddNumberToObject(out, "max_cmd_ms",   s_cfg->max_command_duration_ms);
        }
    } else if (strcmp(uri, "rover://wifi") == 0) {
        cJSON_AddBoolToObject(out, "ok", true);
        if (s_cfg) {
            cJSON_AddNumberToObject(out, "mode",        s_cfg->wifi_mode);
            cJSON_AddStringToObject(out, "ap_ssid",     s_cfg->wifi_ap_ssid);
            cJSON_AddStringToObject(out, "sta_ssid",    s_cfg->wifi_ssid);
            cJSON_AddStringToObject(out, "hostname",    s_cfg->hostname);
            /* Passwords are never returned */
        }
    } else if (strcmp(uri, "rover://display") == 0) {
        cJSON_AddBoolToObject(out,   "ok",   true);
        cJSON_AddStringToObject(out, "type", "SSD1306 128x32");
    } else if (strcmp(uri, "rover://logs/recent") == 0) {
        cJSON_AddStringToObject(out, "log", "(ring buffer not implemented)");
    } else {
        cJSON_AddBoolToObject(out,   "ok",    false);
        cJSON_AddStringToObject(out, "error", "unknown resource uri");
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* JSON-RPC helpers                                                    */
/* ------------------------------------------------------------------ */

static char *jsonrpc_resources_list_resp(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemReferenceToObject(resp, "id", id);
    cJSON_AddItemToObject(resp, "result", build_resources_list());
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

static char *jsonrpc_resources_read_resp(cJSON *id, const char *uri)
{
    cJSON *resp        = cJSON_CreateObject();
    cJSON *content_arr = cJSON_CreateArray();
    cJSON *item        = cJSON_CreateObject();
    cJSON *result      = cJSON_CreateObject();

    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemReferenceToObject(resp, "id", id);

    cJSON_AddStringToObject(item, "uri",      uri ? uri : "");
    cJSON_AddStringToObject(item, "mimeType", "application/json");

    cJSON *data = read_resource(uri);
    char *data_str = cJSON_PrintUnformatted(data);
    cJSON_AddStringToObject(item, "text", data_str ? data_str : "{}");
    free(data_str);
    cJSON_Delete(data);

    cJSON_AddItemToArray(content_arr, item);
    cJSON_AddItemToObject(result, "contents", content_arr);
    cJSON_AddItemToObject(resp,   "result",   result);

    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

static char *jsonrpc_error_resp(cJSON *id, int code, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemReferenceToObject(resp, "id", id);
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code",    code);
    cJSON_AddStringToObject(err, "message", msg ? msg : "error");
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

/* ------------------------------------------------------------------ */
/* Auth check                                                          */
/* ------------------------------------------------------------------ */

/* Constant-time byte comparison — avoids timing side-channel on token */
static bool ct_streq(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    volatile uint8_t diff = (uint8_t)(la ^ lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

#define AUTH_HDR_BUF 96

static bool check_auth(httpd_req_t *req)
{
    if (!s_cfg || !s_cfg->auth_enabled) return true;   /* auth disabled */
    if (s_cfg->auth_token[0] == '\0')   return true;   /* no token set */

    char buf[AUTH_HDR_BUF] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    /* Expect "Bearer <token>" */
    if (strncmp(buf, "Bearer ", 7) != 0) return false;
    return ct_streq(buf + 7, s_cfg->auth_token);
}

/* ------------------------------------------------------------------ */
/* HTTP POST handler for /mcp                                          */
/* ------------------------------------------------------------------ */

static esp_err_t mcp_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
            "{\"code\":-32001,\"message\":\"Unauthorized\"}}");
        return ESP_OK;
    }

    int total = req->content_len;
    if (total <= 0 || total > MCP_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_OK;
    }

    char *body = calloc(1, total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
            return ESP_OK;
        }
        received += r;
    }

    s_last_cmd_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    cJSON *root     = cJSON_Parse(body);
    cJSON *method_j = root ? cJSON_GetObjectItem(root, "method") : NULL;
    cJSON *id_j     = root ? cJSON_GetObjectItem(root, "id")     : NULL;
    const char *method = (method_j && cJSON_IsString(method_j))
                         ? method_j->valuestring : "";

    const char *resp_str = NULL;
    char *inline_resp = NULL;

    if (strcmp(method, "resources/list") == 0) {
        inline_resp = jsonrpc_resources_list_resp(id_j);
        resp_str    = inline_resp;
    } else if (strcmp(method, "resources/read") == 0) {
        cJSON *params = cJSON_GetObjectItem(root, "params");
        cJSON *uri_j  = params ? cJSON_GetObjectItem(params, "uri") : NULL;
        const char *uri = (uri_j && cJSON_IsString(uri_j)) ? uri_j->valuestring : NULL;
        if (!uri) {
            inline_resp = jsonrpc_error_resp(id_j, -32602, "missing uri");
        } else {
            inline_resp = jsonrpc_resources_read_resp(id_j, uri);
        }
        resp_str = inline_resp;
    } else {
        /* Delegate to MCP SDK engine */
        uint8_t *out_buf = NULL;
        uint16_t out_len = 0;
        esp_err_t e = esp_mcp_mgr_req_handle(s_mgr, MCP_ENDPOINT,
                                              (const uint8_t *)body, (uint16_t)total,
                                              &out_buf, &out_len);
        if (e == ESP_OK && out_buf && out_len > 0) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, (const char *)out_buf, (int)out_len);
            esp_mcp_mgr_req_destroy_response(s_mgr, out_buf);
        } else {
            char *err_resp = jsonrpc_error_resp(id_j, -32603, "internal error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, err_resp, err_resp ? (int)strlen(err_resp) : 0);
            free(err_resp);
        }
        if (root) cJSON_Delete(root);
        free(body);
        return ESP_OK;
    }

    if (root) cJSON_Delete(root);
    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, resp_str ? (int)strlen(resp_str) : 0);
    free(inline_resp);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Keepalive watchdog — stop motors if no commands received for 10s   */
/* ------------------------------------------------------------------ */

static void keepalive_cb(TimerHandle_t t)
{
    (void)t;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now - s_last_cmd_ms > 10000) {
        wr_motor_stop();
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t wave_rover_mcp_start(const wave_rover_config_t *cfg,
                                wr_power_mgr_handle_t power_mgr)
{
    if (s_running) return ESP_OK;

    s_cfg = cfg;
    s_power_mgr = power_mgr;
    ESP_RETURN_ON_ERROR(esp_mcp_create(&s_mcp), TAG, "esp_mcp_create");
    wr_mcp_tools_set_config(cfg);
    wr_mcp_tools_set_config_mut((wave_rover_config_t *)cfg);
    wr_mcp_tools_set_power_mgr(s_power_mgr);
    ESP_RETURN_ON_ERROR(wr_mcp_register_all_tools(s_mcp), TAG, "register tools");

    esp_mcp_mgr_config_t mcfg = {
        .transport = s_null_transport,
        .config    = NULL,
        .instance  = s_mcp,
    };
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_init(mcfg, &s_mgr), TAG, "mgr init");
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_register_endpoint(s_mgr, MCP_ENDPOINT, NULL),
                        TAG, "register endpoint");

    httpd_config_t hcfg    = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port       = cfg->mcp_port;
    hcfg.max_uri_handlers  = 16;
    hcfg.stack_size        = 8192;  /* nav tools run in httpd task, need headroom */
    hcfg.recv_wait_timeout = 30;   /* OTA uploads over slow WiFi need longer recv timeout */
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hcfg), TAG, "httpd_start");

    httpd_uri_t mcp_uri = {
        .uri      = "/mcp",
        .method   = HTTP_POST,
        .handler  = mcp_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &mcp_uri),
                        TAG, "register uri");

    wr_mcp_web_set_power_mgr(s_power_mgr);
    ESP_RETURN_ON_ERROR(wr_mcp_web_register(s_httpd, cfg), TAG, "web ui");
    ESP_RETURN_ON_ERROR(wr_mcp_metrics_register(s_httpd, cfg), TAG, "metrics");
    wr_mcp_metrics_set_power_mgr(s_power_mgr);

    s_last_cmd_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_keepalive_timer = xTimerCreate("mcp_ka", pdMS_TO_TICKS(5000),
                                     pdTRUE, NULL, keepalive_cb);
    if (s_keepalive_timer) xTimerStart(s_keepalive_timer, 0);

    s_running = true;
    ESP_LOGI(TAG, "MCP server started on port %u at /mcp", cfg->mcp_port);
    return ESP_OK;
}

esp_err_t wave_rover_mcp_stop(void)
{
    if (!s_running) return ESP_OK;
    if (s_keepalive_timer) {
        xTimerStop(s_keepalive_timer, 0);
        xTimerDelete(s_keepalive_timer, 0);
        s_keepalive_timer = NULL;
    }
    wr_mcp_web_stop();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_mgr)   { esp_mcp_mgr_deinit(s_mgr); s_mgr = 0; }
    if (s_mcp)   { esp_mcp_destroy(s_mcp); s_mcp = NULL; }
    s_cfg       = NULL;
    s_power_mgr = NULL;
    s_running   = false;
    return ESP_OK;
}

bool wave_rover_mcp_is_running(void) { return s_running; }
