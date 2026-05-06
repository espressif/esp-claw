/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_http.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#include "cJSON.h"
#include "cap_lua.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lauxlib.h"
#include "lua.h"

static const char *TAG = "lua_module_http";

/* ── Response buffer ──────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_resp_buf_t;

static esp_err_t http_resp_buf_init(http_resp_buf_t *buf)
{
    buf->data = malloc(256);
    if (!buf->data) return ESP_ERR_NO_MEM;
    buf->len = 0;
    buf->cap = 256;
    buf->data[0] = '\0';
    return ESP_OK;
}

static esp_err_t http_resp_buf_append(http_resp_buf_t *buf, const char *data, size_t len)
{
    size_t needed = buf->len + len + 1;
    if (needed > buf->cap) {
        size_t new_cap = buf->cap * 2;
        while (new_cap < needed) new_cap *= 2;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return ESP_ERR_NO_MEM;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static void http_resp_buf_free(http_resp_buf_t *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/* ── HTTP event handler ───────────────────────────────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_buf_t *buf = (http_resp_buf_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data && evt->data_len > 0) {
            http_resp_buf_append(buf, (const char *)evt->data, evt->data_len);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ── Helper: read table string field ──────────────────── */

static const char *lua_table_string(lua_State *L, int tbl_idx, const char *key)
{
    lua_getfield(L, tbl_idx, key);
    const char *val = lua_tostring(L, -1);
    lua_pop(L, 1);
    return val;
}

static int lua_table_integer(lua_State *L, int tbl_idx, const char *key, int default_val)
{
    lua_getfield(L, tbl_idx, key);
    int val;
    if (lua_isinteger(L, -1)) {
        val = (int)lua_tointeger(L, -1);
    } else {
        val = default_val;
    }
    lua_pop(L, 1);
    return val;
}

/* ── Lua: http.request(opts) ──────────────────────────── */

static int lua_module_http_request(lua_State *L)
{
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "expected a table argument with url, method, headers, body");
        return 2;
    }

    const char *url = lua_table_string(L, 1, "url");
    const char *method_str = lua_table_string(L, 1, "method");
    const char *body = lua_table_string(L, 1, "body");
    int timeout_ms = lua_table_integer(L, 1, "timeout_ms", 10000);

    if (!url || !url[0]) {
        lua_pushnil(L);
        lua_pushstring(L, "url is required");
        return 2;
    }

    esp_http_client_method_t method = HTTP_METHOD_GET;
    if (method_str) {
        if (strcasecmp(method_str, "POST") == 0) method = HTTP_METHOD_POST;
        else if (strcasecmp(method_str, "PUT") == 0) method = HTTP_METHOD_PUT;
        else if (strcasecmp(method_str, "DELETE") == 0) method = HTTP_METHOD_DELETE;
        else if (strcasecmp(method_str, "PATCH") == 0) method = HTTP_METHOD_PATCH;
        else if (strcasecmp(method_str, "HEAD") == 0) method = HTTP_METHOD_HEAD;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };

    http_resp_buf_t resp_buf;
    if (http_resp_buf_init(&resp_buf) != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        http_resp_buf_free(&resp_buf);
        lua_pushnil(L);
        lua_pushstring(L, "failed to init HTTP client");
        return 2;
    }

    esp_http_client_set_user_data(client, &resp_buf);

    /* ── Set custom headers ── */
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            const char *key = lua_tostring(L, -2);
            const char *val = lua_tostring(L, -1);
            if (key && val) {
                esp_http_client_set_header(client, key, val);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    /* ── Set body for POST/PUT/PATCH ── */
    if (body && (method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT || method == HTTP_METHOD_PATCH)) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    /* ── Execute ── */
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        http_resp_buf_free(&resp_buf);
        lua_pushnil(L);
        lua_pushfstring(L, "HTTP request failed: %s (status=%d)", esp_err_to_name(err), status_code);
        return 2;
    }

    /* ── Build result table ── */
    lua_createtable(L, 0, 3);

    lua_pushinteger(L, status_code);
    lua_setfield(L, -2, "status");

    lua_pushstring(L, resp_buf.data ? resp_buf.data : "");
    lua_setfield(L, -2, "body");

    lua_pushinteger(L, (int)resp_buf.len);
    lua_setfield(L, -2, "body_len");

    http_resp_buf_free(&resp_buf);
    return 1; /* return the result table */
}

/* ── Lua: http.download(opts) ─────────────────────────── */

static esp_err_t download_write_to_file(void *user_data, const char *data, int data_len)
{
    FILE *f = (FILE *)user_data;
    if (f && data && data_len > 0) {
        if (fwrite(data, 1, data_len, f) != (size_t)data_len) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t download_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data && evt->data_len > 0) {
            return download_write_to_file(evt->user_data, (const char *)evt->data, evt->data_len);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static int lua_module_http_download(lua_State *L)
{
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "expected a table with url and path");
        return 2;
    }

    const char *url = lua_table_string(L, 1, "url");
    const char *path = lua_table_string(L, 1, "path");
    int timeout_ms = lua_table_integer(L, 1, "timeout_ms", 30000);

    if (!url || !url[0] || !path || !path[0]) {
        lua_pushnil(L);
        lua_pushstring(L, "url and path are required");
        return 2;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "failed to open file for writing: %s", path);
        return 2;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .event_handler = download_event_handler,
        .user_data = f,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(f);
        lua_pushnil(L);
        lua_pushstring(L, "failed to init HTTP client");
        return 2;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(f);

    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushfstring(L, "download failed: %s (status=%d)", esp_err_to_name(err), status_code);
        return 2;
    }

    lua_createtable(L, 0, 2);
    lua_pushinteger(L, status_code);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    return 1;
}

/* ══════════════════════════════════════════════════════════
   HTTP Server module (embedded)
   ══════════════════════════════════════════════════════════ */

#define LUA_HTTP_SRV_MAX_ROUTES  32
#define LUA_HTTP_SRV_URI_BUF     256

typedef struct {
    char method[8];
    char uri_pattern[128];
    int lua_ref;
} lua_http_srv_route_t;

typedef struct {
    httpd_handle_t server;
    bool started;
    lua_http_srv_route_t routes[LUA_HTTP_SRV_MAX_ROUTES];
    size_t route_count;
    int port;
    SemaphoreHandle_t lua_lock;
} lua_http_srv_ctx_t;

static lua_http_srv_ctx_t s_srv_ctx = {0};
static lua_State *s_srv_L = NULL;
static const char *SRV_TAG = "lua_http_srv";

/* ── Directory listing HTML (C implementation, no Lua needed) ── */

static esp_err_t lua_http_srv_list_dir(httpd_req_t *req, const char *path)
{
    char resp[4096];
    int off = 0;
    off += snprintf(resp + off, sizeof(resp) - off,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>ESP-Claw SD Card</title>"
        "<style>*{margin:0;padding:0;box-sizing:border-box;}"
        "body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;padding:20px;}"
        "h1{color:#e94560;margin-bottom:20px;}"
        "table{width:100%%;border-collapse:collapse;}"
        "th{text-align:left;padding:10px 8px;background:#16213e;color:#e94560;}"
        "td{padding:8px;border-bottom:1px solid #333;}"
        "tr:hover td{background:#0f3460;}"
        "a{color:#53d8fb;text-decoration:none;}a:hover{text-decoration:underline;}"
        ".dir{color:#ffd460;font-weight:bold;}"
        ".size{color:#888;text-align:right;}"
        "</style></head><body>"
        "<h1>📂 %s</h1><table><tr><th>Name</th><th>Size</th></tr>", path);

    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            if (name[0] == '.' && (!name[1] || name[1] == '.')) continue;
            if ((unsigned char)name[0] < 32 || (unsigned char)name[0] > 126) continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, name);
            struct stat st;
            bool is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
            if (is_dir)
                off += snprintf(resp + off, sizeof(resp) - off,
                    "<tr><td class='dir'>📁 <a href='/%s'>%s</a></td><td class='size'>dir</td></tr>", full, name);
            else
                off += snprintf(resp + off, sizeof(resp) - off,
                    "<tr><td>📄 <a href='/%s'>%s</a></td><td class='size'>%ld B</td></tr>", full, name, (long)st.st_size);
        }
        closedir(dir);
    }
    off += snprintf(resp + off, sizeof(resp) - off, "</table></body></html>");
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");
    httpd_resp_send(req, resp, off);
    return ESP_OK;
}

/* ── HTTP file server handler (pure C, no Lua) ────────── */

static esp_err_t lua_http_srv_handler(httpd_req_t *req)
{
    /* Static file server - handles / and /sdcard wildcard in C */
    const char *uri = req->uri;

    /* Redirect / to /sdcard/ */
    if (strcmp(uri, "/") == 0 || strcmp(uri, "") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/sdcard/");
        httpd_resp_sendstr(req, "Redirecting to /sdcard/");
        return ESP_OK;
    }

    /* Build physical path from URI 
       /sdcard/... → /sdcard/... (absolute on device) */
    char full_path[512];
    full_path[0] = '\0';

    if (strncmp(uri, "/sdcard", 7) == 0) {
        strlcpy(full_path, uri + 1, sizeof(full_path)); /* remove leading / */
        /* Ensure it starts with the mount path */
    } else {
        strlcpy(full_path, uri + 1, sizeof(full_path));
    }

    /* If path is empty or just "sdcard", list the directory */
    struct stat st;
    if (full_path[0] == '\0' || (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode))) {
        return lua_http_srv_list_dir(req, full_path[0] ? full_path : "sdcard");
    }

    /* Serve file using the same full_path */
    const char *content_type = "application/octet-stream";
    const char *ext = strrchr(full_path, '.');
    if (ext) {
        if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) content_type = "text/html; charset=utf-8";
        else if (strcasecmp(ext, ".css") == 0) content_type = "text/css";
        else if (strcasecmp(ext, ".js") == 0) content_type = "application/javascript";
        else if (strcasecmp(ext, ".png") == 0) content_type = "image/png";
        else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) content_type = "image/jpeg";
        else if (strcasecmp(ext, ".json") == 0) content_type = "application/json";
        else if (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0) content_type = "text/plain; charset=utf-8";
        else if (strcasecmp(ext, ".wav") == 0) content_type = "audio/wav";
        else if (strcasecmp(ext, ".mp3") == 0) content_type = "audio/mpeg";
        else if (strcasecmp(ext, ".lua") == 0) content_type = "text/plain; charset=utf-8";
    }
    httpd_resp_set_hdr(req, "Content-Type", content_type);

    /* Read and serve file */
    FILE *f = fopen(full_path, "rb");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *fbuf = malloc(fsize + 1);
    if (!fbuf) { fclose(f); httpd_resp_send_500(req); return ESP_FAIL; }
    size_t nread = fread(fbuf, 1, fsize, f);
    fclose(f);
    fbuf[nread] = '\0';
    httpd_resp_send(req, fbuf, nread);
    free(fbuf);
    return ESP_OK;
}

static int lua_http_srv_start(lua_State *L)
{
    if (s_srv_ctx.started) { lua_pushboolean(L, 0); lua_pushstring(L, "already running"); return 2; }
    int port = 8080, maxc = 4;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "port"); if (lua_isinteger(L, -1)) port = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "max_clients"); if (lua_isinteger(L, -1)) maxc = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    }

    /* Create Lua mutex if not already created */
    if (!s_srv_ctx.lua_lock) {
        s_srv_ctx.lua_lock = xSemaphoreCreateMutex();
        if (!s_srv_ctx.lua_lock) {
            lua_pushboolean(L, 0); lua_pushstring(L, "failed to create lock"); return 2;
        }
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port; cfg.max_open_sockets = maxc;
    cfg.lru_purge_enable = true; cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_srv_ctx.server, &cfg) != ESP_OK) {
        lua_pushboolean(L, 0); lua_pushstring(L, "failed to start"); return 2;
    }
    s_srv_ctx.started = true; s_srv_ctx.port = port; s_srv_ctx.route_count = 0;
    ESP_LOGI(SRV_TAG, "Server started on port %d", port);
    lua_pushboolean(L, 1); lua_pushinteger(L, port);
    return 2;
}

static int lua_http_srv_route(lua_State *L)
{
    if (!s_srv_ctx.started) { lua_pushboolean(L, 0); lua_pushstring(L, "not started"); return 2; }
    if (s_srv_ctx.route_count >= LUA_HTTP_SRV_MAX_ROUTES) { lua_pushboolean(L, 0); lua_pushstring(L, "max routes"); return 2; }
    const char *method = luaL_checkstring(L, 1);
    const char *uri = luaL_checkstring(L, 2);
    if (!lua_isfunction(L, 3)) { lua_pushboolean(L, 0); lua_pushstring(L, "callback required"); return 2; }
    size_t idx = s_srv_ctx.route_count;
    strlcpy(s_srv_ctx.routes[idx].method, method, sizeof(s_srv_ctx.routes[idx].method));
    strlcpy(s_srv_ctx.routes[idx].uri_pattern, uri, sizeof(s_srv_ctx.routes[idx].uri_pattern));
    lua_pushvalue(L, 3);
    s_srv_ctx.routes[idx].lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_srv_ctx.route_count++;
    httpd_uri_t h = {
        .uri = uri,
        .method = strcmp(method, "GET") == 0 ? HTTP_GET : strcmp(method, "POST") == 0 ? HTTP_POST :
                  strcmp(method, "PUT") == 0 ? HTTP_PUT : strcmp(method, "DELETE") == 0 ? HTTP_DELETE : HTTP_GET,
        .handler = lua_http_srv_handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_srv_ctx.server, &h);
    if (err != ESP_OK) {
        ESP_LOGW(SRV_TAG, "route fail %s %s: %s", method, uri, esp_err_to_name(err));
        lua_pushboolean(L, 0); lua_pushstring(L, esp_err_to_name(err)); return 2;
    }
    ESP_LOGI(SRV_TAG, "Route: %s %s", method, uri);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_http_srv_stop(lua_State *L)
{
    if (!s_srv_ctx.started) { lua_pushboolean(L, 0); return 1; }
    for (size_t i = 0; i < s_srv_ctx.route_count; i++) luaL_unref(L, LUA_REGISTRYINDEX, s_srv_ctx.routes[i].lua_ref);
    s_srv_ctx.route_count = 0;
    httpd_stop(s_srv_ctx.server);
    s_srv_ctx.started = false;
    ESP_LOGI(SRV_TAG, "Server stopped");
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_http_srv_routes(lua_State *L)
{
    lua_createtable(L, s_srv_ctx.route_count, 0);
    for (size_t i = 0; i < s_srv_ctx.route_count; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, s_srv_ctx.routes[i].method); lua_setfield(L, -2, "method");
        lua_pushstring(L, s_srv_ctx.routes[i].uri_pattern); lua_setfield(L, -2, "path");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ── Lua open functions ──────────────────────────────── */

int luaopen_http(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_module_http_request);
    lua_setfield(L, -2, "request");
    lua_pushcfunction(L, lua_module_http_download);
    lua_setfield(L, -2, "download");
    return 1;
}

int luaopen_http_server(lua_State *L)
{
    s_srv_L = L;
    lua_newtable(L);
    lua_pushcfunction(L, lua_http_srv_start);
    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, lua_http_srv_route);
    lua_setfield(L, -2, "route");
    lua_pushcfunction(L, lua_http_srv_stop);
    lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, lua_http_srv_routes);
    lua_setfield(L, -2, "routes");
    return 1;
}

/* ── Registration ─────────────────────────────────────── */

esp_err_t lua_module_http_register(void)
{
    esp_err_t err;
    err = cap_lua_register_module("http", luaopen_http);
    if (err != ESP_OK) return err;
    err = cap_lua_register_module("http_server", luaopen_http_server);
    return err;
}
