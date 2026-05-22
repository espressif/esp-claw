/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_camera.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_log.h"
#include "lua_image.h"
#include "lauxlib.h"
#include "lua.h"
#include "camera_hal.h"

#define LUA_MODULE_CAMERA_NAME "camera"

static const char *TAG = "lua_camera";

/* Camera-side release hook. The service can hold multiple borrowed buffers,
 * each identified by its data pointer; ctx is unused. */
static void lua_module_camera_frame_release_cb(void *ctx, const uint8_t *data)
{
    (void)ctx;
    if (data != NULL) {
        esp_err_t err = camera_release_frame((void *)data);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to release camera frame: %s", esp_err_to_name(err));
        }
    }
}

/* Pack a 4-char FOURCC string into the V4L2 uint32 code. Rejects anything that
 * is not exactly four printable bytes. */
static int lua_module_camera_parse_fourcc(lua_State *L, const char *fourcc, uint32_t *out)
{
    size_t len = (fourcc != NULL) ? strlen(fourcc) : 0;
    if (len != 4) {
        return luaL_error(L, "pixel format must be a 4-char FOURCC string (got %d chars)", (int)len);
    }
    *out = ((uint32_t)(uint8_t)fourcc[0])
         | ((uint32_t)(uint8_t)fourcc[1] << 8)
         | ((uint32_t)(uint8_t)fourcc[2] << 16)
         | ((uint32_t)(uint8_t)fourcc[3] << 24);
    return 0;
}

static int lua_module_camera_optional_uint_field(lua_State *L, int table_idx, const char *name, uint32_t *out)
{
    lua_getfield(L, table_idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_isinteger(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "opts.%s must be an integer", name);
    }
    lua_Integer value = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (value < 0 || value > UINT32_MAX) {
        return luaL_error(L, "opts.%s out of range", name);
    }
    *out = (uint32_t)value;
    return 0;
}

static int lua_module_camera_optional_bool_field(lua_State *L, int table_idx, const char *name, bool *out)
{
    lua_getfield(L, table_idx, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_isboolean(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "opts.%s must be a boolean", name);
    }
    *out = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return 0;
}

/* Camera must already be open when this is called. Walks the sensor's
 * advertised formats and returns the FOURCC of the highest-priority entry in
 * @p preferred that the sensor exposes. ESP_ERR_NOT_FOUND when no preference
 * matches. */
static esp_err_t lua_module_camera_resolve_fourcc(const uint32_t *preferred, int n_preferred,
                                                  uint32_t *out_fourcc)
{
    for (int p = 0; p < n_preferred; p++) {
        for (uint32_t i = 0; ; i++) {
            camera_format_desc_t desc = {0};
            esp_err_t err = camera_enum_format(i, &desc);
            if (err == ESP_ERR_NOT_FOUND) {
                break;
            }
            if (err != ESP_OK) {
                return err;
            }
            if (desc.pixel_format == preferred[p]) {
                *out_fourcc = desc.pixel_format;
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Walk VIDIOC_ENUM_FRAMESIZES for @p fourcc and return the first discrete
 * size. ESP_ERR_NOT_FOUND when the driver does not enumerate discrete sizes
 * for this format (only stepwise/continuous, or no entries at all). */
static esp_err_t lua_module_camera_enum_first_discrete_size(uint32_t fourcc,
                                                            uint32_t *out_width, uint32_t *out_height)
{
    for (uint32_t j = 0; ; j++) {
        camera_frame_size_t fsz = {0};
        esp_err_t err = camera_enum_frame_size(fourcc, j, &fsz);
        if (err == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (err != ESP_OK) {
            return err;
        }
        if (fsz.type == CAMERA_FRAME_SIZE_DISCRETE) {
            *out_width = fsz.width;
            *out_height = fsz.height;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* Build a human-readable list of all advertised FOURCC strings; used to make
 * the "no preference matched" error actionable. Returns the number of chars
 * written (excluding NUL). */
static int lua_module_camera_describe_available(char *out, size_t out_size)
{
    camera_format_desc_t desc = {0};
    size_t written = 0;

    out[0] = '\0';
    for (uint32_t i = 0; ; i++) {
        esp_err_t err = camera_enum_format(i, &desc);
        if (err != ESP_OK) {
            break;
        }
        int n = snprintf(out + written, out_size - written, "%s%s",
                         written == 0 ? "" : ",", desc.pixel_format_str);
        if (n < 0 || (size_t)n >= out_size - written) {
            break;
        }
        written += (size_t)n;
    }
    return (int)written;
}

/* camera.open(dev_path [, opts])
 * opts (optional table): { width, height, format, nearest }
 *   - width/height: requested capture resolution (integer; driver may adjust)
 *   - format: array of FOURCC strings in priority order, e.g.
 *       { "JPEG", "RGBP", "YUYV" }. The driver is probed first and the
 *       highest-priority entry the sensor actually advertises wins; the
 *       camera is then (re)opened with that format. A single-element array
 *       like { "RGBP" } gives the strict "use exactly this format" behaviour
 *       with a helpful error listing the sensor's actual formats on miss.
 *   - nearest: snap requested width/height to the closest supported size for
 *     the chosen format. When false (default) the exact size is sent to the
 *     driver and an unsupported request fails.
 *
 * Negotiation runs in one pass:
 *   1. probe-open (no opts) so we can enumerate
 *   2. pick fourcc — preferred list (if given) or current probe fourcc
 *   3. pick (w, h) — user size snapped via camera_find_closest_size when
 *      nearest=true, user size verbatim when nearest=false, or the first
 *      enumerated discrete size when no size was given alongside a format
 *   4. close + reopen with the exact (fourcc, w, h)
 *
 * The probe is skipped when no enumeration is required (no format list and
 * no nearest snap), in which case opts pass straight to the HAL. */
static int lua_module_camera_open(lua_State *L)
{
    const char *dev_path = luaL_checkstring(L, 1);
    camera_open_opts_t opts = {0};
    bool opt_nearest = false;
    enum { LUA_CAMERA_MAX_PREFERRED = 16 };
    uint32_t preferred[LUA_CAMERA_MAX_PREFERRED];
    int n_preferred = 0;
    bool has_format = false;
    bool has_opts_table = false;
    esp_err_t err;

    if (!lua_isnoneornil(L, 2)) {
        if (!lua_istable(L, 2)) {
            return luaL_error(L, "camera.open opts must be a table");
        }
        has_opts_table = true;

        lua_getfield(L, 2, "format");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return luaL_error(L,
                    "camera.open opts.format must be an array of FOURCC strings, e.g. { \"RGBP\" }");
            }
            lua_Integer raw_count = luaL_len(L, -1);
            if (raw_count <= 0) {
                lua_pop(L, 1);
                return luaL_error(L, "camera.open opts.format must be a non-empty array");
            }
            if (raw_count > LUA_CAMERA_MAX_PREFERRED) {
                lua_pop(L, 1);
                return luaL_error(L, "camera.open opts.format length %d exceeds limit %d",
                                  (int)raw_count, LUA_CAMERA_MAX_PREFERRED);
            }
            for (lua_Integer i = 1; i <= raw_count; i++) {
                lua_rawgeti(L, -1, i);
                if (!lua_isstring(L, -1)) {
                    lua_pop(L, 2);
                    return luaL_error(L, "camera.open opts.format[%d] must be a FOURCC string", (int)i);
                }
                const char *fourcc = lua_tostring(L, -1);
                lua_module_camera_parse_fourcc(L, fourcc, &preferred[n_preferred++]);
                lua_pop(L, 1);
            }
            has_format = true;
        }
        lua_pop(L, 1); /* format value (array or nil) */

        lua_module_camera_optional_uint_field(L, 2, "width", &opts.width);
        lua_module_camera_optional_uint_field(L, 2, "height", &opts.height);
        lua_module_camera_optional_bool_field(L, 2, "nearest", &opt_nearest);
    }

    /* Fast path: nothing to enumerate. Pass through to the HAL — either no
     * opts at all (idempotent open) or exact (w, h) with no nearest snap. */
    bool needs_probe = has_format || (opt_nearest && opts.width != 0 && opts.height != 0);
    if (!needs_probe) {
        err = camera_open(dev_path, has_opts_table ? &opts : NULL);
        if (err == ESP_ERR_INVALID_STATE) {
            return luaL_error(L, "camera open with opts requires close first (already open)");
        }
        if (err != ESP_OK) {
            return luaL_error(L, "camera open failed: %s", esp_err_to_name(err));
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    /* Probe-open so we can enumerate. The probe path silently closes any
     * existing session — callers that hit this path explicitly asked for a
     * non-default configuration, and refusing here would require them to
     * also call camera.close() before every reconfigure. */
    if (camera_is_open()) {
        err = camera_close();
        if (err != ESP_OK) {
            return luaL_error(L, "camera open: close-before-probe failed: %s", esp_err_to_name(err));
        }
    }
    err = camera_open(dev_path, NULL);
    if (err != ESP_OK) {
        return luaL_error(L, "camera open: probe failed: %s", esp_err_to_name(err));
    }

    /* Resolve fourcc. */
    uint32_t chosen_fourcc = 0;
    if (has_format) {
        err = lua_module_camera_resolve_fourcc(preferred, n_preferred, &chosen_fourcc);
        if (err == ESP_ERR_NOT_FOUND) {
            char avail[64];
            lua_module_camera_describe_available(avail, sizeof(avail));
            (void)camera_close();
            return luaL_error(L, "camera open: none of requested formats supported; sensor offers {%s}", avail);
        }
        if (err != ESP_OK) {
            (void)camera_close();
            return luaL_error(L, "camera open: format enumerate failed: %s", esp_err_to_name(err));
        }
    } else {
        camera_stream_info_t info = {0};
        err = camera_get_stream_info(&info);
        if (err != ESP_OK) {
            (void)camera_close();
            return luaL_error(L, "camera open: read probe stream info failed: %s", esp_err_to_name(err));
        }
        chosen_fourcc = info.pixel_format;
    }

    /* Resolve (width, height) under the chosen fourcc.
     *   - user gave size + nearest=true → snap via camera_find_closest_size
     *   - user gave size + nearest=false → verbatim (HAL S_FMT will fail loudly
     *     if the driver rejects the request — that is the documented contract)
     *   - user gave no size + format was specified → first enumerated discrete
     *     for the chosen fourcc so we don't carry over a default size that
     *     belonged to a different pixel format
     *   - if enumeration is unavailable on this driver, keep what we have
     *     (zero) and let the HAL fall back to its G_FMT defaults */
    uint32_t final_w = opts.width;
    uint32_t final_h = opts.height;
    if (final_w != 0 && final_h != 0 && opt_nearest) {
        uint32_t exact_w = 0;
        uint32_t exact_h = 0;
        esp_err_t cerr = camera_find_closest_size(chosen_fourcc, final_w, final_h, &exact_w, &exact_h);
        if (cerr == ESP_OK) {
            final_w = exact_w;
            final_h = exact_h;
        }
    } else if (final_w == 0 && final_h == 0 && has_format) {
        uint32_t enum_w = 0;
        uint32_t enum_h = 0;
        if (lua_module_camera_enum_first_discrete_size(chosen_fourcc, &enum_w, &enum_h) == ESP_OK) {
            final_w = enum_w;
            final_h = enum_h;
        }
    }

    /* If the probe already landed on the target configuration, skip the
     * close/reopen cycle. */
    camera_stream_info_t current = {0};
    if (camera_get_stream_info(&current) == ESP_OK
            && current.pixel_format == chosen_fourcc
            && (final_w == 0 || current.width == final_w)
            && (final_h == 0 || current.height == final_h)) {
        lua_pushboolean(L, 1);
        return 1;
    }

    camera_open_opts_t reopen = {
        .pixel_format = chosen_fourcc,
        .width = final_w,
        .height = final_h,
    };
    err = camera_close();
    if (err != ESP_OK) {
        return luaL_error(L, "camera open: close-before-reopen failed: %s", esp_err_to_name(err));
    }
    err = camera_open(dev_path, &reopen);
    if (err != ESP_OK) {
        return luaL_error(L, "camera open: reopen failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* camera.is_open() -> bool */
static int lua_module_camera_is_open(lua_State *L)
{
    lua_pushboolean(L, camera_is_open());
    return 1;
}

/* camera.is_streaming() -> bool */
static int lua_module_camera_is_streaming(lua_State *L)
{
    lua_pushboolean(L, camera_is_streaming());
    return 1;
}

/* camera.flush()
 * Drops every queued buffer so the next get_frame() returns a freshly captured
 * frame. Useful after long idle / wake-up to discard stale AE/AWB output. */
static int lua_module_camera_flush(lua_State *L)
{
    esp_err_t err = camera_flush();

    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera flush failed: not streaming or one or more frames are still borrowed");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera flush failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Push a Lua array of discrete fps integers onto the stack. Stepwise / range
 * intervals are intentionally ignored — they are rare and surfacing them
 * doubles the output shape. Returns the number of fps entries pushed; caller
 * decides whether to attach (count > 0) or discard (count == 0). */
static int lua_module_camera_push_fps_for_size(lua_State *L, uint32_t pixel_format,
                                               uint32_t w, uint32_t h)
{
    camera_frame_interval_t interval = {0};
    int count = 0;

    lua_newtable(L); /* fps array */
    for (uint32_t k = 0; ; k++) {
        esp_err_t err = camera_enum_frame_interval(pixel_format, w, h, k, &interval);
        if (err != ESP_OK) {
            break;
        }
        if (interval.type != CAMERA_FRAME_SIZE_DISCRETE) {
            break;
        }
        if (interval.numerator == 0) {
            continue;
        }
        if (interval.denominator % interval.numerator == 0) {
            lua_pushinteger(L, (lua_Integer)(interval.denominator / interval.numerator));
        } else {
            lua_pushnumber(L, (lua_Number)interval.denominator / (lua_Number)interval.numerator);
        }
        lua_rawseti(L, -2, ++count);
    }
    return count;
}

/* camera.list_formats() — see README. Output shape:
 *   {
 *     { format = "JPEG", description = "...",
 *       sizes = { { w=640, h=480, fps = {30, 15} }, ... },
 *     },
 *     ...
 *   }
 * Empty fps arrays, stepwise/continuous size ranges, and verbose driver flags
 * are intentionally omitted; query the V4L2 raw enum APIs via custom C code if
 * those are needed. */
static int lua_module_camera_list_formats(lua_State *L)
{
    camera_format_desc_t desc = {0};

    lua_newtable(L);

    for (uint32_t i = 0; ; i++) {
        esp_err_t err = camera_enum_format(i, &desc);
        if (err == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            return luaL_error(L, "camera list_formats failed: camera not opened");
        }
        if (err != ESP_OK) {
            return luaL_error(L, "camera list_formats failed: %s", esp_err_to_name(err));
        }

        lua_newtable(L); /* format entry */
        lua_pushstring(L, desc.pixel_format_str);
        lua_setfield(L, -2, "format");
        lua_pushstring(L, desc.description);
        lua_setfield(L, -2, "description");

        lua_newtable(L); /* sizes (array of discrete) */
        int discrete_count = 0;

        for (uint32_t j = 0; ; j++) {
            camera_frame_size_t fsz = {0};
            esp_err_t serr = camera_enum_frame_size(desc.pixel_format, j, &fsz);
            if (serr == ESP_ERR_NOT_FOUND) {
                break;
            }
            if (serr != ESP_OK) {
                return luaL_error(L, "camera enum_frame_size failed: %s", esp_err_to_name(serr));
            }

            if (fsz.type != CAMERA_FRAME_SIZE_DISCRETE) {
                /* Keep the Lua shape simple: list_formats exposes only discrete sizes. */
                break;
            }

            lua_newtable(L); /* size entry */
            lua_pushinteger(L, (lua_Integer)fsz.width);
            lua_setfield(L, -2, "w");
            lua_pushinteger(L, (lua_Integer)fsz.height);
            lua_setfield(L, -2, "h");
            /* Only attach fps when the driver enumerates discrete intervals. */
            int fps_count = lua_module_camera_push_fps_for_size(L, desc.pixel_format,
                                                                fsz.width, fsz.height);
            if (fps_count > 0) {
                lua_setfield(L, -2, "fps");
            } else {
                lua_pop(L, 1);
            }

            lua_rawseti(L, -2, ++discrete_count);
        }

        lua_setfield(L, -2, "sizes");

        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }

    return 1;
}

/* camera.info()
 * Returns a table with stream info: width, height, pixel_format. */
static int lua_module_camera_info(lua_State *L)
{
    camera_stream_info_t info = {0};
    esp_err_t err = camera_get_stream_info(&info);

    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera info failed: camera not opened, call camera.open() first");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera info failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushinteger(L, info.width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, info.height);
    lua_setfield(L, -2, "height");
    lua_pushstring(L, info.pixel_format_str);
    lua_setfield(L, -2, "pixel_format");
    return 1;
}

/* camera.get_frame([timeout_ms])
 * Borrows one V4L2 buffer and wraps it as an image.frame userdata.
 * Release with frame:release() or by declaring the variable <close>. */
static int lua_module_camera_get_frame(lua_State *L)
{
    camera_frame_info_t frame_info = {0};
    lua_image_frame_info_t info = {0};
    int timeout_ms = (int)luaL_optinteger(L, 1, 0);
    uint8_t *frame_data = NULL;
    size_t frame_bytes = 0;
    esp_err_t err;

    if (timeout_ms < 0) {
        return luaL_error(L, "timeout_ms must be non-negative");
    }

    err = camera_capture_frame(timeout_ms, &frame_data, &frame_bytes, &frame_info);
    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "camera get_frame failed: camera not opened or no capture buffer is available");
    }
    if (err != ESP_OK) {
        return luaL_error(L, "camera get_frame failed: %s", esp_err_to_name(err));
    }

    info.width = (int)frame_info.width;
    info.height = (int)frame_info.height;
    info.bytes = frame_bytes;
    info.timestamp_us = frame_info.timestamp_us;
    strlcpy(info.pixel_format, frame_info.pixel_format_str, sizeof(info.pixel_format));

    err = lua_image_push_frame(L, frame_data, frame_bytes, &info,
                                    lua_module_camera_frame_release_cb, NULL);
    if (err != ESP_OK) {
        /* push failed before transferring ownership; we still hold the buffer */
        camera_release_frame(frame_data);
        return luaL_error(L, "camera get_frame failed: %s", esp_err_to_name(err));
    }
    return 1;
}

/* camera.close()
 * Closes the camera device and releases all resources. */
static int lua_module_camera_close(lua_State *L)
{
    esp_err_t err = camera_close();

    if (err != ESP_OK) {
        uint32_t borrowed_count = 0;
        if (err == ESP_ERR_INVALID_STATE && camera_get_borrowed_count(&borrowed_count) == ESP_OK && borrowed_count > 0) {
            return luaL_error(L, "camera close failed: %" PRIu32 " image frame(s) still hold camera buffers; "
                              "release all frame views first", borrowed_count);
        }
        return luaL_error(L, "camera close failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_camera(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"open",         lua_module_camera_open},
        {"info",         lua_module_camera_info},
        {"get_frame",    lua_module_camera_get_frame},
        {"flush",        lua_module_camera_flush},
        {"is_open",      lua_module_camera_is_open},
        {"is_streaming", lua_module_camera_is_streaming},
        {"list_formats", lua_module_camera_list_formats},
        {"close",        lua_module_camera_close},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_camera_register(void)
{
    return cap_lua_register_module(LUA_MODULE_CAMERA_NAME, luaopen_camera);
}
