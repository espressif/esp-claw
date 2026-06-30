/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_image.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua_image_resize.h"

#define LUA_MODULE_IMAGE_NAME "image"
#define LUA_IMAGE_FRAME_MT "image.frame"
#define LUA_IMAGE_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define LUA_IMAGE_MAX_PIXELS (1920U * 1080U)

static const char *TAG = "image";

typedef enum {
    LUA_IMAGE_FILE_FORMAT_UNKNOWN,
    LUA_IMAGE_FILE_FORMAT_JPEG,
} lua_image_file_format_t;

typedef struct {
    uint8_t *data;                         ///< Pixel or encoded data owned by this cached buffer entry.
    size_t bytes;                          ///< Buffer size in bytes.
    lua_image_frame_info_t info;           ///< Public metadata reported by frame:info().
    lua_image_frame_release_fn_t release_cb; ///< Optional owner-specific release hook for original producer buffers.
    void *release_ctx;                     ///< Opaque context passed to release_cb.
    bool valid;                            ///< True when this cache slot contains usable data.
    bool owns_data;                        ///< True when the store must release data through release_cb.
} lua_image_buffer_t;

typedef struct {
    int ref_count;                         ///< Number of Lua frame views currently holding this store.
    lua_image_buffer_t buffers[LUA_IMAGE_FORMAT_COUNT]; ///< Per-format cache entries for one logical image.
    bool closed;                           ///< Set once the store has started final teardown.
} lua_image_store_t;

typedef struct {
    lua_image_store_t *store;              ///< Shared backing store that owns original and converted buffers.
    lua_image_format_t format;             ///< Selected format view exposed by this Lua userdata.
    bool closed;                           ///< True after this view has released its store reference.
} lua_image_frame_ud_t;

static bool lua_image_format_from_string(const char *fmt, lua_image_format_t *out);

static void lua_image_free_owned_frame(void *ctx, const uint8_t *data)
{
    (void)ctx;
    free((void *)data);
}

static lua_image_buffer_t *lua_image_store_get_buffer(lua_image_store_t *store, lua_image_format_t format)
{
    if (store == NULL || format < LUA_IMAGE_FORMAT_RGB565LE || format >= LUA_IMAGE_FORMAT_COUNT) {
        return NULL;
    }
    return &store->buffers[format];
}

static const lua_image_buffer_t *lua_image_frame_get_buffer(const lua_image_frame_ud_t *frame)
{
    lua_image_buffer_t *buffer = NULL;

    if (frame == NULL || frame->closed || frame->store == NULL || frame->store->closed) {
        return NULL;
    }
    buffer = lua_image_store_get_buffer(frame->store, frame->format);
    if (buffer == NULL || !buffer->valid || buffer->data == NULL) {
        return NULL;
    }
    return buffer;
}

static void lua_image_store_release_buffer(lua_image_buffer_t *buffer)
{
    if (buffer == NULL || !buffer->valid) {
        return;
    }
    if (buffer->owns_data && buffer->release_cb != NULL && buffer->data != NULL) {
        buffer->release_cb(buffer->release_ctx, buffer->data);
    }
    memset(buffer, 0, sizeof(*buffer));
}

static void lua_image_store_unref(lua_image_store_t *store)
{
    if (store == NULL) {
        return;
    }
    store->ref_count--;
    if (store->ref_count > 0) {
        return;
    }
    if (store->ref_count < 0) {
        ESP_LOGE(TAG, "image store refcount underflow");
    }
    store->closed = true;
    for (int i = 0; i < LUA_IMAGE_FORMAT_COUNT; i++) {
        lua_image_store_release_buffer(&store->buffers[i]);
    }
    free(store);
}

static void lua_image_store_ref(lua_image_store_t *store)
{
    if (store == NULL) {
        return;
    }
    store->ref_count++;
}

static bool lua_image_has_suffix_ci(const char *path, const char *suffix)
{
    size_t path_len;
    size_t suffix_len;

    if (path == NULL || suffix == NULL) {
        return false;
    }
    path_len = strlen(path);
    suffix_len = strlen(suffix);
    if (path_len < suffix_len) {
        return false;
    }
    path += path_len - suffix_len;
    for (size_t i = 0; i < suffix_len; i++) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

static lua_image_file_format_t lua_image_guess_file_format(const char *path)
{
    if (lua_image_has_suffix_ci(path, ".jpg") || lua_image_has_suffix_ci(path, ".jpeg")) {
        return LUA_IMAGE_FILE_FORMAT_JPEG;
    }
    return LUA_IMAGE_FILE_FORMAT_UNKNOWN;
}

static bool lua_image_check_format_value(lua_Integer value, lua_image_format_t *out)
{
    if (value < LUA_IMAGE_FORMAT_RGB565LE || value >= LUA_IMAGE_FORMAT_COUNT) {
        return false;
    }
    *out = (lua_image_format_t)value;
    return true;
}

static const char *lua_image_format_fourcc(lua_image_format_t format)
{
    switch (format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
        return "RGBP";
    case LUA_IMAGE_FORMAT_RGB565BE:
        return "RGBR";
    case LUA_IMAGE_FORMAT_RGB888:
        return "RGB3";
    case LUA_IMAGE_FORMAT_BGR888:
        return "BGR3";
    case LUA_IMAGE_FORMAT_GRAY8:
        return "GREY";
    case LUA_IMAGE_FORMAT_YUYV:
        return "YUYV";
    case LUA_IMAGE_FORMAT_UYVY:
        return "UYVY";
    case LUA_IMAGE_FORMAT_JPEG:
        return "JPEG";
    case LUA_IMAGE_FORMAT_MJPEG:
        return "MJPG";
    default:
        return NULL;
    }
}

static void lua_image_set_format_constant(lua_State *L, const char *name, lua_image_format_t format)
{
    lua_pushinteger(L, (lua_Integer)format);
    lua_setfield(L, -2, name);
}

static void lua_image_set_format_constants(lua_State *L)
{
    lua_image_set_format_constant(L, "RGB565", LUA_IMAGE_FORMAT_RGB565LE);
    lua_image_set_format_constant(L, "RGB565_BE", LUA_IMAGE_FORMAT_RGB565BE);
    lua_image_set_format_constant(L, "RGB888", LUA_IMAGE_FORMAT_RGB888);
    lua_image_set_format_constant(L, "BGR888", LUA_IMAGE_FORMAT_BGR888);
    lua_image_set_format_constant(L, "GRAY8", LUA_IMAGE_FORMAT_GRAY8);
    lua_image_set_format_constant(L, "YUYV", LUA_IMAGE_FORMAT_YUYV);
    lua_image_set_format_constant(L, "UYVY", LUA_IMAGE_FORMAT_UYVY);
    lua_image_set_format_constant(L, "JPEG", LUA_IMAGE_FORMAT_JPEG);
    lua_image_set_format_constant(L, "MJPEG", LUA_IMAGE_FORMAT_MJPEG);
}

/* ------------------------------------------------------------------------- */
/* image.frame userdata                                             */
/* ------------------------------------------------------------------------- */

static lua_image_frame_ud_t *lua_image_check_frame(lua_State *L, int index)
{
    return (lua_image_frame_ud_t *)luaL_checkudata(L, index, LUA_IMAGE_FRAME_MT);
}

static void lua_image_frame_do_release(lua_image_frame_ud_t *frame)
{
    if (frame == NULL || frame->closed) {
        return;
    }
    frame->closed = true;
    if (frame->store != NULL) {
        lua_image_store_unref(frame->store);
        frame->store = NULL;
    }
}

static void lua_image_push_frame_info(lua_State *L, const lua_image_frame_ud_t *frame)
{
    const lua_image_buffer_t *buffer = lua_image_frame_get_buffer(frame);

    if (buffer == NULL) {
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "bytes");
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "width");
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "height");
        lua_pushstring(L, "");
        lua_setfield(L, -2, "pixel_format");
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "timestamp_us");
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "valid");
        return;
    }

    lua_pushinteger(L, (lua_Integer)buffer->bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, buffer->info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, buffer->info.height);
    lua_setfield(L, -2, "height");
    lua_pushstring(L, buffer->info.pixel_format);
    lua_setfield(L, -2, "pixel_format");
    lua_pushinteger(L, (lua_Integer)buffer->info.timestamp_us);
    lua_setfield(L, -2, "timestamp_us");
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "valid");
}

static int lua_image_frame_method_data(lua_State *L)
{
    lua_image_frame_ud_t *frame = lua_image_check_frame(L, 1);
    const lua_image_buffer_t *buffer = lua_image_frame_get_buffer(frame);

    if (buffer == NULL) {
        return luaL_error(L, "image.frame is already released");
    }
    lua_pushlstring(L, (const char *)buffer->data, buffer->bytes);
    return 1;
}

static int lua_image_frame_method_info(lua_State *L)
{
    lua_image_frame_ud_t *frame = lua_image_check_frame(L, 1);

    lua_newtable(L);
    lua_image_push_frame_info(L, frame);
    return 1;
}

static int lua_image_frame_method_release(lua_State *L)
{
    lua_image_frame_ud_t *frame = lua_image_check_frame(L, 1);
    lua_image_frame_do_release(frame);
    return 0;
}

static int lua_image_frame_gc(lua_State *L)
{
    lua_image_frame_ud_t *frame = lua_image_check_frame(L, 1);
    lua_image_frame_do_release(frame);
    return 0;
}

/* Lua 5.4+ to-be-closed hook. */
static int lua_image_frame_close(lua_State *L)
{
    lua_image_frame_ud_t *frame = lua_image_check_frame(L, 1);
    lua_image_frame_do_release(frame);
    return 0;
}

static int lua_image_frame_tostring(lua_State *L)
{
    lua_image_frame_ud_t *frame = lua_image_check_frame(L, 1);
    const lua_image_buffer_t *buffer = lua_image_frame_get_buffer(frame);

    lua_pushfstring(L,
                    "image.frame(%s, bytes=%d, %dx%d, format=%s)",
                    buffer == NULL ? "released" : "valid",
                    buffer == NULL ? 0 : (int)buffer->bytes,
                    buffer == NULL ? 0 : buffer->info.width,
                    buffer == NULL ? 0 : buffer->info.height,
                    buffer == NULL ? "" : buffer->info.pixel_format);
    return 1;
}

static void lua_image_create_frame_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_IMAGE_FRAME_MT) == 0) {
        lua_pop(L, 1);
        return;
    }

    lua_newtable(L);
    lua_pushcfunction(L, lua_image_frame_method_data);
    lua_setfield(L, -2, "data");
    lua_pushcfunction(L, lua_image_frame_method_info);
    lua_setfield(L, -2, "info");
    lua_pushcfunction(L, lua_image_frame_method_release);
    lua_setfield(L, -2, "release");
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lua_image_frame_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, lua_image_frame_close);
    lua_setfield(L, -2, "__close");
    lua_pushcfunction(L, lua_image_frame_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

esp_err_t lua_image_push_frame(lua_State *L,
                               const uint8_t *data,
                               size_t bytes,
                               const lua_image_frame_info_t *info,
                               lua_image_frame_release_fn_t release_cb,
                               void *release_ctx)
{
    lua_image_frame_ud_t *frame;
    lua_image_store_t *store = NULL;
    lua_image_buffer_t *buffer = NULL;
    lua_image_format_t format;

    if (L == NULL || data == NULL || info == NULL || release_cb == NULL) {
        ESP_LOGE(TAG, "invalid image frame push args");
        return ESP_ERR_INVALID_ARG;
    }
    if (!lua_image_format_from_string(info->pixel_format, &format)) {
        ESP_LOGE(TAG, "push frame unsupported pixel format: %s", info->pixel_format[0] ? info->pixel_format : "(empty)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    store = (lua_image_store_t *)calloc(1, sizeof(*store));
    if (store == NULL) {
        ESP_LOGE(TAG, "image store alloc failed");
        return ESP_ERR_NO_MEM;
    }
    store->ref_count = 1;
    buffer = lua_image_store_get_buffer(store, format);
    if (buffer == NULL) {
        free(store);
        ESP_LOGE(TAG, "invalid frame format index: %d", (int)format);
        return ESP_ERR_INVALID_ARG;
    }
    buffer->data = (uint8_t *)data;
    buffer->bytes = bytes;
    buffer->info = *info;
    buffer->info.bytes = bytes;
    buffer->release_cb = release_cb;
    buffer->release_ctx = release_ctx;
    buffer->valid = true;
    buffer->owns_data = true;

    /* Make sure the frame metatable exists even if luaopen_image()
     * has not been called yet (e.g. producer registered before module). */
    lua_image_create_frame_metatable(L);

    frame = (lua_image_frame_ud_t *)lua_newuserdata(L, sizeof(*frame));
    memset(frame, 0, sizeof(*frame));
    frame->store = store;
    frame->format = format;
    frame->closed = false;
    luaL_getmetatable(L, LUA_IMAGE_FRAME_MT);
    lua_setmetatable(L, -2);
    return ESP_OK;
}

static int lua_image_push_frame_view(lua_State *L, lua_image_store_t *store, lua_image_format_t format)
{
    lua_image_frame_ud_t *frame;
    lua_image_buffer_t *buffer = lua_image_store_get_buffer(store, format);

    if (L == NULL || store == NULL || store->closed || buffer == NULL || !buffer->valid || buffer->data == NULL) {
        ESP_LOGE(TAG, "push frame view invalid: format=%d", (int)format);
        return luaL_error(L, "image frame view unavailable");
    }

    lua_image_create_frame_metatable(L);
    lua_image_store_ref(store);
    frame = (lua_image_frame_ud_t *)lua_newuserdata(L, sizeof(*frame));
    memset(frame, 0, sizeof(*frame));
    frame->store = store;
    frame->format = format;
    frame->closed = false;
    luaL_getmetatable(L, LUA_IMAGE_FRAME_MT);
    lua_setmetatable(L, -2);
    return 1;
}

esp_err_t lua_image_borrow_frame(lua_State *L, int index, lua_image_frame_borrow_t *out)
{
    lua_image_frame_ud_t *frame;
    const lua_image_buffer_t *buffer;

    if (L == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    frame = (lua_image_frame_ud_t *)luaL_testudata(L, index, LUA_IMAGE_FRAME_MT);
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer = lua_image_frame_get_buffer(frame);
    if (buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    out->data = buffer->data;
    out->bytes = buffer->bytes;
    out->info = buffer->info;
    return ESP_OK;
}

static bool lua_image_format_from_string(const char *fmt, lua_image_format_t *out)
{
    if (fmt == NULL || out == NULL) {
        return false;
    }
    if (strcmp(fmt, "RGBP") == 0) {
        *out = LUA_IMAGE_FORMAT_RGB565LE;
    } else if (strcmp(fmt, "RGBR") == 0) {
        *out = LUA_IMAGE_FORMAT_RGB565BE;
    } else if (strcmp(fmt, "RGB3") == 0) {
        *out = LUA_IMAGE_FORMAT_RGB888;
    } else if (strcmp(fmt, "BGR3") == 0) {
        *out = LUA_IMAGE_FORMAT_BGR888;
    } else if (strcmp(fmt, "GREY") == 0 || strcmp(fmt, "Y800") == 0) {
        *out = LUA_IMAGE_FORMAT_GRAY8;
    } else if (strcmp(fmt, "YUYV") == 0) {
        *out = LUA_IMAGE_FORMAT_YUYV;
    } else if (strcmp(fmt, "UYVY") == 0) {
        *out = LUA_IMAGE_FORMAT_UYVY;
    } else if (strcmp(fmt, "JPEG") == 0) {
        *out = LUA_IMAGE_FORMAT_JPEG;
    } else if (strcmp(fmt, "MJPG") == 0) {
        *out = LUA_IMAGE_FORMAT_MJPEG;
    } else {
        return false;
    }
    return true;
}

static esp_err_t lua_image_source_from_buffer(const lua_image_buffer_t *buffer, lua_image_format_t format, lua_image_source_t *out)
{
    if (buffer == NULL || out == NULL || !buffer->valid || buffer->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->data = buffer->data;
    out->bytes = buffer->bytes;
    out->width = buffer->info.width;
    out->height = buffer->info.height;
    out->format = format;
    strlcpy(out->source_format, buffer->info.pixel_format, sizeof(out->source_format));
    return ESP_OK;
}

static lua_image_buffer_t *lua_image_store_find_source(lua_image_store_t *store, lua_image_format_t preferred, lua_image_format_t *out_format)
{
    lua_image_buffer_t *buffer = lua_image_store_get_buffer(store, preferred);

    if (buffer != NULL && buffer->valid && buffer->data != NULL) {
        if (out_format) {
            *out_format = preferred;
        }
        return buffer;
    }
    for (int i = 0; i < LUA_IMAGE_FORMAT_COUNT; i++) {
        buffer = &store->buffers[i];
        if (buffer->valid && buffer->data != NULL) {
            if (out_format) {
                *out_format = (lua_image_format_t)i;
            }
            return buffer;
        }
    }
    return NULL;
}

static esp_err_t lua_image_store_commit_view(lua_image_store_t *store, lua_image_format_t format, lua_image_view_t *view, int64_t timestamp_us)
{
    lua_image_buffer_t *buffer = lua_image_store_get_buffer(store, format);
    const char *fourcc = lua_image_format_fourcc(format);

    if (buffer == NULL || view == NULL || view->data == NULL || fourcc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buffer->valid) {
        return ESP_OK;
    }

    buffer->data = (uint8_t *)view->data;
    buffer->bytes = view->bytes;
    buffer->info.width = view->width;
    buffer->info.height = view->height;
    buffer->info.bytes = view->bytes;
    buffer->info.timestamp_us = timestamp_us;
    strlcpy(buffer->info.pixel_format, fourcc, sizeof(buffer->info.pixel_format));
    buffer->valid = true;
    buffer->owns_data = view->owned;
    buffer->release_cb = view->owned ? lua_image_free_owned_frame : NULL;
    buffer->release_ctx = NULL;

    /* Ownership or alias lifetime moves into the image store. */
    view->data = NULL;
    view->bytes = 0;
    view->owned = false;
    return ESP_OK;
}

static esp_err_t lua_image_store_convert_from(lua_image_store_t *store,
                                              lua_image_format_t source_format,
                                              lua_image_format_t target_format,
                                              lua_image_view_t *out)
{
    lua_image_source_t src = {0};
    lua_image_buffer_t *source = lua_image_store_get_buffer(store, source_format);
    esp_err_t err = lua_image_source_from_buffer(source, source_format, &src);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image source unavailable for conversion: %s", lua_image_format_name(source_format));
        return err;
    }
    err = lua_image_convert_view(&src, target_format, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image store conversion failed: %s -> %s: %s",
                 lua_image_format_name(source_format), lua_image_format_name(target_format), esp_err_to_name(err));
    }
    return err;
}

static esp_err_t lua_image_store_require_format(lua_image_store_t *store, lua_image_format_t format)
{
    lua_image_buffer_t *target = lua_image_store_get_buffer(store, format);
    lua_image_buffer_t *source = NULL;
    lua_image_format_t source_format = LUA_IMAGE_FORMAT_RGB565LE;
    lua_image_view_t view = {0};
    int64_t timestamp_us = 0;
    esp_err_t err;

    if (store == NULL || store->closed || target == NULL) {
        ESP_LOGE(TAG, "image store unavailable for format: %s", lua_image_format_name(format));
        return ESP_ERR_INVALID_STATE;
    }
    if (target->valid && target->data != NULL) {
        return ESP_OK;
    }

    if (format == LUA_IMAGE_FORMAT_GRAY8 &&
        lua_image_store_get_buffer(store, LUA_IMAGE_FORMAT_RGB565LE)->valid) {
        source_format = LUA_IMAGE_FORMAT_RGB565LE;
        goto convert;
    }
    if ((format == LUA_IMAGE_FORMAT_GRAY8 || format == LUA_IMAGE_FORMAT_JPEG) &&
        lua_image_store_get_buffer(store, LUA_IMAGE_FORMAT_JPEG)->valid) {
        err = lua_image_store_require_format(store, LUA_IMAGE_FORMAT_RGB565LE);
        if (err != ESP_OK) {
            return err;
        }
        source_format = LUA_IMAGE_FORMAT_RGB565LE;
        goto convert;
    }
    if (format == LUA_IMAGE_FORMAT_MJPEG && lua_image_store_get_buffer(store, LUA_IMAGE_FORMAT_JPEG)->valid) {
        source_format = LUA_IMAGE_FORMAT_JPEG;
        goto convert;
    }

    source = lua_image_store_find_source(store, LUA_IMAGE_FORMAT_JPEG, &source_format);
    if (source == NULL) {
        ESP_LOGE(TAG, "image store has no source buffer for format: %s", lua_image_format_name(format));
        return ESP_ERR_INVALID_STATE;
    }

convert:
    source = lua_image_store_get_buffer(store, source_format);
    timestamp_us = source != NULL ? source->info.timestamp_us : 0;
    err = lua_image_store_convert_from(store, source_format, format, &view);
    if (err != ESP_OK) {
        lua_image_release_view(&view);
        return err;
    }
    err = lua_image_store_commit_view(store, format, &view, timestamp_us);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image store cache commit failed: %s", esp_err_to_name(err));
        lua_image_release_view(&view);
    }
    return err;
}

esp_err_t lua_image_require_format(lua_State *L, int frame_index, lua_image_format_t format, lua_image_view_t *out)
{
    lua_image_frame_ud_t *frame;
    lua_image_buffer_t *buffer;
    esp_err_t err;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    frame = (lua_image_frame_ud_t *)luaL_testudata(L, frame_index, LUA_IMAGE_FRAME_MT);
    if (frame == NULL) {
        ESP_LOGE(TAG, "image.frame expected");
        return ESP_ERR_INVALID_ARG;
    }
    if (frame->closed || frame->store == NULL || frame->store->closed) {
        ESP_LOGE(TAG, "image.frame unavailable when requiring format: %s", lua_image_format_name(format));
        return ESP_ERR_INVALID_STATE;
    }

    err = lua_image_store_require_format(frame->store, format);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "require image format failed: %s: %s", lua_image_format_name(format), esp_err_to_name(err));
        return err;
    }
    buffer = lua_image_store_get_buffer(frame->store, format);
    if (buffer == NULL || !buffer->valid || buffer->data == NULL) {
        ESP_LOGE(TAG, "required image format missing after conversion: %s", lua_image_format_name(format));
        return ESP_ERR_INVALID_STATE;
    }
    out->data = buffer->data;
    out->bytes = buffer->bytes;
    out->width = buffer->info.width;
    out->height = buffer->info.height;
    out->format = format;
    out->owned = false;
    strlcpy(out->source_format, buffer->info.pixel_format, sizeof(out->source_format));
    return ESP_OK;
}

/* image.convert(frame, format)
 * Ensures the requested format exists in the shared store and returns a new frame view. */
static int lua_module_image_convert(lua_State *L)
{
    lua_Integer format_value = luaL_checkinteger(L, 2);
    lua_image_format_t format;
    lua_image_frame_ud_t *source = lua_image_check_frame(L, 1);
    esp_err_t err;

    if (!lua_image_check_format_value(format_value, &format)) {
        return luaL_error(L, "image.convert invalid format constant: %d", (int)format_value);
    }
    if (source->closed || source->store == NULL || source->store->closed) {
        ESP_LOGE(TAG, "image.convert source frame is released");
        return luaL_error(L, "image frame is released");
    }

    err = lua_image_store_require_format(source->store, format);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image convert failed: %s: %s", lua_image_format_name(format), esp_err_to_name(err));
        return luaL_error(L, "image.convert failed: %s", esp_err_to_name(err));
    }
    return lua_image_push_frame_view(L, source->store, format);
}

/* Pick the resize output format. Caller may pin it through opts.format;
 * otherwise we mirror the source frame's logical channels (GRAY8 stays
 * gray, everything else lifts to RGB565LE). */
static lua_image_format_t lua_image_resize_pick_output(lua_image_format_t source_format,
                                                       bool format_explicit,
                                                       lua_image_format_t requested)
{
    if (format_explicit) {
        return requested;
    }
    return source_format == LUA_IMAGE_FORMAT_GRAY8 ? LUA_IMAGE_FORMAT_GRAY8 : LUA_IMAGE_FORMAT_RGB565LE;
}

static bool lua_image_resize_parse_filter(const char *name, lua_image_resize_filter_t *out)
{
    if (name == NULL || strcmp(name, "nearest") == 0) {
        *out = LUA_IMAGE_RESIZE_FILTER_NEAREST;
        return true;
    }
    if (strcmp(name, "bilinear") == 0) {
        *out = LUA_IMAGE_RESIZE_FILTER_BILINEAR;
        return true;
    }
    return false;
}

/* image.resize(frame, opts)
 * opts = { width, height, [format = image.RGB565|image.GRAY8], [filter = "nearest"|"bilinear"] }
 * Returns a new image.frame backed by an independent store; the source frame
 * and its cache are untouched (apart from the intermediate RGB565/GRAY8
 * conversion that may have been needed to read the pixels). */
static int lua_module_image_resize(lua_State *L)
{
    lua_image_frame_ud_t *source = lua_image_check_frame(L, 1);
    int dst_width;
    int dst_height;
    lua_image_format_t requested_format = LUA_IMAGE_FORMAT_RGB565LE;
    bool format_explicit = false;
    const char *filter_name = NULL;
    lua_image_resize_filter_t filter;
    lua_image_format_t source_format;
    lua_image_format_t output_format;
    const lua_image_buffer_t *intermediate;
    lua_image_source_t intermediate_src;
    lua_image_view_t intermediate_view;
    lua_image_view_t output_view = {0};
    lua_image_frame_info_t info = {0};
    const char *fourcc;
    esp_err_t err;

    luaL_checktype(L, 2, LUA_TTABLE);

    if (source->closed || source->store == NULL || source->store->closed) {
        return luaL_error(L, "image.resize source frame is released");
    }
    source_format = source->format;

    lua_getfield(L, 2, "width");
    dst_width = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "height");
    dst_height = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (dst_width <= 0 || dst_height <= 0) {
        return luaL_error(L, "image.resize width/height must be positive");
    }

    lua_getfield(L, 2, "format");
    if (!lua_isnil(L, -1)) {
        lua_Integer fv = luaL_checkinteger(L, -1);
        if (!lua_image_check_format_value(fv, &requested_format)) {
            lua_pop(L, 1);
            return luaL_error(L, "image.resize invalid format constant: %d", (int)fv);
        }
        format_explicit = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "filter");
    if (!lua_isnil(L, -1)) {
        filter_name = luaL_checkstring(L, -1);
    }
    if (!lua_image_resize_parse_filter(filter_name, &filter)) {
        lua_pop(L, 1);
        return luaL_error(L, "image.resize unsupported filter: %s", filter_name);
    }
    lua_pop(L, 1);

    output_format = lua_image_resize_pick_output(source_format, format_explicit, requested_format);
    if (output_format != LUA_IMAGE_FORMAT_RGB565LE && output_format != LUA_IMAGE_FORMAT_GRAY8) {
        return luaL_error(L, "image.resize output format must be RGB565 or GRAY8, got %s",
                          lua_image_format_name(output_format));
    }

    /* Lift the source into the canonical intermediate format, caching it
     * inside the source store so future calls (including convert / resize)
     * skip the decode step. */
    err = lua_image_store_require_format(source->store, output_format);
    if (err != ESP_OK) {
        return luaL_error(L, "image.resize source conversion failed: %s", esp_err_to_name(err));
    }
    intermediate = lua_image_store_get_buffer(source->store, output_format);
    if (intermediate == NULL || !intermediate->valid || intermediate->data == NULL) {
        return luaL_error(L, "image.resize intermediate buffer missing after conversion");
    }
    err = lua_image_source_from_buffer(intermediate, output_format, &intermediate_src);
    if (err != ESP_OK) {
        return luaL_error(L, "image.resize intermediate buffer borrow failed");
    }
    intermediate_view.data = intermediate_src.data;
    intermediate_view.bytes = intermediate_src.bytes;
    intermediate_view.width = intermediate_src.width;
    intermediate_view.height = intermediate_src.height;
    intermediate_view.format = output_format;
    intermediate_view.owned = false;
    strlcpy(intermediate_view.source_format, intermediate_src.source_format, sizeof(intermediate_view.source_format));

    err = lua_image_resize_view(&intermediate_view, dst_width, dst_height, filter, &output_view);
    if (err != ESP_OK) {
        return luaL_error(L, "image.resize failed: %s", esp_err_to_name(err));
    }

    fourcc = lua_image_format_fourcc(output_format);
    if (fourcc == NULL) {
        lua_image_release_view(&output_view);
        return luaL_error(L, "image.resize internal format error");
    }
    info.width = output_view.width;
    info.height = output_view.height;
    info.bytes = output_view.bytes;
    info.timestamp_us = intermediate->info.timestamp_us;
    strlcpy(info.pixel_format, fourcc, sizeof(info.pixel_format));

    err = lua_image_push_frame(L, output_view.data, output_view.bytes, &info, lua_image_free_owned_frame, NULL);
    if (err != ESP_OK) {
        lua_image_release_view(&output_view);
        return luaL_error(L, "image.resize push frame failed: %s", esp_err_to_name(err));
    }
    /* Ownership transferred to the new frame's store; suppress release_view free. */
    output_view.data = NULL;
    output_view.bytes = 0;
    output_view.owned = false;
    return 1;
}

/* image.load_file(path)
 * Loads an image file into an owned image.frame. The frame owns the file buffer
 * and releases it when frame:release(), <close>, or __gc runs. */
static int lua_module_image_load_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_image_file_format_t format = lua_image_guess_file_format(path);
    FILE *file = NULL;
    long file_size = 0;
    uint8_t *data = NULL;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = NULL;
    jpeg_dec_io_t io = {0};
    jpeg_dec_header_info_t header = {0};
    jpeg_error_t jpeg_err;
    lua_image_frame_info_t info = {0};
    esp_err_t err;

    if (format != LUA_IMAGE_FILE_FORMAT_JPEG) {
        return luaL_error(L, "image.load_file unsupported image file suffix: %s", path);
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return luaL_error(L, "image.load_file failed to open %s: errno=%d", path, errno);
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return luaL_error(L, "image.load_file seek failed for %s", path);
    }
    file_size = ftell(file);
    if (file_size <= 0 || file_size > INT_MAX || (unsigned long)file_size > LUA_IMAGE_MAX_FILE_BYTES) {
        ESP_LOGE(TAG, "image.load_file invalid file size: %ld bytes (limit=%u)", file_size, (unsigned)LUA_IMAGE_MAX_FILE_BYTES);
        fclose(file);
        return luaL_error(L, "image.load_file invalid file size for %s", path);
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return luaL_error(L, "image.load_file seek failed for %s", path);
    }

    data = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_8BIT|MALLOC_CAP_SPIRAM);
    if (data == NULL) {
        ESP_LOGE(TAG, "image.load_file file buffer alloc failed: %u bytes", (unsigned)file_size);
        fclose(file);
        return luaL_error(L, "image.load_file no memory");
    }
    if (fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(data);
        fclose(file);
        return luaL_error(L, "image.load_file read failed for %s", path);
    }
    fclose(file);

    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_err = jpeg_dec_open(&config, &decoder);
    if (jpeg_err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed while loading file: %d", jpeg_err);
        free(data);
        return luaL_error(L, "image.load_file decoder open failed");
    }
    io.inbuf = data;
    io.inbuf_len = (int)file_size;
    jpeg_err = jpeg_dec_parse_header(decoder, &io, &header);
    jpeg_dec_close(decoder);
    if (jpeg_err != JPEG_ERR_OK || header.width == 0 || header.height == 0) {
        ESP_LOGE(TAG, "image.load_file invalid JPEG header: err=%d size=%ux%u", jpeg_err, (unsigned)header.width, (unsigned)header.height);
        free(data);
        return luaL_error(L, "image.load_file invalid JPEG file: %s", path);
    }
    if ((size_t)header.width > SIZE_MAX / (size_t)header.height ||
        (size_t)header.width * (size_t)header.height > LUA_IMAGE_MAX_PIXELS) {
        ESP_LOGE(TAG, "image.load_file JPEG exceeds pixel limit: %ux%u (limit=%u)",
                 (unsigned)header.width, (unsigned)header.height, (unsigned)LUA_IMAGE_MAX_PIXELS);
        free(data);
        return luaL_error(L, "image.load_file invalid JPEG file: %s", path);
    }

    info.width = (int)header.width;
    info.height = (int)header.height;
    info.bytes = (size_t)file_size;
    info.timestamp_us = 0;
    strlcpy(info.pixel_format, "JPEG", sizeof(info.pixel_format));

    err = lua_image_push_frame(L, data, (size_t)file_size, &info, lua_image_free_owned_frame, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "push JPEG file frame failed: %s", esp_err_to_name(err));
        free(data);
        return luaL_error(L, "image.load_file failed: %s", esp_err_to_name(err));
    }
    return 1;
}

/* image.save_file(path, frame)
 * Saves an image.frame using the format implied by the file suffix. */
static int lua_module_image_save_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_image_file_format_t format = lua_image_guess_file_format(path);
    lua_image_view_t view = {0};
    FILE *file = NULL;
    esp_err_t err;

    if (format != LUA_IMAGE_FILE_FORMAT_JPEG) {
        return luaL_error(L, "image.save_file unsupported image file suffix: %s", path);
    }

    err = lua_image_require_format(L, 2, LUA_IMAGE_FORMAT_JPEG, &view);
    if (err != ESP_OK) {
        return luaL_error(L, "image.save_file failed to encode JPEG: %s", esp_err_to_name(err));
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        lua_image_release_view(&view);
        return luaL_error(L, "image.save_file failed to open %s: errno=%d", path, errno);
    }
    if (fwrite(view.data, 1, view.bytes, file) != view.bytes) {
        fclose(file);
        lua_image_release_view(&view);
        return luaL_error(L, "image.save_file write failed for %s", path);
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "close JPEG output file failed: %s errno=%d", path, errno);
        lua_image_release_view(&view);
        return luaL_error(L, "image.save_file close failed for %s", path);
    }

    lua_image_release_view(&view);
    return 0;
}

int luaopen_image(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"convert", lua_module_image_convert},
        {"resize", lua_module_image_resize},
        {"load_file", lua_module_image_load_file},
        {"save_file", lua_module_image_save_file},
        {NULL, NULL},
    };

    lua_image_create_frame_metatable(L);
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_image_set_format_constants(L);
    return 1;
}

esp_err_t lua_module_image_register(void)
{
    return cap_lua_register_module(LUA_MODULE_IMAGE_NAME, luaopen_image);
}
