/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_vision.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <new>
#include <string>
#include <vector>

#include "color_detect.hpp"
#include "dl_image.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C" {
#include "lauxlib.h"
#include "lua_image.h"
}

static const char *TAG = "lua_color_detect";

#define LUA_COLOR_DETECT_WIDTH  100
#define LUA_COLOR_DETECT_HEIGHT 100
#define LUA_COLOR_DETECT_NAME   "target"

typedef struct {
    int source_x;
    int source_y;
    int source_width;
    int source_height;
    int min_pixels;
    int max_blob_pixels;
    std::array<uint8_t, 3> hsv_min;
    std::array<uint8_t, 3> hsv_max;
} lua_color_detect_config_t;

typedef struct {
    int category;
    float score;
    int left;
    int top;
    int right;
    int bottom;
    int area;
} lua_color_detect_result_t;

static ColorDetect *s_detector;
static StaticSemaphore_t s_detector_mutex_buffer;
static SemaphoreHandle_t s_detector_mutex;
static bool s_registered_color;
static std::array<uint8_t, 3> s_registered_hsv_min;
static std::array<uint8_t, 3> s_registered_hsv_max;
static int s_registered_min_pixels;
static uint16_t *s_crop_scratch;
static size_t s_crop_scratch_pixels;

static SemaphoreHandle_t lua_color_detect_get_mutex(void)
{
    if (s_detector_mutex == nullptr) {
        s_detector_mutex = xSemaphoreCreateMutexStatic(&s_detector_mutex_buffer);
    }
    return s_detector_mutex;
}

static esp_err_t lua_color_detect_init_detector(void)
{
    if (s_detector != nullptr) {
        return ESP_OK;
    }

    s_detector = new (std::nothrow) ColorDetect(LUA_COLOR_DETECT_WIDTH, LUA_COLOR_DETECT_HEIGHT);
    if (s_detector == nullptr) {
        ESP_LOGE(TAG, "detector alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void lua_color_detect_release_crop_scratch_locked(void)
{
    heap_caps_free(s_crop_scratch);
    s_crop_scratch = nullptr;
    s_crop_scratch_pixels = 0;
}

static void lua_color_detect_release_detector_locked(void)
{
    delete s_detector;
    s_detector = nullptr;
    s_registered_color = false;
    s_registered_min_pixels = 0;
    lua_color_detect_release_crop_scratch_locked();
}

static esp_err_t lua_color_detect_ensure_crop_scratch_locked(size_t pixels, uint16_t **out)
{
    uint16_t *new_scratch = nullptr;

    if (out == nullptr || pixels == 0 || pixels > SIZE_MAX / sizeof(uint16_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_crop_scratch != nullptr && s_crop_scratch_pixels >= pixels) {
        *out = s_crop_scratch;
        return ESP_OK;
    }

    new_scratch = static_cast<uint16_t *>(heap_caps_aligned_alloc(
        16, pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_scratch == nullptr) {
        ESP_LOGE(TAG, "crop scratch alloc failed: %u bytes", (unsigned)(pixels * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    heap_caps_free(s_crop_scratch);
    s_crop_scratch = new_scratch;
    s_crop_scratch_pixels = pixels;
    *out = s_crop_scratch;
    ESP_LOGI(TAG, "crop scratch ready: %u bytes", (unsigned)(pixels * sizeof(uint16_t)));
    return ESP_OK;
}

static bool lua_color_detect_get_integer_field(lua_State *L, int table_idx, const char *name, lua_Integer *out)
{
    bool ok = false;
    lua_getfield(L, table_idx, name);
    if (lua_isinteger(L, -1)) {
        *out = lua_tointeger(L, -1);
        ok = true;
    } else if (lua_isnumber(L, -1)) {
        *out = (lua_Integer)lua_tonumber(L, -1);
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

static bool lua_color_detect_get_number_field(lua_State *L, int table_idx, const char *name, lua_Number *out)
{
    bool ok = false;
    lua_getfield(L, table_idx, name);
    if (lua_isnumber(L, -1)) {
        *out = lua_tonumber(L, -1);
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

static uint8_t lua_color_detect_sv_to_u8(lua_Number value)
{
    if (value <= 1.0) {
        value *= 255.0;
    }
    value = std::max<lua_Number>(0.0, std::min<lua_Number>(255.0, value));
    return static_cast<uint8_t>(std::lround(value));
}

static esp_err_t lua_color_detect_parse_source(lua_State *L, int opts_idx, lua_color_detect_config_t *config)
{
    lua_Integer integer_value = 0;

    lua_getfield(L, opts_idx, "source");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return ESP_OK;
    }

    int source_idx = lua_gettop(L);
    if (lua_color_detect_get_integer_field(L, source_idx, "x", &integer_value)) {
        config->source_x = static_cast<int>(integer_value);
    }
    if (lua_color_detect_get_integer_field(L, source_idx, "y", &integer_value)) {
        config->source_y = static_cast<int>(integer_value);
    }
    if (lua_color_detect_get_integer_field(L, source_idx, "width", &integer_value)) {
        config->source_width = static_cast<int>(integer_value);
    }
    if (lua_color_detect_get_integer_field(L, source_idx, "height", &integer_value)) {
        config->source_height = static_cast<int>(integer_value);
    }

    lua_pop(L, 1);
    return ESP_OK;
}

static esp_err_t lua_color_detect_parse_config(lua_State *L,
                                               int opts_idx,
                                               int frame_width,
                                               int frame_height,
                                               lua_color_detect_config_t *config)
{
    lua_Integer integer_value = 0;
    lua_Number number_value = 0;

    config->source_x = 0;
    config->source_y = 0;
    config->source_width = frame_width;
    config->source_height = frame_height;
    config->min_pixels = 250;
    config->max_blob_pixels = 0;
    config->hsv_min = {50, 80, 50};
    config->hsv_max = {88, 255, 255};

    if (opts_idx > 0 && lua_istable(L, opts_idx)) {
        opts_idx = lua_absindex(L, opts_idx);
        lua_color_detect_parse_source(L, opts_idx, config);
        if (lua_color_detect_get_integer_field(L, opts_idx, "min_pixels", &integer_value)) {
            config->min_pixels = static_cast<int>(integer_value);
        }
        if (lua_color_detect_get_integer_field(L, opts_idx, "max_blob_pixels", &integer_value) ||
            lua_color_detect_get_integer_field(L, opts_idx, "max_pixels", &integer_value)) {
            config->max_blob_pixels = static_cast<int>(integer_value);
        }
        if (lua_color_detect_get_integer_field(L, opts_idx, "h_min", &integer_value)) {
            config->hsv_min[0] = static_cast<uint8_t>(integer_value);
        }
        if (lua_color_detect_get_integer_field(L, opts_idx, "h_max", &integer_value)) {
            config->hsv_max[0] = static_cast<uint8_t>(integer_value);
        }
        if (lua_color_detect_get_number_field(L, opts_idx, "s_min", &number_value)) {
            config->hsv_min[1] = lua_color_detect_sv_to_u8(number_value);
        }
        if (lua_color_detect_get_number_field(L, opts_idx, "s_max", &number_value)) {
            config->hsv_max[1] = lua_color_detect_sv_to_u8(number_value);
        }
        if (lua_color_detect_get_number_field(L, opts_idx, "v_min", &number_value)) {
            config->hsv_min[2] = lua_color_detect_sv_to_u8(number_value);
        }
        if (lua_color_detect_get_number_field(L, opts_idx, "v_max", &number_value)) {
            config->hsv_max[2] = lua_color_detect_sv_to_u8(number_value);
        }
    }

    if (config->source_width <= 0 || config->source_height <= 0 ||
        config->source_x < 0 || config->source_y < 0 ||
        config->source_x + config->source_width > frame_width ||
        config->source_y + config->source_height > frame_height ||
        config->min_pixels <= 0 ||
        config->hsv_min[0] > 180 || config->hsv_max[0] > 180 ||
        config->hsv_min[1] >= config->hsv_max[1] ||
        config->hsv_min[2] >= config->hsv_max[2]) {
        return ESP_ERR_INVALID_ARG;
    }

    const int source_pixels = config->source_width * config->source_height;
    if (config->max_blob_pixels <= 0) {
        config->max_blob_pixels = source_pixels * 35 / 100;
    }
    if (config->max_blob_pixels <= 0 || config->max_blob_pixels >= source_pixels) {
        config->max_blob_pixels = source_pixels;
    }
    if (config->min_pixels >= source_pixels) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static bool lua_color_detect_needs_crop(const lua_color_detect_config_t *config, const lua_image_view_t *view)
{
    return config->source_x != 0 || config->source_y != 0 ||
           config->source_width != view->width || config->source_height != view->height;
}

static esp_err_t lua_color_detect_copy_rgb565_crop_locked(const lua_image_view_t *view,
                                                          const lua_color_detect_config_t *config,
                                                          uint16_t **out)
{
    const uint16_t *src = reinterpret_cast<const uint16_t *>(view->data);
    const size_t pixels = static_cast<size_t>(config->source_width) * config->source_height;
    uint16_t *crop = nullptr;
    esp_err_t err = lua_color_detect_ensure_crop_scratch_locked(pixels, &crop);
    if (err != ESP_OK) {
        return err;
    }

    for (int row = 0; row < config->source_height; row++) {
        const uint16_t *src_row = src + (static_cast<size_t>(config->source_y + row) * view->width) +
                                  config->source_x;
        uint16_t *dst_row = crop + (static_cast<size_t>(row) * config->source_width);
        memcpy(dst_row, src_row, static_cast<size_t>(config->source_width) * sizeof(uint16_t));
    }
    *out = crop;
    return ESP_OK;
}

static int lua_color_detect_scale_min_pixels(const lua_color_detect_config_t *config)
{
    const int source_pixels = config->source_width * config->source_height;
    const int detect_pixels = LUA_COLOR_DETECT_WIDTH * LUA_COLOR_DETECT_HEIGHT;
    int scaled = (config->min_pixels * detect_pixels + source_pixels - 1) / source_pixels;
    scaled = std::max(1, scaled);
    return std::min(scaled, detect_pixels - 1);
}

static esp_err_t lua_color_detect_prepare_detector(const lua_color_detect_config_t *config)
{
    esp_err_t err = lua_color_detect_init_detector();
    if (err != ESP_OK) {
        return err;
    }

    const int scaled_min_pixels = lua_color_detect_scale_min_pixels(config);
    if (s_registered_color &&
        s_registered_hsv_min == config->hsv_min &&
        s_registered_hsv_max == config->hsv_max &&
        s_registered_min_pixels == scaled_min_pixels) {
        return ESP_OK;
    }

    while (s_detector->get_color_num() > 0) {
        s_detector->delete_color(0);
    }
    s_detector->register_color(config->hsv_min,
                               config->hsv_max,
                               LUA_COLOR_DETECT_NAME,
                               scaled_min_pixels);
    if (s_detector->get_color_num() == 0) {
        s_registered_color = false;
        return ESP_ERR_INVALID_STATE;
    }
    s_registered_color = true;
    s_registered_hsv_min = config->hsv_min;
    s_registered_hsv_max = config->hsv_max;
    s_registered_min_pixels = scaled_min_pixels;
    return ESP_OK;
}

static bool lua_color_detect_convert_result(const dl::detect::result_t &result, int limit_width, int limit_height, lua_color_detect_result_t *out)
{
    if (out == nullptr || result.box.size() < 4 || limit_width <= 0 || limit_height <= 0) {
        ESP_LOGE(TAG, "invalid color_detect result");
        return false;
    }

    // Keep Lua output compatible with the previous local best-result extension.
    lua_color_detect_result_t converted = {
        .category = result.category,
        .score = result.score,
        .left = std::max(0, std::min(result.box[0], limit_width - 1)),
        .top = std::max(0, std::min(result.box[1], limit_height - 1)),
        .right = std::max(0, std::min(result.box[2], limit_width - 1)),
        .bottom = std::max(0, std::min(result.box[3], limit_height - 1)),
        .area = 0,
    };
    if (converted.right < converted.left || converted.bottom < converted.top) {
        ESP_LOGE(TAG, "invalid color_detect box: left=%d top=%d right=%d bottom=%d", converted.left, converted.top, converted.right, converted.bottom);
        return false;
    }

    converted.area = (converted.right - converted.left + 1) * (converted.bottom - converted.top + 1);
    if (converted.area <= 0) {
        ESP_LOGE(TAG, "invalid color_detect box area");
        return false;
    }

    *out = converted;
    return true;
}

static bool lua_color_detect_select_best(const std::list<dl::detect::result_t> &results,
                                         const lua_color_detect_config_t *config,
                                         int limit_width,
                                         int limit_height,
                                         lua_color_detect_result_t *out)
{
    bool has_best = false;
    lua_color_detect_result_t best = {};

    if (config == nullptr || out == nullptr) {
        ESP_LOGE(TAG, "invalid select_best arguments");
        return false;
    }

    for (const dl::detect::result_t &result : results) {
        lua_color_detect_result_t candidate = {};
        if (!lua_color_detect_convert_result(result, limit_width, limit_height, &candidate)) {
            continue;
        }
        if (config->max_blob_pixels > 0 && candidate.area > config->max_blob_pixels) {
            continue;
        }
        if (!has_best || candidate.area > best.area) {
            best = candidate;
            has_best = true;
        }
    }

    if (has_best) {
        *out = best;
    }
    return has_best;
}

static void lua_color_detect_push_empty(lua_State *L, const lua_color_detect_config_t *config, int frame_width, int frame_height)
{
    lua_newtable(L);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "count");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "detected");
    lua_pushinteger(L, frame_width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, frame_height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, config->source_x);
    lua_setfield(L, -2, "source_x");
    lua_pushinteger(L, config->source_y);
    lua_setfield(L, -2, "source_y");
    lua_pushinteger(L, config->source_width);
    lua_setfield(L, -2, "source_width");
    lua_pushinteger(L, config->source_height);
    lua_setfield(L, -2, "source_height");
}

static void lua_color_detect_push_result(lua_State *L,
                                         const lua_color_detect_config_t *config,
                                         const lua_color_detect_result_t *result,
                                         int frame_width,
                                         int frame_height)
{
    const int left = config->source_x + result->left;
    const int top = config->source_y + result->top;
    const int right = config->source_x + result->right;
    const int bottom = config->source_y + result->bottom;
    const int box_w = right - left + 1;
    const int box_h = bottom - top + 1;
    const int pixels = box_w * box_h;
    const double cx = (static_cast<double>(left) + static_cast<double>(right)) * 0.5;
    const double cy = (static_cast<double>(top) + static_cast<double>(bottom)) * 0.5;

    lua_newtable(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "count");
    lua_pushboolean(L, true);
    lua_setfield(L, -2, "detected");
    lua_pushinteger(L, pixels);
    lua_setfield(L, -2, "pixels");
    lua_pushinteger(L, frame_width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, frame_height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, result->category);
    lua_setfield(L, -2, "category");
    lua_pushnumber(L, result->score);
    lua_setfield(L, -2, "score");

    lua_pushinteger(L, config->source_x);
    lua_setfield(L, -2, "source_x");
    lua_pushinteger(L, config->source_y);
    lua_setfield(L, -2, "source_y");
    lua_pushinteger(L, config->source_width);
    lua_setfield(L, -2, "source_width");
    lua_pushinteger(L, config->source_height);
    lua_setfield(L, -2, "source_height");

    lua_pushinteger(L, left);
    lua_setfield(L, -2, "left");
    lua_pushinteger(L, top);
    lua_setfield(L, -2, "top");
    lua_pushinteger(L, right);
    lua_setfield(L, -2, "right");
    lua_pushinteger(L, bottom);
    lua_setfield(L, -2, "bottom");
    lua_pushinteger(L, left);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, top);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, box_w);
    lua_setfield(L, -2, "box_width");
    lua_pushinteger(L, box_h);
    lua_setfield(L, -2, "box_height");
    lua_pushnumber(L, cx);
    lua_setfield(L, -2, "cx");
    lua_pushnumber(L, cy);
    lua_setfield(L, -2, "cy");

    lua_createtable(L, 4, 0);
    lua_pushinteger(L, left);
    lua_rawseti(L, -2, 1);
    lua_pushinteger(L, top);
    lua_rawseti(L, -2, 2);
    lua_pushinteger(L, right);
    lua_rawseti(L, -2, 3);
    lua_pushinteger(L, bottom);
    lua_rawseti(L, -2, 4);
    lua_setfield(L, -2, "box");
}

static int lua_color_detect_detect(lua_State *L)
{
    lua_image_view_t view = {};
    lua_color_detect_config_t config = {};
    lua_color_detect_result_t best_result = {};
    bool detected = false;
    uint16_t *crop = nullptr;
    const uint8_t *detect_data = nullptr;
    int detect_width = 0;
    int detect_height = 0;

    esp_err_t err = lua_image_require_format(L, 1, LUA_IMAGE_FORMAT_RGB565LE, &view);
    if (err != ESP_OK) {
        return luaL_error(L, "color_detect unsupported frame: %s", esp_err_to_name(err));
    }

    err = lua_color_detect_parse_config(L, lua_istable(L, 2) ? 2 : 0, view.width, view.height, &config);
    if (err != ESP_OK) {
        lua_image_release_view(&view);
        return luaL_error(L, "invalid color_detect options");
    }

    SemaphoreHandle_t mutex = lua_color_detect_get_mutex();
    if (mutex == nullptr) {
        lua_image_release_view(&view);
        return luaL_error(L, "color_detect mutex alloc failed");
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        lua_image_release_view(&view);
        return luaL_error(L, "color_detect lock failed");
    }

    detect_data = view.data;
    detect_width = view.width;
    detect_height = view.height;
    if (lua_color_detect_needs_crop(&config, &view)) {
        err = lua_color_detect_copy_rgb565_crop_locked(&view, &config, &crop);
        if (err != ESP_OK) {
            xSemaphoreGive(mutex);
            lua_image_release_view(&view);
            return luaL_error(L, "color_detect crop failed: %s", esp_err_to_name(err));
        }
        detect_data = reinterpret_cast<const uint8_t *>(crop);
        detect_width = config.source_width;
        detect_height = config.source_height;
    }

    dl::image::img_t img = {
        .data = const_cast<uint8_t *>(detect_data),
        .width = static_cast<uint16_t>(detect_width),
        .height = static_cast<uint16_t>(detect_height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };

    err = lua_color_detect_prepare_detector(&config);
    if (err == ESP_OK) {
        // The registry component exposes run(); Lua keeps the one-best-result API by selecting here.
        const std::list<dl::detect::result_t> &results = s_detector->run(img);
        detected = lua_color_detect_select_best(results, &config, detect_width, detect_height, &best_result);
    }
    xSemaphoreGive(mutex);

    if (err != ESP_OK) {
        lua_image_release_view(&view);
        return luaL_error(L, "color_detect detector prepare failed: %s", esp_err_to_name(err));
    }

    if (!detected) {
        lua_color_detect_push_empty(L, &config, view.width, view.height);
    } else {
        lua_color_detect_push_result(L, &config, &best_result, view.width, view.height);
    }

    lua_image_release_view(&view);
    return 1;
}

static int lua_color_detect_release(lua_State *L)
{
    (void)L;
    SemaphoreHandle_t mutex = lua_color_detect_get_mutex();
    if (mutex == nullptr) {
        return 0;
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
        lua_color_detect_release_detector_locked();
        xSemaphoreGive(mutex);
    }
    return 0;
}

extern "C" int luaopen_color_detect_dl(lua_State *L)
{
    esp_err_t err = lua_color_detect_init_detector();
    if (err != ESP_OK) {
        return luaL_error(L, "color_detect init failed: %s", esp_err_to_name(err));
    }

    static const luaL_Reg funcs[] = {
        {"detect", lua_color_detect_detect},
        {"release", lua_color_detect_release},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}
