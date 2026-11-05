/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_vision.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vision_core.h"
#include "lauxlib.h"
#include "lua_image.h"
#include "vision_core_runtime.h"

#define LUA_QRCODE_RESULT_CAPACITY 4
#define LUA_QRCODE_PAYLOAD_CAPACITY 256

static const char *TAG = "lua_qrcode_detect";

typedef struct {
    esp_vision_qrcode_t codes[LUA_QRCODE_RESULT_CAPACITY];
    char payloads[LUA_QRCODE_RESULT_CAPACITY][LUA_QRCODE_PAYLOAD_CAPACITY];
} lua_qrcode_scratch_t;

static bool lua_qrcode_get_integer(lua_State *L, int table_idx, const char *name, int *out)
{
    lua_getfield(L, table_idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_Number number = lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (number != number || number < INT_MIN || number > INT_MAX) {
        return false;
    }
    int value = (int)number;
    if ((lua_Number)value != number) {
        return false;
    }
    *out = value;
    return true;
}

static bool lua_qrcode_parse_roi(lua_State *L, int opts_idx, int width, int height, esp_vision_rect_t *roi)
{
    int x = 0;
    int y = 0;
    int roi_width = width;
    int roi_height = height;

    if (width <= 0 || height <= 0 || width > INT16_MAX || height > INT16_MAX) {
        return false;
    }
    *roi = (esp_vision_rect_t) {.x = 0, .y = 0, .w = (int16_t)width, .h = (int16_t)height};
    if (opts_idx == 0) {
        return true;
    }
    if (!lua_istable(L, opts_idx)) {
        return false;
    }
    lua_getfield(L, opts_idx, "roi");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    int roi_idx = lua_gettop(L);
    bool valid = lua_qrcode_get_integer(L, roi_idx, "x", &x) && lua_qrcode_get_integer(L, roi_idx, "y", &y) &&
                 lua_qrcode_get_integer(L, roi_idx, "width", &roi_width) && lua_qrcode_get_integer(L, roi_idx, "height", &roi_height);
    lua_pop(L, 1);
    if (!valid || x < 0 || y < 0 || roi_width <= 0 || roi_height <= 0 || x > width || y > height ||
        roi_width > width - x || roi_height > height - y || x > INT16_MAX || y > INT16_MAX ||
        roi_width > INT16_MAX || roi_height > INT16_MAX) {
        return false;
    }
    *roi = (esp_vision_rect_t) {.x = (int16_t)x, .y = (int16_t)y, .w = (int16_t)roi_width, .h = (int16_t)roi_height};
    return true;
}

static void lua_qrcode_push_point(lua_State *L, const esp_vision_point_t *point)
{
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, point->x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, point->y);
    lua_setfield(L, -2, "y");
}

static void lua_qrcode_push_code(lua_State *L, const esp_vision_qrcode_t *code, const char *payload)
{
    size_t payload_size = code->payload_len < LUA_QRCODE_PAYLOAD_CAPACITY ? code->payload_len : LUA_QRCODE_PAYLOAD_CAPACITY - 1;
    int right = code->rect.x + code->rect.w - 1;
    int bottom = code->rect.y + code->rect.h - 1;

    lua_createtable(L, 0, 17);
    lua_pushlstring(L, payload, payload_size);
    lua_setfield(L, -2, "payload");
    lua_pushinteger(L, code->payload_len);
    lua_setfield(L, -2, "payload_len");
    lua_pushinteger(L, code->version);
    lua_setfield(L, -2, "version");
    lua_pushinteger(L, code->ecc_level);
    lua_setfield(L, -2, "ecc_level");
    lua_pushinteger(L, code->mask);
    lua_setfield(L, -2, "mask");
    lua_pushinteger(L, code->data_type);
    lua_setfield(L, -2, "data_type");
    lua_pushinteger(L, code->eci);
    lua_setfield(L, -2, "eci");
    lua_pushinteger(L, code->rect.x);
    lua_setfield(L, -2, "left");
    lua_pushinteger(L, code->rect.y);
    lua_setfield(L, -2, "top");
    lua_pushinteger(L, right);
    lua_setfield(L, -2, "right");
    lua_pushinteger(L, bottom);
    lua_setfield(L, -2, "bottom");
    lua_pushinteger(L, code->rect.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, code->rect.y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, code->rect.w);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, code->rect.h);
    lua_setfield(L, -2, "height");

    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; i++) {
        lua_qrcode_push_point(L, &code->corners[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "corners");
}

static int lua_qrcode_detect(lua_State *L)
{
    lua_image_view_t view = {0};
    esp_vision_rect_t roi = {0};
    int opts_idx = lua_isnoneornil(L, 2) ? 0 : 2;
    size_t total = 0;

    lua_qrcode_scratch_t *scratch = lua_newuserdata(L, sizeof(*scratch));
    int scratch_idx = lua_gettop(L);
    memset(scratch, 0, sizeof(*scratch));
    for (size_t i = 0; i < LUA_QRCODE_RESULT_CAPACITY; i++) {
        scratch->codes[i].payload = scratch->payloads[i];
        scratch->codes[i].payload_size = sizeof(scratch->payloads[i]);
    }

    esp_err_t err = lua_image_require_format(L, 1, LUA_IMAGE_FORMAT_GRAY8, &view);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame conversion failed: %s", esp_err_to_name(err));
        return luaL_error(L, "qrcode_detect unsupported frame: %s", esp_err_to_name(err));
    }
    if (!lua_qrcode_parse_roi(L, opts_idx, view.width, view.height, &roi)) {
        ESP_LOGE(TAG, "invalid detection options");
        lua_image_release_view(&view);
        return luaL_error(L, "invalid qrcode_detect options");
    }
    err = lua_vision_core_lock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "core lock failed: %s", esp_err_to_name(err));
        lua_image_release_view(&view);
        return luaL_error(L, "qrcode_detect lock failed: %s", esp_err_to_name(err));
    }
    esp_vision_image_t image = {
        .width = view.width,
        .height = view.height,
        .pixformat = ESP_VISION_PIXFORMAT_GRAYSCALE,
        .data = (uint8_t *)view.data,
        .size = view.bytes,
    };
    err = esp_vision_image_find_qrcodes(&image, &roi, scratch->codes, LUA_QRCODE_RESULT_CAPACITY, &total);
    lua_vision_core_unlock();
    lua_image_release_view(&view);

    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        ESP_LOGE(TAG, "QR detection failed: %s", esp_err_to_name(err));
        return luaL_error(L, "qrcode_detect failed: %s", esp_err_to_name(err));
    }
    size_t returned = total < LUA_QRCODE_RESULT_CAPACITY ? total : LUA_QRCODE_RESULT_CAPACITY;
    bool truncated = err == ESP_ERR_INVALID_SIZE;
    lua_createtable(L, (int)returned, 3);
    for (size_t i = 0; i < returned; i++) {
        lua_qrcode_push_code(L, &scratch->codes[i], scratch->payloads[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
        if (scratch->codes[i].payload_len >= sizeof(scratch->payloads[i])) {
            truncated = true;
        }
    }
    lua_pushinteger(L, returned);
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, total);
    lua_setfield(L, -2, "total");
    lua_pushboolean(L, truncated);
    lua_setfield(L, -2, "truncated");
    lua_remove(L, scratch_idx);

    return 1;
}

int luaopen_qrcode_detect(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"detect", lua_qrcode_detect},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}
