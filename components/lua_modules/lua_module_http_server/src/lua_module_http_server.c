/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_http_server.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define LUA_HTTP_SERVER_NAME             "http_server"
#define LUA_HTTP_APP_MT                  "http_server.app"
#define LUA_HTTP_APP_ID_MAX              32
#define LUA_HTTP_PATH_MAX                192
#define LUA_HTTP_STATIC_ROOT_MAX         128
#define LUA_HTTP_MAX_ROUTES              16
#define LUA_HTTP_QUEUE_LEN               8
#define LUA_HTTP_MAX_BODY                8192
#define LUA_HTTP_WAIT_MS                 10000
#define LUA_HTTP_SCRATCH_SIZE            1024

static const char *TAG = "lua_http_server";

typedef enum {
    LUA_HTTP_METHOD_GET = 0,
    LUA_HTTP_METHOD_POST,
} lua_http_method_t;

typedef struct {
    lua_http_method_t method;
    char path[LUA_HTTP_PATH_MAX];
    int callback_ref;
} lua_http_route_t;

typedef struct lua_http_app {
    bool active;
    bool serving;
    lua_State *L;
    char app_id[LUA_HTTP_APP_ID_MAX];
    char static_root[LUA_HTTP_STATIC_ROOT_MAX];
    QueueHandle_t queue;
    lua_http_route_t routes[LUA_HTTP_MAX_ROUTES];
    size_t route_count;
    struct lua_http_app *next;
} lua_http_app_t;

typedef struct {
    lua_http_method_t method;
    char path[LUA_HTTP_PATH_MAX];
    char query[256];
    char content_type[96];
    char *body;
    size_t body_len;
    SemaphoreHandle_t done;
    int status;
    char content_type_out[64];
    char *response;
    size_t response_len;
    esp_err_t err;
} lua_http_request_t;

typedef struct {
    lua_http_app_t *app;
} lua_http_app_ud_t;

static SemaphoreHandle_t s_lock;
static lua_http_app_t *s_apps;

static void lua_http_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void lua_http_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static bool lua_http_app_id_is_valid(const char *app_id)
{
    if (!app_id || !app_id[0] || strlen(app_id) >= LUA_HTTP_APP_ID_MAX) {
        return false;
    }
    for (size_t i = 0; app_id[i]; i++) {
        unsigned char ch = (unsigned char)app_id[i];
        if (!isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

static bool lua_http_path_is_safe(const char *path, bool require_abs)
{
    return path && path[0] &&
           (!require_abs || path[0] == '/') &&
           strstr(path, "..") == NULL;
}

static bool lua_http_static_root_is_valid(const char *path)
{
    return path && path[0] == '/' && strstr(path, "..") == NULL &&
           strlen(path) < LUA_HTTP_STATIC_ROOT_MAX;
}

static void lua_http_url_decode_inplace(char *value)
{
    char *src = value;
    char *dst = value;

    if (!value) {
        return;
    }
    while (*src) {
        if (src[0] == '%' && isxdigit((unsigned char)src[1]) &&
                isxdigit((unsigned char)src[2])) {
            char bytes[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtol(bytes, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static const char *lua_http_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');

    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

static lua_http_app_t *lua_http_find_app_locked(const char *app_id)
{
    for (lua_http_app_t *app = s_apps; app; app = app->next) {
        if (app->active && strcmp(app->app_id, app_id) == 0) {
            return app;
        }
    }
    return NULL;
}

static void lua_http_remove_app_locked(lua_http_app_t *app)
{
    lua_http_app_t **cursor = &s_apps;

    while (*cursor) {
        if (*cursor == app) {
            *cursor = app->next;
            app->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static void lua_http_complete_request(lua_http_request_t *req,
                                      esp_err_t err,
                                      int status,
                                      const char *content_type,
                                      const char *body)
{
    if (!req) {
        return;
    }

    req->err = err;
    req->status = status;
    strlcpy(req->content_type_out,
            content_type && content_type[0] ? content_type : "text/plain; charset=utf-8",
            sizeof(req->content_type_out));
    if (body) {
        req->response = strdup(body);
        req->response_len = req->response ? strlen(req->response) : 0;
        if (!req->response) {
            req->err = ESP_ERR_NO_MEM;
            req->status = 500;
        }
    }
    if (req->done) {
        xSemaphoreGive(req->done);
    }
}

static void lua_http_fail_pending(lua_http_app_t *app, const char *message)
{
    lua_http_request_t *req = NULL;

    if (!app || !app->queue) {
        return;
    }
    while (xQueueReceive(app->queue, &req, 0) == pdTRUE) {
        lua_http_complete_request(req,
                                  ESP_ERR_INVALID_STATE,
                                  503,
                                  "text/plain; charset=utf-8",
                                  message ? message : "Lua HTTP app stopped");
    }
}

static void lua_http_unregister_app(lua_http_app_t *app)
{
    if (!app) {
        return;
    }

    lua_http_lock();
    if (app->active) {
        app->active = false;
        lua_http_remove_app_locked(app);
    }
    lua_http_unlock();

    lua_http_fail_pending(app, "Lua HTTP app stopped");
}

static lua_http_app_t *lua_http_check_app(lua_State *L, int index)
{
    lua_http_app_ud_t *ud = (lua_http_app_ud_t *)luaL_checkudata(L, index, LUA_HTTP_APP_MT);

    if (!ud || !ud->app || !ud->app->active) {
        luaL_error(L, "http_server app is closed");
    }
    return ud->app;
}

static lua_http_route_t *lua_http_find_route(lua_http_app_t *app,
                                             lua_http_method_t method,
                                             const char *path)
{
    if (!app || !path) {
        return NULL;
    }
    for (size_t i = 0; i < app->route_count; i++) {
        if (app->routes[i].method == method && strcmp(app->routes[i].path, path) == 0) {
            return &app->routes[i];
        }
    }
    return NULL;
}

static cJSON *lua_http_json_from_value(lua_State *L, int index);

static bool lua_http_table_is_array(lua_State *L, int index)
{
    lua_Integer expected = 1;
    bool has_entries = false;

    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) != expected) {
            lua_pop(L, 2);
            return false;
        }
        has_entries = true;
        expected++;
        lua_pop(L, 1);
    }
    return has_entries;
}

static cJSON *lua_http_json_from_table(lua_State *L, int index)
{
    cJSON *json = lua_http_table_is_array(L, index) ? cJSON_CreateArray() : cJSON_CreateObject();

    if (!json) {
        return NULL;
    }
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        cJSON *child = lua_http_json_from_value(L, -1);
        char key_buf[32];
        const char *key = NULL;

        if (!child) {
            lua_pop(L, 1);
            cJSON_Delete(json);
            return NULL;
        }
        if (cJSON_IsArray(json)) {
            if (!cJSON_AddItemToArray(json, child)) {
                cJSON_Delete(child);
                lua_pop(L, 1);
                cJSON_Delete(json);
                return NULL;
            }
        } else {
            if (lua_type(L, -2) == LUA_TSTRING) {
                key = lua_tostring(L, -2);
            } else if (lua_isinteger(L, -2)) {
                snprintf(key_buf, sizeof(key_buf), "%lld", (long long)lua_tointeger(L, -2));
                key = key_buf;
            }
            if (!key || !cJSON_AddItemToObject(json, key, child)) {
                cJSON_Delete(child);
                lua_pop(L, 1);
                cJSON_Delete(json);
                return NULL;
            }
        }
        lua_pop(L, 1);
    }
    return json;
}

static cJSON *lua_http_json_from_value(lua_State *L, int index)
{
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, index));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, index));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, index));
    case LUA_TTABLE:
        return lua_http_json_from_table(L, index);
    default:
        return NULL;
    }
}

static void lua_http_push_query(lua_State *L, const char *query)
{
    char *copy = NULL;
    char *saveptr = NULL;
    char *token = NULL;

    lua_newtable(L);
    if (!query || !query[0]) {
        return;
    }

    copy = strdup(query);
    if (!copy) {
        luaL_error(L, "out of memory");
    }
    for (token = strtok_r(copy, "&", &saveptr); token; token = strtok_r(NULL, "&", &saveptr)) {
        char *eq = strchr(token, '=');
        char *key = token;
        char *value = eq ? eq + 1 : "";

        if (eq) {
            *eq = '\0';
        }
        lua_http_url_decode_inplace(key);
        lua_http_url_decode_inplace(value);
        if (key[0]) {
            lua_pushstring(L, value);
            lua_setfield(L, -2, key);
        }
    }
    free(copy);
}

static void lua_http_push_request(lua_State *L, const lua_http_request_t *req)
{
    lua_newtable(L);
    lua_pushstring(L, req->method == LUA_HTTP_METHOD_POST ? "POST" : "GET");
    lua_setfield(L, -2, "method");
    lua_pushstring(L, req->path);
    lua_setfield(L, -2, "path");
    lua_http_push_query(L, req->query);
    lua_setfield(L, -2, "query");
    lua_pushlstring(L, req->body ? req->body : "", req->body_len);
    lua_setfield(L, -2, "body");
    lua_pushstring(L, req->content_type);
    lua_setfield(L, -2, "content_type");
}

static void lua_http_set_response_from_lua(lua_State *L, lua_http_request_t *req, int index)
{
    index = lua_absindex(L, index);

    if (lua_isnoneornil(L, index)) {
        lua_http_complete_request(req, ESP_OK, 204, "text/plain; charset=utf-8", NULL);
        return;
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        lua_http_complete_request(req,
                                  ESP_OK,
                                  200,
                                  "text/plain; charset=utf-8",
                                  lua_tostring(L, index));
        return;
    }
    if (!lua_istable(L, index)) {
        lua_http_complete_request(req,
                                  ESP_ERR_INVALID_RESPONSE,
                                  500,
                                  "text/plain; charset=utf-8",
                                  "Lua HTTP callback returned unsupported value");
        return;
    }

    int status = 200;
    lua_getfield(L, index, "status");
    if (lua_isinteger(L, -1)) {
        status = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "json");
    if (!lua_isnil(L, -1)) {
        cJSON *json = lua_http_json_from_value(L, -1);
        char *payload = json ? cJSON_PrintUnformatted(json) : NULL;
        cJSON_Delete(json);
        lua_pop(L, 1);
        if (!payload) {
            lua_http_complete_request(req, ESP_ERR_NO_MEM, 500, "text/plain; charset=utf-8", "JSON serialization failed");
            return;
        }
        lua_http_complete_request(req, ESP_OK, status, "application/json", payload);
        free(payload);
        return;
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "body");
    if (!lua_isnil(L, -1)) {
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            lua_http_complete_request(req,
                                      ESP_ERR_INVALID_RESPONSE,
                                      500,
                                      "text/plain; charset=utf-8",
                                      "response body must be a string");
            return;
        }
        const char *body = lua_tostring(L, -1);
        const char *content_type = "text/plain; charset=utf-8";
        lua_getfield(L, index, "content_type");
        if (lua_isstring(L, -1)) {
            content_type = lua_tostring(L, -1);
        }
        lua_http_complete_request(req, ESP_OK, status, content_type, body);
        lua_pop(L, 2);
        return;
    }
    lua_pop(L, 1);

    lua_http_complete_request(req, ESP_OK, status, "text/plain; charset=utf-8", NULL);
}

static const char *lua_http_status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    case 504: return "504 Gateway Timeout";
    default:  return NULL;
    }
}

static void lua_http_dispatch_request(lua_http_app_t *app, lua_http_request_t *req)
{
    lua_State *L = app->L;
    lua_http_route_t *route = lua_http_find_route(app, req->method, req->path);
    int status;

    if (!route) {
        lua_http_complete_request(req, ESP_ERR_NOT_FOUND, 404, "text/plain; charset=utf-8", "Lua HTTP route not found");
        return;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, route->callback_ref);
    lua_http_push_request(L, req);
    status = lua_pcall(L, 1, 1, 0);
    if (status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        ESP_LOGW(TAG, "callback failed app=%s path=%s: %s",
                 app->app_id,
                 req->path,
                 message ? message : "unknown");
        lua_http_complete_request(req,
                                  ESP_FAIL,
                                  500,
                                  "text/plain; charset=utf-8",
                                  message ? message : "Lua HTTP callback failed");
        lua_pop(L, 1);
        return;
    }
    lua_http_set_response_from_lua(L, req, -1);
    lua_pop(L, 1);
}

static bool lua_http_runtime_stop_requested(lua_State *L)
{
    return cap_lua_runtime_stop_requested(L);
}

static int lua_http_app_gc(lua_State *L)
{
    lua_http_app_ud_t *ud = (lua_http_app_ud_t *)luaL_checkudata(L, 1, LUA_HTTP_APP_MT);

    if (ud && ud->app) {
        lua_http_app_t *app = ud->app;

        lua_http_unregister_app(app);
        for (size_t i = 0; i < app->route_count; i++) {
            if (app->routes[i].callback_ref != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, app->routes[i].callback_ref);
                app->routes[i].callback_ref = LUA_NOREF;
            }
        }
        if (app->queue) {
            vQueueDelete(app->queue);
        }
        free(app);
        ud->app = NULL;
    }
    return 0;
}

static int lua_http_app_mount_static(lua_State *L)
{
    lua_http_app_t *app = lua_http_check_app(L, 1);
    const char *path = luaL_checkstring(L, 2);
    struct stat st = {0};

    if (!lua_http_static_root_is_valid(path)) {
        return luaL_error(L, "static root must be an absolute safe path");
    }
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return luaL_error(L, "static root is not a directory: %s", path);
    }
    strlcpy(app->static_root, path, sizeof(app->static_root));
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_http_app_add_route(lua_State *L, lua_http_method_t method)
{
    lua_http_app_t *app = lua_http_check_app(L, 1);
    const char *path = luaL_checkstring(L, 2);

    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!lua_http_path_is_safe(path, true) || strlen(path) >= LUA_HTTP_PATH_MAX) {
        return luaL_error(L, "route path must be an absolute safe path");
    }
    if (lua_http_find_route(app, method, path)) {
        return luaL_error(L, "duplicate route: %s", path);
    }
    if (app->route_count >= LUA_HTTP_MAX_ROUTES) {
        return luaL_error(L, "too many routes");
    }

    lua_http_route_t *route = &app->routes[app->route_count++];
    route->method = method;
    strlcpy(route->path, path, sizeof(route->path));
    lua_pushvalue(L, 3);
    route->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, 1);
    return 1;
}

static int lua_http_app_get(lua_State *L)
{
    return lua_http_app_add_route(L, LUA_HTTP_METHOD_GET);
}

static int lua_http_app_post(lua_State *L)
{
    return lua_http_app_add_route(L, LUA_HTTP_METHOD_POST);
}

static int lua_http_app_url(lua_State *L)
{
    lua_http_app_t *app = lua_http_check_app(L, 1);
    lua_pushfstring(L, "/lua/%s/", app->app_id);
    return 1;
}

static int lua_http_app_serve_forever(lua_State *L)
{
    lua_http_app_t *app = lua_http_check_app(L, 1);
    lua_http_request_t *req = NULL;

    app->serving = true;
    while (app->active && !lua_http_runtime_stop_requested(L)) {
        if (xQueueReceive(app->queue, &req, pdMS_TO_TICKS(200)) == pdTRUE && req) {
            lua_http_dispatch_request(app, req);
            req = NULL;
        }
    }
    app->serving = false;
    lua_http_unregister_app(app);
    return 0;
}

static int lua_http_app_new(lua_State *L)
{
    const char *app_id = luaL_checkstring(L, 1);
    lua_http_app_t *app = NULL;
    lua_http_app_ud_t *ud = NULL;

    if (!lua_http_app_id_is_valid(app_id)) {
        return luaL_error(L, "app_id may only contain letters, digits, '_' and '-'");
    }

    lua_http_lock();
    if (lua_http_find_app_locked(app_id)) {
        lua_http_unlock();
        return luaL_error(L, "http_server app already exists: %s", app_id);
    }
    lua_http_unlock();

    app = calloc(1, sizeof(*app));
    if (!app) {
        return luaL_error(L, "out of memory");
    }
    app->queue = xQueueCreate(LUA_HTTP_QUEUE_LEN, sizeof(lua_http_request_t *));
    if (!app->queue) {
        free(app);
        return luaL_error(L, "failed to create request queue");
    }
    app->active = true;
    app->L = L;
    strlcpy(app->app_id, app_id, sizeof(app->app_id));
    for (size_t i = 0; i < LUA_HTTP_MAX_ROUTES; i++) {
        app->routes[i].callback_ref = LUA_NOREF;
    }

    lua_http_lock();
    if (lua_http_find_app_locked(app_id)) {
        lua_http_unlock();
        vQueueDelete(app->queue);
        free(app);
        return luaL_error(L, "http_server app already exists: %s", app_id);
    }
    app->next = s_apps;
    s_apps = app;
    lua_http_unlock();

    ud = (lua_http_app_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->app = app;
    luaL_getmetatable(L, LUA_HTTP_APP_MT);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_http_server(lua_State *L)
{
    static const luaL_Reg app_methods[] = {
        {"mount_static", lua_http_app_mount_static},
        {"get", lua_http_app_get},
        {"post", lua_http_app_post},
        {"url", lua_http_app_url},
        {"serve_forever", lua_http_app_serve_forever},
        {NULL, NULL},
    };

    if (luaL_newmetatable(L, LUA_HTTP_APP_MT)) {
        lua_pushcfunction(L, lua_http_app_gc);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        luaL_setfuncs(L, app_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_http_app_new);
    lua_setfield(L, -2, "app");
    return 1;
}

esp_err_t lua_module_http_server_register(void)
{
    return cap_lua_register_module("http_server", luaopen_http_server);
}

static bool lua_http_extract_app_and_path(const char *uri,
                                          const char *prefix,
                                          char *app_id,
                                          size_t app_id_size,
                                          char *path,
                                          size_t path_size,
                                          char *query,
                                          size_t query_size)
{
    const char *cursor = NULL;
    const char *slash = NULL;
    const char *query_start = NULL;
    size_t app_len;
    size_t path_len;

    if (!uri || !prefix || !app_id || !path || !query) {
        return false;
    }
    app_id[0] = '\0';
    path[0] = '\0';
    query[0] = '\0';

    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        return false;
    }
    cursor = uri + strlen(prefix);
    slash = strchr(cursor, '/');
    query_start = strchr(cursor, '?');
    if (!slash || (query_start && query_start < slash)) {
        slash = query_start;
    }
    app_len = slash ? (size_t)(slash - cursor) : strlen(cursor);
    if (app_len == 0 || app_len >= app_id_size) {
        return false;
    }
    memcpy(app_id, cursor, app_len);
    app_id[app_len] = '\0';
    if (!lua_http_app_id_is_valid(app_id)) {
        return false;
    }

    if (slash && *slash == '/') {
        query_start = strchr(slash, '?');
        path_len = query_start ? (size_t)(query_start - slash) : strlen(slash);
        if (path_len == 0) {
            path_len = 1;
        }
        if (path_len >= path_size) {
            return false;
        }
        memcpy(path, slash, path_len);
        path[path_len] = '\0';
    } else {
        strlcpy(path, "/", path_size);
        query_start = slash && *slash == '?' ? slash : strchr(cursor, '?');
    }
    if (query_start && *query_start == '?') {
        strlcpy(query, query_start + 1, query_size);
    }
    lua_http_url_decode_inplace(path);
    return lua_http_path_is_safe(path, true);
}

esp_err_t lua_module_http_server_handle_static(httpd_req_t *req)
{
    char app_id[LUA_HTTP_APP_ID_MAX];
    char rel_path[LUA_HTTP_PATH_MAX];
    char query[4];
    char full_path[LUA_HTTP_PATH_MAX + LUA_HTTP_STATIC_ROOT_MAX];
    char root[LUA_HTTP_STATIC_ROOT_MAX];
    lua_http_app_t *app = NULL;
    bool found = false;
    struct stat st = {0};

    if (!lua_http_extract_app_and_path(req->uri,
                                       "/lua/",
                                       app_id,
                                       sizeof(app_id),
                                       rel_path,
                                       sizeof(rel_path),
                                       query,
                                       sizeof(query))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Lua HTTP path");
    }

    lua_http_lock();
    app = lua_http_find_app_locked(app_id);
    if (app && app->static_root[0]) {
        strlcpy(root, app->static_root, sizeof(root));
        found = true;
    } else {
        root[0] = '\0';
    }
    lua_http_unlock();

    if (!found || !root[0]) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Lua HTTP app not found");
    }

    if (strcmp(rel_path, "/") == 0) {
        strlcpy(rel_path, "/index.html", sizeof(rel_path));
    }
    int written = snprintf(full_path, sizeof(full_path), "%s%s", root, rel_path);
    if (written <= 0 || (size_t)written >= sizeof(full_path)) {
        return httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Path too long");
    }
    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    FILE *file = fopen(full_path, "rb");
    if (!file) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
    }
    char *scratch = malloc(LUA_HTTP_SCRATCH_SIZE);
    if (!scratch) {
        fclose(file);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, lua_http_mime_type(full_path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    while (!feof(file)) {
        size_t read_bytes = fread(scratch, 1, LUA_HTTP_SCRATCH_SIZE, file);
        if (read_bytes > 0 && httpd_resp_send_chunk(req, scratch, read_bytes) != ESP_OK) {
            free(scratch);
            fclose(file);
            return ESP_FAIL;
        }
    }
    free(scratch);
    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t lua_http_read_body(httpd_req_t *req, lua_http_request_t *out)
{
    if (req->content_len <= 0) {
        return ESP_OK;
    }
    if ((size_t)req->content_len > LUA_HTTP_MAX_BODY) {
        return ESP_ERR_INVALID_SIZE;
    }
    out->body = calloc(1, req->content_len + 1);
    if (!out->body) {
        return ESP_ERR_NO_MEM;
    }
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, out->body + received, req->content_len - received);
        if (ret <= 0) {
            return ret == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += ret;
    }
    out->body_len = (size_t)received;
    return ESP_OK;
}

esp_err_t lua_module_http_server_handle_api(httpd_req_t *req)
{
    char app_id[LUA_HTTP_APP_ID_MAX];
    lua_http_request_t *call = NULL;
    lua_http_app_t *app = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;

    call = calloc(1, sizeof(*call));
    if (!call) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    if (req->method != HTTP_GET && req->method != HTTP_POST) {
        free(call);
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    }
    if (!lua_http_extract_app_and_path(req->uri,
                                       "/api/lua/",
                                       app_id,
                                       sizeof(app_id),
                                       call->path,
                                       sizeof(call->path),
                                       call->query,
                                       sizeof(call->query))) {
        free(call);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Lua HTTP API path");
    }
    call->method = req->method == HTTP_POST ? LUA_HTTP_METHOD_POST : LUA_HTTP_METHOD_GET;
    httpd_req_get_hdr_value_str(req, "Content-Type", call->content_type, sizeof(call->content_type));

    err = lua_http_read_body(req, call);
    if (err == ESP_ERR_INVALID_SIZE) {
        free(call);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    }
    if (err != ESP_OK) {
        free(call->body);
        free(call);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read body");
    }

    call->done = xSemaphoreCreateBinary();
    if (!call->done) {
        free(call->body);
        free(call);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    lua_http_lock();
    app = lua_http_find_app_locked(app_id);
    if (app && xQueueSend(app->queue, &call, 0) != pdTRUE) {
        app = NULL;
        err = ESP_ERR_TIMEOUT;
    }
    lua_http_unlock();

    if (!app) {
        vSemaphoreDelete(call->done);
        free(call->body);
        free(call);
        if (err == ESP_ERR_TIMEOUT) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "Lua HTTP queue full");
        }
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Lua HTTP app not found");
    }

    if (xSemaphoreTake(call->done, pdMS_TO_TICKS(LUA_HTTP_WAIT_MS)) != pdTRUE) {
        httpd_resp_set_status(req, "504 Gateway Timeout");
        return httpd_resp_sendstr(req, "Lua HTTP callback timed out");
    }

    vSemaphoreDelete(call->done);
    free(call->body);
    if (call->err != ESP_OK && call->status == 404) {
        free(call->response);
        free(call);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Lua HTTP route not found");
    }
    if (call->status < 100 || call->status > 599) {
        call->status = call->err == ESP_OK ? 200 : 500;
    }
    const char *known_status = lua_http_status_line(call->status);
    char custom_status[32];
    if (known_status) {
        httpd_resp_set_status(req, known_status);
    } else {
        snprintf(custom_status, sizeof(custom_status), "%d Lua", call->status);
        httpd_resp_set_status(req, custom_status);
    }
    httpd_resp_set_type(req, call->content_type_out[0] ? call->content_type_out : "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    err = httpd_resp_send(req, call->response, call->response ? call->response_len : 0);
    free(call->response);
    free(call);
    return err;
}
