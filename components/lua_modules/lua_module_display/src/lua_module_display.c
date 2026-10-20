/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_display.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cap_lua.h"
#include "display_bitmap.h"
#include "display_core.h"
#include "display_text.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua_image.h"

static const char *TAG = "lua_display";
#define DISPLAY_SCREEN_MT "display.screen"
#define DISPLAY_FONT_MT "display.font"
#define DISPLAY_OWNER "lua_display"
#define DISPLAY_EXIT_TASK_STACK 3072
#define DISPLAY_EXIT_TASK_PRIORITY 5
#define DISPLAY_EXIT_WAIT_MS 5000

typedef struct { display_handle_t handle; display_service_session_handle_t pending_session; } lua_display_screen_t;
typedef struct { display_font_handle_t handle; } lua_display_font_t;
typedef struct { char job_id[CAP_LUA_JOB_ID_LEN]; char output[256]; } lua_display_exit_ctx_t;

static SemaphoreHandle_t s_guard;
static bool s_opened, s_exit_pending, s_recovering;
static lua_display_screen_t s_orphan;
static char s_job_id[CAP_LUA_JOB_ID_LEN];
static char s_registry_screen_key;

static int lua_display_error(lua_State *L, const char *operation, esp_err_t err)
{
    return luaL_error(L, "display %s failed: %s", operation, esp_err_to_name(err));
}

static int lua_display_integer(lua_State *L, int index, const char *name)
{
    if (!lua_isinteger(L, index)) luaL_error(L, "display %s must be an integer", name);
    lua_Integer value = lua_tointeger(L, index);
    if (value < INT_MIN || value > INT_MAX) luaL_error(L, "display %s is out of range", name);
    return (int)value;
}

static uint8_t lua_display_byte(lua_State *L, int index, const char *name)
{
    int value = lua_display_integer(L, index, name);
    if (value < 0 || value > 255) luaL_error(L, "display %s must be in [0, 255]", name);
    return value;
}

static void lua_display_raw_field(lua_State *L, int table, const char *field)
{
    table = lua_absindex(L, table);
    lua_pushstring(L, field);
    lua_rawget(L, table);
}

static int lua_display_field_integer(lua_State *L, int table, const char *field, int fallback)
{
    lua_display_raw_field(L, table, field);
    int value = lua_isnil(L, -1) ? fallback : lua_display_integer(L, -1, field);
    lua_pop(L, 1);
    return value;
}

static bool lua_display_field_boolean(lua_State *L, int table, const char *field, bool fallback)
{
    lua_display_raw_field(L, table, field);
    if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) luaL_error(L, "display %s must be a boolean", field);
    bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static uint8_t lua_display_color_channel(lua_State *L, int table, const char *name, int ordinal, int fallback)
{
    lua_display_raw_field(L, table, name);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, table, ordinal); }
    uint8_t value = lua_isnil(L, -1) ? fallback : lua_display_byte(L, -1, name);
    lua_pop(L, 1);
    return value;
}

static int lua_display_hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static display_color_t lua_display_color(lua_State *L, int index)
{
    display_color_t color = { .a = 255 };
    if (lua_isinteger(L, index)) {
        int64_t value = lua_tointeger(L, index);
        if (value < INT32_MIN || value > UINT32_MAX) luaL_error(L, "display packed color is out of range");
        uint32_t packed = (uint32_t)value;
        color.r = (packed >> 16) & 255; color.g = (packed >> 8) & 255;
        color.b = packed & 255; color.a = (packed >> 24) & 255;
    } else if (lua_type(L, index) == LUA_TSTRING) {
        size_t length;
        const char *value = lua_tolstring(L, index, &length);
        if ((length != 7 && length != 9) || value[0] != '#') luaL_error(L, "display color must be #rrggbb or #rrggbbaa");
        uint8_t *channels[] = {&color.r, &color.g, &color.b, &color.a};
        for (size_t i = 0; i < (length - 1) / 2; ++i) {
            int hi = lua_display_hex_digit(value[1 + i * 2]), lo = lua_display_hex_digit(value[2 + i * 2]);
            if (hi < 0 || lo < 0) luaL_error(L, "display color contains a non-hex digit");
            *channels[i] = (hi << 4) | lo;
        }
    } else if (lua_istable(L, index)) {
        color.r = lua_display_color_channel(L, index, "r", 1, 0);
        color.g = lua_display_color_channel(L, index, "g", 2, 0);
        color.b = lua_display_color_channel(L, index, "b", 3, 0);
        color.a = lua_display_color_channel(L, index, "a", 4, 255);
    } else luaL_error(L, "display color must be packed ARGB, hex, or table");
    return color;
}

static lua_display_screen_t *lua_display_screen(lua_State *L)
{
    lua_display_screen_t *screen = luaL_checkudata(L, 1, DISPLAY_SCREEN_MT);
    if (screen->handle == NULL) luaL_error(L, "display screen is closed");
    return screen;
}

static display_raster_t *lua_display_draw(lua_State *L)
{
    display_raster_t *raster = display_draw_view(lua_display_screen(L)->handle);
    if (raster == NULL) luaL_error(L, "display frame is not active");
    return raster;
}

static void lua_display_exit_task(void *arg)
{
    lua_display_exit_ctx_t *ctx = arg;
    esp_err_t err = cap_lua_stop_job(ctx->job_id, DISPLAY_EXIT_WAIT_MS, ctx->output, sizeof(ctx->output));
    if (err != ESP_OK) ESP_LOGW(TAG, "exit gesture stop failed: %s", esp_err_to_name(err));
    free(ctx);
    if (xSemaphoreTake(s_guard, portMAX_DELAY) == pdTRUE) { s_exit_pending = false; xSemaphoreGive(s_guard); }
    vTaskDelete(NULL);
}

static void lua_display_exit_request(display_service_session_handle_t session, void *user_ctx)
{
    (void)session; (void)user_ctx;
    if (xSemaphoreTake(s_guard, 0) != pdTRUE) return;
    if (s_exit_pending || !s_job_id[0]) { xSemaphoreGive(s_guard); return; }
    lua_display_exit_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) { xSemaphoreGive(s_guard); ESP_LOGE(TAG, "exit gesture allocation failed"); return; }
    strlcpy(ctx->job_id, s_job_id, sizeof(ctx->job_id));
    s_exit_pending = true;
    xSemaphoreGive(s_guard);
    if (xTaskCreate(lua_display_exit_task, "display_exit", DISPLAY_EXIT_TASK_STACK, ctx, DISPLAY_EXIT_TASK_PRIORITY, NULL) != pdPASS) {
        if (xSemaphoreTake(s_guard, portMAX_DELAY) == pdTRUE) { s_exit_pending = false; xSemaphoreGive(s_guard); }
        free(ctx);
        ESP_LOGE(TAG, "exit gesture task creation failed");
    }
}

static esp_err_t lua_display_close_native(lua_State *L, lua_display_screen_t *screen)
{
    if (screen->handle == NULL && screen->pending_session == NULL) return ESP_OK;
    if (screen->handle != NULL) {
        esp_err_t err = display_delete(screen->handle);
        if (err != ESP_OK) return err;
        screen->handle = NULL;
    }
    if (screen->pending_session != NULL) {
        if (display_service_session_is_valid(screen->pending_session)) {
            esp_err_t err = display_service_close(screen->pending_session);
            if (err != ESP_OK) return err;
        }
        screen->pending_session = NULL;
    }
    if (xSemaphoreTake(s_guard, portMAX_DELAY) == pdTRUE) {
        s_opened = false;
        s_job_id[0] = '\0';
        xSemaphoreGive(s_guard);
    }
    lua_pushboolean(L, false);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &s_registry_screen_key);
    return ESP_OK;
}

static void lua_display_orphan_native(lua_State *L, lua_display_screen_t *screen)
{
    if (screen->handle == NULL && screen->pending_session == NULL) return;
    if (screen->handle != NULL) {
        esp_err_t err = display_detach_for_cleanup(screen->handle);
        if (err != ESP_OK) { ESP_LOGE(TAG, "screen cleanup detach failed: %s", esp_err_to_name(err)); return; }
    }
    if (xSemaphoreTake(s_guard, portMAX_DELAY) != pdTRUE) return;
    bool transferred = s_orphan.handle == NULL && s_orphan.pending_session == NULL;
    if (transferred) {
        s_orphan = *screen;
        screen->handle = NULL;
        screen->pending_session = NULL;
    }
    xSemaphoreGive(s_guard);
    if (transferred) { lua_pushboolean(L, false); lua_rawsetp(L, LUA_REGISTRYINDEX, &s_registry_screen_key); }
    else ESP_LOGE(TAG, "screen cleanup orphan slot occupied");
}

static int lua_display_close(lua_State *L)
{
    lua_display_screen_t *screen = luaL_checkudata(L, 1, DISPLAY_SCREEN_MT);
    esp_err_t err = lua_display_close_native(L, screen);
    if (err != ESP_OK) return lua_display_error(L, "close", err);
    return 0;
}

static int lua_display_gc(lua_State *L)
{
    lua_display_screen_t *screen = luaL_testudata(L, 1, DISPLAY_SCREEN_MT);
    if (screen != NULL) {
        esp_err_t err = lua_display_close_native(L, screen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "screen finalizer close failed: %s", esp_err_to_name(err));
            lua_display_orphan_native(L, screen);
        }
    }
    return 0;
}

static void lua_display_exit_cleanup(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &s_registry_screen_key);
    lua_display_screen_t *screen = luaL_testudata(L, -1, DISPLAY_SCREEN_MT);
    if (screen != NULL && (screen->handle != NULL || screen->pending_session != NULL)) {
        esp_err_t err = lua_display_close_native(L, screen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Lua exit display cleanup failed: %s", esp_err_to_name(err));
            lua_display_orphan_native(L, screen);
        }
    }
    lua_pop(L, 1);
}

static int lua_display_open(lua_State *L)
{
    int count = 1;
    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        count = lua_display_field_integer(L, 1, "framebuffer_count", 1);
    }
    if (count < 1 || count > 2) luaL_error(L, "display framebuffer_count must be 1 or 2");
    lua_display_screen_t *screen = lua_newuserdata(L, sizeof(*screen));
    screen->handle = NULL;
    screen->pending_session = NULL;
    luaL_getmetatable(L, DISPLAY_SCREEN_MT);
    lua_setmetatable(L, -2);
    if (!lua_checkstack(L, 2)) return luaL_error(L, "display Lua stack unavailable");
    if (xSemaphoreTake(s_guard, portMAX_DELAY) != pdTRUE) return luaL_error(L, "display guard unavailable");
    if (s_recovering) { xSemaphoreGive(s_guard); return luaL_error(L, "display already open"); }
    if (s_opened && (s_orphan.handle != NULL || s_orphan.pending_session != NULL)) {
        lua_display_screen_t orphan = s_orphan;
        s_orphan = (lua_display_screen_t) {0};
        s_recovering = true;
        xSemaphoreGive(s_guard);
        esp_err_t recover_err = lua_display_close_native(L, &orphan);
        if (xSemaphoreTake(s_guard, portMAX_DELAY) != pdTRUE) return luaL_error(L, "display guard unavailable");
        if (recover_err != ESP_OK) s_orphan = orphan;
        s_recovering = false;
        xSemaphoreGive(s_guard);
        if (recover_err != ESP_OK) return lua_display_error(L, "recover", recover_err);
        if (xSemaphoreTake(s_guard, portMAX_DELAY) != pdTRUE) return luaL_error(L, "display guard unavailable");
    }
    if (s_opened) { xSemaphoreGive(s_guard); return luaL_error(L, "display already open"); }
    s_opened = true;
    const char *job_id = cap_lua_runtime_job_id(L);
    if (job_id != NULL) strlcpy(s_job_id, job_id, sizeof(s_job_id));
    else s_job_id[0] = '\0';
    xSemaphoreGive(s_guard);
    /* The registry slot is preallocated at module load, so this update cannot allocate. */
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &s_registry_screen_key);

    display_service_session_handle_t session = NULL;
    esp_err_t err = display_service_open(&(display_service_session_config_t) {
        .owner_name = DISPLAY_OWNER, .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
        .exit_request_cb = lua_display_exit_request,
    }, &session);
    display_service_info_t info = {0};
    display_handle_t handle = NULL;
    if (err == ESP_OK) err = display_service_session_get_info(session, &info);
    display_pixel_format_t format = info.bits_per_pixel == 16 ? DISPLAY_PIXEL_FORMAT_RGB565 : DISPLAY_PIXEL_FORMAT_RGB888;
    if (err == ESP_OK && info.bits_per_pixel != 16 && info.bits_per_pixel != 24) err = ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_OK) err = display_create(&(display_config_t) {
        .session = session, .info = info, .pixel_format = format,
        .framebuffer_count = count,
    }, &handle);
    if (err != ESP_OK) {
        if (session != NULL) {
            esp_err_t close_err = display_service_close(session);
            if (close_err != ESP_OK) {
                screen->pending_session = session;
                ESP_LOGE(TAG, "open rollback session close failed: %s", esp_err_to_name(close_err));
                return lua_display_error(L, "open", err);
            }
        }
        if (xSemaphoreTake(s_guard, portMAX_DELAY) == pdTRUE) { s_opened = false; s_job_id[0] = '\0'; xSemaphoreGive(s_guard); }
        lua_pushboolean(L, false); lua_rawsetp(L, LUA_REGISTRYINDEX, &s_registry_screen_key);
        return lua_display_error(L, "open", err);
    }
    screen->handle = handle;
    return 1;
}

static int lua_display_pack_color(lua_State *L)
{
    uint32_t r = lua_display_byte(L, 1, "r"), g = lua_display_byte(L, 2, "g"), b = lua_display_byte(L, 3, "b");
    uint32_t a = lua_isnoneornil(L, 4) ? 255 : lua_display_byte(L, 4, "a");
    lua_pushinteger(L, (lua_Integer)((a << 24) | (r << 16) | (g << 8) | b));
    return 1;
}

static void lua_display_table_integer(lua_State *L, const char *field, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, field);
}

static int lua_display_info(lua_State *L)
{
    display_config_t config = *display_get_config(lua_display_screen(L)->handle);
    size_t bpp = config.pixel_format == DISPLAY_PIXEL_FORMAT_RGB565 ? 2 : 3;
    lua_createtable(L, 0, 7);
    lua_display_table_integer(L, "width", config.info.width);
    lua_display_table_integer(L, "height", config.info.height);
    lua_pushstring(L, config.pixel_format == DISPLAY_PIXEL_FORMAT_RGB565 ? "rgb565" : "rgb888"); lua_setfield(L, -2, "pixel_format");
    lua_display_table_integer(L, "bytes_per_pixel", bpp);
    lua_display_table_integer(L, "framebuffer_count", config.framebuffer_count);
    lua_display_table_integer(L, "framebuffer_bytes", (size_t)config.info.width * config.info.height * bpp * config.framebuffer_count);
    lua_pushboolean(L, config.info.touch_available); lua_setfield(L, -2, "touch_available");
    return 1;
}

static int lua_display_stats(lua_State *L)
{
    display_stats_t stats = *display_get_stats(lua_display_screen(L)->handle);
    lua_createtable(L, 0, 7);
    lua_display_table_integer(L, "draw_us", stats.draw_us);
    lua_display_table_integer(L, "present_us", stats.present_us);
    lua_display_table_integer(L, "sync_us", stats.sync_us);
    lua_display_table_integer(L, "dirty_pixels", stats.dirty_pixels);
    lua_display_table_integer(L, "dirty_rects", stats.dirty_rects);
    lua_display_table_integer(L, "submitted_bytes", stats.submitted_bytes);
    lua_display_table_integer(L, "framebuffer_bytes", stats.framebuffer_bytes);
    return 1;
}

static int lua_display_begin(lua_State *L)
{
    lua_display_screen_t *screen = lua_display_screen(L);
    bool clear = false;
    display_color_t color = { .a = 255 };
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_display_raw_field(L, 2, "clear");
        if (!lua_isnil(L, -1)) { clear = true; color = lua_display_color(L, -1); }
        lua_pop(L, 1);
    }
    esp_err_t err = display_begin(screen->handle, clear, color);
    return err == ESP_OK ? 0 : lua_display_error(L, "begin", err);
}

static int lua_display_present(lua_State *L)
{
    lua_display_screen_t *screen = lua_display_screen(L);
    bool full = false, updated = false;
    if (!lua_isnoneornil(L, 2)) { luaL_checktype(L, 2, LUA_TTABLE); full = lua_display_field_boolean(L, 2, "full", false); }
    esp_err_t err = display_present(screen->handle, full, &updated);
    if (err != ESP_OK) return lua_display_error(L, "present", err);
    lua_pushboolean(L, updated);
    return 1;
}

static int lua_display_save(lua_State *L)
{
    esp_err_t err = display_save(lua_display_screen(L)->handle);
    return err == ESP_OK ? 0 : lua_display_error(L, "save", err);
}

static int lua_display_restore(lua_State *L)
{
    esp_err_t err = display_restore(lua_display_screen(L)->handle);
    return err == ESP_OK ? 0 : lua_display_error(L, "restore", err);
}

static int lua_display_translate(lua_State *L)
{
    esp_err_t err = display_translate(lua_display_screen(L)->handle, lua_display_integer(L, 2, "dx"), lua_display_integer(L, 3, "dy"));
    return err == ESP_OK ? 0 : lua_display_error(L, "translate", err);
}

static int lua_display_clip(lua_State *L)
{
    esp_err_t err = display_clip(lua_display_screen(L)->handle, lua_display_integer(L, 2, "x"), lua_display_integer(L, 3, "y"),
                                 lua_display_integer(L, 4, "width"), lua_display_integer(L, 5, "height"));
    return err == ESP_OK ? 0 : lua_display_error(L, "clip", err);
}

static int lua_display_fill_rect(lua_State *L)
{
    int x = lua_display_integer(L, 2, "x"), y = lua_display_integer(L, 3, "y");
    int w = lua_display_integer(L, 4, "width"), h = lua_display_integer(L, 5, "height");
    display_color_t color = lua_display_color(L, 6);
    display_raster_fill_rect(lua_display_draw(L), x, y, w, h, color);
    return 0;
}

static int lua_display_stroke_rect(lua_State *L)
{
    int x = lua_display_integer(L, 2, "x"), y = lua_display_integer(L, 3, "y");
    int w = lua_display_integer(L, 4, "width"), h = lua_display_integer(L, 5, "height");
    display_color_t color = lua_display_color(L, 6);
    display_raster_stroke_rect(lua_display_draw(L), x, y, w, h, color);
    return 0;
}

static int lua_display_line(lua_State *L)
{
    int x0 = lua_display_integer(L, 2, "x0"), y0 = lua_display_integer(L, 3, "y0");
    int x1 = lua_display_integer(L, 4, "x1"), y1 = lua_display_integer(L, 5, "y1");
    display_color_t color = lua_display_color(L, 6);
    display_raster_line(lua_display_draw(L), x0, y0, x1, y1, color);
    return 0;
}

static int lua_display_fill_circle(lua_State *L)
{
    int cx = lua_display_integer(L, 2, "cx"), cy = lua_display_integer(L, 3, "cy");
    int radius = lua_display_integer(L, 4, "radius");
    display_color_t color = lua_display_color(L, 5);
    display_raster_fill_circle(lua_display_draw(L), cx, cy, radius, color);
    return 0;
}

static int lua_display_stroke_circle(lua_State *L)
{
    int cx = lua_display_integer(L, 2, "cx"), cy = lua_display_integer(L, 3, "cy");
    int radius = lua_display_integer(L, 4, "radius");
    display_color_t color = lua_display_color(L, 5);
    display_raster_stroke_circle(lua_display_draw(L), cx, cy, radius, color);
    return 0;
}

static int lua_display_arc(lua_State *L)
{
    int cx = lua_display_integer(L, 2, "cx"), cy = lua_display_integer(L, 3, "cy"), radius = lua_display_integer(L, 4, "radius");
    double start = luaL_checknumber(L, 5), end = luaL_checknumber(L, 6);
    if (!isfinite(start) || !isfinite(end)) luaL_error(L, "display arc angles must be finite");
    display_color_t color = lua_display_color(L, 7);
    display_raster_arc(lua_display_draw(L), cx, cy, radius, start, end, color);
    return 0;
}

static int lua_display_fill_round_rect(lua_State *L)
{
    int x = lua_display_integer(L, 2, "x"), y = lua_display_integer(L, 3, "y");
    int w = lua_display_integer(L, 4, "width"), h = lua_display_integer(L, 5, "height");
    int radius = lua_display_integer(L, 6, "radius");
    display_color_t color = lua_display_color(L, 7);
    display_raster_fill_round_rect(lua_display_draw(L), x, y, w, h, radius, color);
    return 0;
}

static int lua_display_stroke_round_rect(lua_State *L)
{
    int x = lua_display_integer(L, 2, "x"), y = lua_display_integer(L, 3, "y");
    int w = lua_display_integer(L, 4, "width"), h = lua_display_integer(L, 5, "height");
    int radius = lua_display_integer(L, 6, "radius");
    display_color_t color = lua_display_color(L, 7);
    display_raster_stroke_round_rect(lua_display_draw(L), x, y, w, h, radius, color);
    return 0;
}

static int lua_display_fill_triangle(lua_State *L)
{
    int x1 = lua_display_integer(L, 2, "x1"), y1 = lua_display_integer(L, 3, "y1");
    int x2 = lua_display_integer(L, 4, "x2"), y2 = lua_display_integer(L, 5, "y2");
    int x3 = lua_display_integer(L, 6, "x3"), y3 = lua_display_integer(L, 7, "y3");
    display_color_t color = lua_display_color(L, 8);
    display_raster_fill_triangle(lua_display_draw(L), x1, y1, x2, y2, x3, y3, color);
    return 0;
}

static int lua_display_touch(lua_State *L)
{
    const display_config_t *config = display_get_config(lua_display_screen(L)->handle);
    display_service_touch_snapshot_t snapshot = {0};
    esp_err_t err = display_service_session_get_touch_snapshot(config->session, &snapshot);
    if (err != ESP_OK) return lua_display_error(L, "touch", err);
    lua_createtable(L, 0, 1);
    lua_createtable(L, snapshot.count, 0);
    for (uint8_t i = 0; i < snapshot.count; ++i) {
        lua_createtable(L, 0, 3);
        lua_display_table_integer(L, "id", snapshot.points[i].id);
        lua_display_table_integer(L, "x", snapshot.points[i].x);
        lua_display_table_integer(L, "y", snapshot.points[i].y);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "points");
    return 1;
}

static display_bitmap_mode_t lua_display_bitmap_mode(lua_State *L, int table)
{
    lua_display_raw_field(L, table, "mode");
    const char *mode = lua_isnil(L, -1) ? "raw" : luaL_checkstring(L, -1);
    display_bitmap_mode_t parsed = DISPLAY_BITMAP_RAW;
    if (strcmp(mode, "raw") == 0) parsed = DISPLAY_BITMAP_RAW;
    else if (strcmp(mode, "contain") == 0) parsed = DISPLAY_BITMAP_CONTAIN;
    else if (strcmp(mode, "cover") == 0) parsed = DISPLAY_BITMAP_COVER;
    else if (strcmp(mode, "stretch") == 0) parsed = DISPLAY_BITMAP_STRETCH;
    else if (strcmp(mode, "crop") == 0) parsed = DISPLAY_BITMAP_CROP;
    else luaL_error(L, "display image mode is invalid");
    lua_pop(L, 1);
    return parsed;
}

static display_bitmap_options_t lua_display_bitmap_options(lua_State *L, int index)
{
    display_bitmap_options_t options = { .opacity = 255 };
    if (lua_isnoneornil(L, index)) return options;
    luaL_checktype(L, index, LUA_TTABLE);
    options.mode = lua_display_bitmap_mode(L, index);
    options.width = lua_display_field_integer(L, index, "width", 0);
    options.height = lua_display_field_integer(L, index, "height", 0);
    int opacity = lua_display_field_integer(L, index, "opacity", 255);
    if (opacity < 0 || opacity > 255) luaL_error(L, "display opacity must be in [0, 255]");
    options.opacity = opacity;
    lua_display_raw_field(L, index, "source");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        options.source_x = lua_display_field_integer(L, -1, "x", 0);
        options.source_y = lua_display_field_integer(L, -1, "y", 0);
        options.source_width = lua_display_field_integer(L, -1, "width", 0);
        options.source_height = lua_display_field_integer(L, -1, "height", 0);
    }
    lua_pop(L, 1);
    return options;
}

static int lua_display_blit(lua_State *L)
{
    int x = lua_display_integer(L, 2, "x"), y = lua_display_integer(L, 3, "y");
    size_t bytes;
    luaL_checktype(L, 4, LUA_TSTRING);
    const uint8_t *pixels = (const uint8_t *)luaL_checklstring(L, 4, &bytes);
    luaL_checktype(L, 5, LUA_TTABLE);
    int width = lua_display_field_integer(L, 5, "width", 0), height = lua_display_field_integer(L, 5, "height", 0);
    lua_display_raw_field(L, 5, "format");
    const char *format = luaL_checkstring(L, -1);
    display_bitmap_format_t bitmap_format = DISPLAY_BITMAP_RGB565;
    if (strcmp(format, "rgb565") == 0) bitmap_format = DISPLAY_BITMAP_RGB565;
    else if (strcmp(format, "rgb888") == 0) bitmap_format = DISPLAY_BITMAP_RGB888;
    else if (strcmp(format, "bgr888") == 0) bitmap_format = DISPLAY_BITMAP_BGR888;
    else luaL_error(L, "display blit format is invalid");
    lua_pop(L, 1);
    if (width <= 0 || height <= 0) luaL_error(L, "display blit requires positive width and height");
    size_t bpp = bitmap_format == DISPLAY_BITMAP_RGB565 ? 2 : 3;
    if ((size_t)width > SIZE_MAX / (size_t)height || (size_t)width * (size_t)height > SIZE_MAX / bpp ||
        bytes != (size_t)width * (size_t)height * bpp) luaL_error(L, "display blit byte length does not match dimensions");
    display_bitmap_view_t view = {.pixels = pixels, .length = bytes, .width = width, .height = height, .format = bitmap_format};
    display_bitmap_options_t options = {.opacity = 255};
    esp_err_t err = display_bitmap_draw(lua_display_draw(L), x, y, &view, &options);
    return err == ESP_OK ? 0 : lua_display_error(L, "blit", err);
}

static int lua_display_image(lua_State *L)
{
    int x = lua_display_integer(L, 2, "x"), y = lua_display_integer(L, 3, "y");
    display_bitmap_options_t options = lua_display_bitmap_options(L, 5);
    lua_display_screen_t *screen = lua_display_screen(L);
    lua_image_view_t image = {0};
    display_config_t config = *display_get_config(screen->handle);
    lua_image_format_t requested = config.pixel_format == DISPLAY_PIXEL_FORMAT_RGB565 ? LUA_IMAGE_FORMAT_RGB565LE : LUA_IMAGE_FORMAT_BGR888;
    esp_err_t err = lua_image_require_format(L, 4, requested, &image);
    if (err != ESP_OK) return lua_display_error(L, "image", err);
    display_raster_t *r = display_draw_view(screen->handle);
    if (r == NULL) {
        lua_image_release_view(&image);
        return luaL_error(L, "display frame is not active");
    }
    display_bitmap_view_t view = { .pixels = image.data, .length = image.bytes, .width = image.width, .height = image.height,
                                   .format = r->bpp == 2 ? DISPLAY_BITMAP_RGB565 : DISPLAY_BITMAP_BGR888 };
    err = display_bitmap_draw(r, x, y, &view, &options);
    lua_image_release_view(&image);
    return err == ESP_OK ? 0 : lua_display_error(L, "image", err);
}

static int lua_display_font_close(lua_State *L)
{
    lua_display_font_t *font = luaL_checkudata(L, 1, DISPLAY_FONT_MT);
    display_font_delete(font->handle);
    font->handle = NULL;
    return 0;
}

static int lua_display_load_font(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_display_font_t *font = lua_newuserdata(L, sizeof(*font));
    font->handle = NULL;
    luaL_getmetatable(L, DISPLAY_FONT_MT);
    lua_setmetatable(L, -2);
    display_font_handle_t handle = NULL;
    esp_err_t err = display_font_create(path, &handle);
    if (err != ESP_OK) return lua_display_error(L, "load_font", err);
    font->handle = handle;
    return 1;
}

static display_text_options_t lua_display_text_options(lua_State *L, int index)
{
    display_text_options_t options = { .font_size = 24, .color = {.r = 255, .g = 255, .b = 255, .a = 255} };
    if (lua_isnoneornil(L, index)) return options;
    luaL_checktype(L, index, LUA_TTABLE);
    options.font_size = lua_display_field_integer(L, index, "font_size", 24);
    lua_display_raw_field(L, index, "color");
    if (!lua_isnil(L, -1)) options.color = lua_display_color(L, -1);
    lua_pop(L, 1);
    lua_display_raw_field(L, index, "font");
    if (!lua_isnil(L, -1)) {
        lua_display_font_t *font = luaL_checkudata(L, -1, DISPLAY_FONT_MT);
        if (font->handle == NULL) luaL_error(L, "display font is closed");
        options.font = font->handle;
    }
    lua_pop(L, 1);
    return options;
}

static int lua_display_measure_text(lua_State *L)
{
    (void)lua_display_screen(L);
    size_t length;
    luaL_checktype(L, 2, LUA_TSTRING);
    const char *text = luaL_checklstring(L, 2, &length);
    display_text_options_t options = lua_display_text_options(L, 3);
    int width = 0, height = 0;
    esp_err_t err = display_text_measure(text, length, &options, &width, &height);
    if (err != ESP_OK) return lua_display_error(L, "measure_text", err);
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    return 2;
}

static int lua_display_text(lua_State *L)
{
    int x = lua_display_integer(L, 2, "x"), y = lua_display_integer(L, 3, "y");
    size_t length;
    luaL_checktype(L, 4, LUA_TSTRING);
    const char *text = luaL_checklstring(L, 4, &length);
    display_text_options_t options = lua_display_text_options(L, 5);
    esp_err_t err = display_text_draw(lua_display_draw(L), x, y, text, length, &options);
    return err == ESP_OK ? 0 : lua_display_error(L, "text", err);
}

static const luaL_Reg s_screen_methods[] = {
    {"info", lua_display_info}, {"stats", lua_display_stats},
    {"close", lua_display_close}, {"begin", lua_display_begin}, {"present", lua_display_present},
    {"save", lua_display_save}, {"restore", lua_display_restore}, {"translate", lua_display_translate}, {"clip", lua_display_clip},
    {"fill_rect", lua_display_fill_rect}, {"stroke_rect", lua_display_stroke_rect}, {"line", lua_display_line},
    {"fill_circle", lua_display_fill_circle}, {"stroke_circle", lua_display_stroke_circle}, {"arc", lua_display_arc},
    {"fill_round_rect", lua_display_fill_round_rect}, {"stroke_round_rect", lua_display_stroke_round_rect},
    {"fill_triangle", lua_display_fill_triangle}, {"touch", lua_display_touch}, {"image", lua_display_image}, {"blit", lua_display_blit},
    {"text", lua_display_text}, {"measure_text", lua_display_measure_text},
    {NULL, NULL},
};

int luaopen_display(lua_State *L)
{
    lua_pushboolean(L, false);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &s_registry_screen_key);
    if (luaL_newmetatable(L, DISPLAY_FONT_MT)) {
        lua_pushcfunction(L, lua_display_font_close); lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, lua_display_font_close); lua_setfield(L, -2, "__close");
        lua_newtable(L); lua_pushcfunction(L, lua_display_font_close); lua_setfield(L, -2, "close"); lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, DISPLAY_SCREEN_MT)) {
        lua_pushcfunction(L, lua_display_gc); lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, lua_display_close); lua_setfield(L, -2, "__close");
        lua_newtable(L); luaL_setfuncs(L, s_screen_methods, 0); lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, lua_display_open); lua_setfield(L, -2, "open");
    lua_pushcfunction(L, lua_display_pack_color); lua_setfield(L, -2, "color");
    lua_pushcfunction(L, lua_display_load_font); lua_setfield(L, -2, "load_font");
    return 1;
}

esp_err_t lua_module_display_register(void)
{
    if (s_guard == NULL) s_guard = xSemaphoreCreateMutex();
    if (s_guard == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = cap_lua_register_module("display", luaopen_display);
    return err == ESP_OK ? cap_lua_register_exit_cleanup(lua_display_exit_cleanup) : err;
}
