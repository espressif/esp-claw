/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_vision.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vision_core.h"
#include "lauxlib.h"
#include "lua_image.h"
#include "vision_core_runtime.h"

static const char *TAG = "lua_color_detect";

#define LUA_COLOR_DETECT_HUE_STEP       10
#define LUA_COLOR_DETECT_MAX_THRESHOLDS 8
#define LUA_COLOR_DETECT_RESULT_CAPACITY 32
#define LUA_COLOR_DETECT_LAB_PADDING    1
#define LUA_COLOR_DETECT_SIZE           100
#define LUA_COLOR_DETECT_MAX_STRIDE     16

typedef struct {
    int source_x;
    int source_y;
    int source_width;
    int source_height;
    int min_pixels;
    int max_blob_pixels;
    int x_stride;
    int y_stride;
    int h_min;
    int h_max;
    uint8_t s_min;
    uint8_t s_max;
    uint8_t v_min;
    uint8_t v_max;
    bool use_lab;
    esp_vision_color_threshold_t lab;
} lua_color_detect_config_t;

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
    int area;
} lua_color_detect_result_t;

typedef struct {
    int l;
    int a;
    int b;
} lua_color_detect_lab_t;

static esp_vision_blob_t *s_blobs;
static uint16_t *s_detect_buffer;
static bool s_thresholds_valid;
static lua_color_detect_config_t s_threshold_config;
static esp_vision_color_threshold_t s_thresholds[LUA_COLOR_DETECT_MAX_THRESHOLDS];
static size_t s_threshold_count;

static int lua_color_detect_clamp(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    return value > max_value ? max_value : value;
}

static esp_err_t lua_color_detect_init_locked(void)
{
    if (s_blobs != NULL && s_detect_buffer != NULL) {
        return ESP_OK;
    }

    s_blobs = heap_caps_calloc(LUA_COLOR_DETECT_RESULT_CAPACITY, sizeof(*s_blobs), MALLOC_CAP_8BIT);
    if (s_blobs == NULL) {
        ESP_LOGE(TAG, "blob result alloc failed");
        return ESP_ERR_NO_MEM;
    }
    size_t buffer_size = LUA_COLOR_DETECT_SIZE * LUA_COLOR_DETECT_SIZE * sizeof(*s_detect_buffer);
    s_detect_buffer = heap_caps_aligned_alloc(16, buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_detect_buffer == NULL) {
        s_detect_buffer = heap_caps_aligned_alloc(16, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_detect_buffer == NULL) {
        ESP_LOGE(TAG, "detect buffer alloc failed");
        heap_caps_free(s_blobs);
        s_blobs = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void lua_color_detect_release_locked(void)
{
    heap_caps_free(s_blobs);
    s_blobs = NULL;
    heap_caps_free(s_detect_buffer);
    s_detect_buffer = NULL;
    s_thresholds_valid = false;
    s_threshold_count = 0;
}

static bool lua_color_detect_get_integer_field(lua_State *L, int table_idx, const char *name, int *out)
{
    bool found = false;

    lua_getfield(L, table_idx, name);
    if (lua_isnumber(L, -1)) {
        lua_Integer value = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : (lua_Integer)lua_tonumber(L, -1);
        if (value >= INT_MIN && value <= INT_MAX) {
            *out = (int)value;
            found = true;
        }
    }
    lua_pop(L, 1);
    return found;
}

static bool lua_color_detect_get_number_field(lua_State *L, int table_idx, const char *name, lua_Number *out)
{
    bool found = false;

    lua_getfield(L, table_idx, name);
    if (lua_isnumber(L, -1)) {
        *out = lua_tonumber(L, -1);
        found = true;
    }
    lua_pop(L, 1);
    return found;
}

static uint8_t lua_color_detect_sv_to_u8(lua_Number value)
{
    if (value <= 1.0) {
        value *= 255.0;
    }
    value = fmax(0.0, fmin(255.0, value));
    return (uint8_t)lround(value);
}

static esp_err_t lua_color_detect_parse_lab(lua_State *L, int opts_idx, lua_color_detect_config_t *config)
{
    static const char *const names[] = {"l_min", "l_max", "a_min", "a_max", "b_min", "b_max"};
    int values[sizeof(names) / sizeof(names[0])] = {0};
    size_t found = 0;

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        lua_getfield(L, opts_idx, names[i]);
        if (!lua_isnil(L, -1)) {
            if (!lua_isnumber(L, -1)) {
                lua_pop(L, 1);
                return ESP_ERR_INVALID_ARG;
            }
            lua_Number value = lua_tonumber(L, -1);
            if (!isfinite(value) || value < INT_MIN || value > INT_MAX) {
                lua_pop(L, 1);
                return ESP_ERR_INVALID_ARG;
            }
            values[i] = (int)value;
            found++;
        }
        lua_pop(L, 1);
    }

    if (found == 0) {
        return ESP_OK;
    }
    if (found != sizeof(names) / sizeof(names[0]) || values[0] < 0 || values[1] > 100 || values[2] < -128 || values[3] > 127 ||
        values[4] < -128 || values[5] > 127 || values[0] > values[1] || values[2] > values[3] || values[4] > values[5]) {
        return ESP_ERR_INVALID_ARG;
    }
    config->use_lab = true;
    config->lab = (esp_vision_color_threshold_t) {
        .l_min = (uint8_t)values[0],
        .l_max = (uint8_t)values[1],
        .a_min = (int8_t)values[2],
        .a_max = (int8_t)values[3],
        .b_min = (int8_t)values[4],
        .b_max = (int8_t)values[5],
    };
    return ESP_OK;
}

static void lua_color_detect_parse_source(lua_State *L, int opts_idx, lua_color_detect_config_t *config)
{
    lua_getfield(L, opts_idx, "source");
    if (lua_istable(L, -1)) {
        int source_idx = lua_gettop(L);
        lua_color_detect_get_integer_field(L, source_idx, "x", &config->source_x);
        lua_color_detect_get_integer_field(L, source_idx, "y", &config->source_y);
        lua_color_detect_get_integer_field(L, source_idx, "width", &config->source_width);
        lua_color_detect_get_integer_field(L, source_idx, "height", &config->source_height);
    }
    lua_pop(L, 1);
}

static esp_err_t lua_color_detect_parse_config(lua_State *L, int opts_idx, int frame_width, int frame_height, lua_color_detect_config_t *config)
{
    lua_Number number_value = 0;

    *config = (lua_color_detect_config_t) {
        .source_width = frame_width,
        .source_height = frame_height,
        .min_pixels = 250,
        .x_stride = 2,
        .y_stride = 2,
        .h_min = 50,
        .h_max = 88,
        .s_min = 80,
        .s_max = 255,
        .v_min = 50,
        .v_max = 255,
    };

    if (opts_idx > 0 && lua_istable(L, opts_idx)) {
        opts_idx = lua_absindex(L, opts_idx);
        if (lua_color_detect_parse_lab(L, opts_idx, config) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        lua_color_detect_parse_source(L, opts_idx, config);
        lua_color_detect_get_integer_field(L, opts_idx, "min_pixels", &config->min_pixels);
        if (!lua_color_detect_get_integer_field(L, opts_idx, "max_blob_pixels", &config->max_blob_pixels)) {
            lua_color_detect_get_integer_field(L, opts_idx, "max_pixels", &config->max_blob_pixels);
        }
        lua_color_detect_get_integer_field(L, opts_idx, "x_stride", &config->x_stride);
        lua_color_detect_get_integer_field(L, opts_idx, "y_stride", &config->y_stride);
        if (!config->use_lab) {
            lua_color_detect_get_integer_field(L, opts_idx, "h_min", &config->h_min);
            lua_color_detect_get_integer_field(L, opts_idx, "h_max", &config->h_max);
            if (lua_color_detect_get_number_field(L, opts_idx, "s_min", &number_value)) {
                config->s_min = lua_color_detect_sv_to_u8(number_value);
            }
            if (lua_color_detect_get_number_field(L, opts_idx, "s_max", &number_value)) {
                config->s_max = lua_color_detect_sv_to_u8(number_value);
            }
            if (lua_color_detect_get_number_field(L, opts_idx, "v_min", &number_value)) {
                config->v_min = lua_color_detect_sv_to_u8(number_value);
            }
            if (lua_color_detect_get_number_field(L, opts_idx, "v_max", &number_value)) {
                config->v_max = lua_color_detect_sv_to_u8(number_value);
            }
        }
    }

    if (config->source_x < 0 || config->source_y < 0 || config->source_width <= 0 || config->source_height <= 0 ||
        config->source_x > frame_width || config->source_y > frame_height || config->source_width > frame_width - config->source_x ||
        config->source_height > frame_height - config->source_y || config->min_pixels <= 0 ||
        config->x_stride <= 0 || config->x_stride > LUA_COLOR_DETECT_MAX_STRIDE ||
        config->y_stride <= 0 || config->y_stride > LUA_COLOR_DETECT_MAX_STRIDE ||
        config->source_x > INT16_MAX || config->source_y > INT16_MAX || config->source_width > INT16_MAX ||
        config->source_height > INT16_MAX || (!config->use_lab && (config->h_min < 0 || config->h_max > 180 ||
        config->h_min > config->h_max || config->s_min >= config->s_max || config->v_min >= config->v_max))) {
        return ESP_ERR_INVALID_ARG;
    }

    int source_pixels = config->source_width * config->source_height;
    if (config->max_blob_pixels <= 0) {
        config->max_blob_pixels = source_pixels * 35 / 100;
    }
    if (config->max_blob_pixels <= 0 || config->max_blob_pixels >= source_pixels) {
        config->max_blob_pixels = source_pixels;
    }
    return config->min_pixels < source_pixels ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void lua_color_detect_hsv_to_rgb565(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float hue = fmodf(h * 2.0f, 360.0f);
    float saturation = s / 255.0f;
    float value = v / 255.0f;
    float chroma = value * saturation;
    float x = chroma * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
    float m = value - chroma;
    float red = 0;
    float green = 0;
    float blue = 0;

    if (hue < 60.0f) {
        red = chroma;
        green = x;
    } else if (hue < 120.0f) {
        red = x;
        green = chroma;
    } else if (hue < 180.0f) {
        green = chroma;
        blue = x;
    } else if (hue < 240.0f) {
        green = x;
        blue = chroma;
    } else if (hue < 300.0f) {
        red = x;
        blue = chroma;
    } else {
        red = chroma;
        blue = x;
    }

    uint8_t r8 = (uint8_t)lroundf((red + m) * 255.0f);
    uint8_t g8 = (uint8_t)lroundf((green + m) * 255.0f);
    uint8_t b8 = (uint8_t)lroundf((blue + m) * 255.0f);
    uint16_t rgb565 = (uint16_t)(((r8 & 0xF8U) << 8) | ((g8 & 0xFCU) << 3) | (b8 >> 3));
    uint8_t r5 = (uint8_t)((rgb565 >> 11) & 0x1FU);
    uint8_t g6 = (uint8_t)((rgb565 >> 5) & 0x3FU);
    uint8_t b5 = (uint8_t)(rgb565 & 0x1FU);
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static float lua_color_detect_srgb_linear(uint8_t value)
{
    float channel = value / 255.0f;
    return ((channel > 0.04045f) ? powf((channel + 0.055f) / 1.055f, 2.4f) : channel / 12.92f) * 100.0f;
}

static float lua_color_detect_lab_curve(float value)
{
    return value > 0.008856f ? cbrtf(value) : value * 7.787037f + 0.137931f;
}

static lua_color_detect_lab_t lua_color_detect_hsv_to_lab(float h, float s, float v)
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    /* Match LAB bounds to the detector's RGB565 input. */
    lua_color_detect_hsv_to_rgb565(h, s, v, &r, &g, &b);

    float r_linear = lua_color_detect_srgb_linear(r);
    float g_linear = lua_color_detect_srgb_linear(g);
    float b_linear = lua_color_detect_srgb_linear(b);
    float x = lua_color_detect_lab_curve((r_linear * 0.4124f + g_linear * 0.3576f + b_linear * 0.1805f) / 95.047f);
    float y = lua_color_detect_lab_curve((r_linear * 0.2126f + g_linear * 0.7152f + b_linear * 0.0722f) / 100.0f);
    float z = lua_color_detect_lab_curve((r_linear * 0.0193f + g_linear * 0.1192f + b_linear * 0.9505f) / 108.883f);
    return (lua_color_detect_lab_t) {
        .l = lua_color_detect_clamp((int)floorf(116.0f * y) - 16, 0, 100),
        .a = lua_color_detect_clamp((int)floorf(500.0f * (x - y)), -128, 127),
        .b = lua_color_detect_clamp((int)floorf(200.0f * (y - z)), -128, 127),
    };
}

static size_t lua_color_detect_build_thresholds(const lua_color_detect_config_t *config, esp_vision_color_threshold_t *thresholds)
{
    if (config->use_lab) {
        thresholds[0] = config->lab;
        return 1;
    }
    /* Split nonlinear HSV ranges into tighter LAB boxes. */
    int hue_span = config->h_max - config->h_min;
    size_t count = (size_t)((hue_span + LUA_COLOR_DETECT_HUE_STEP - 1) / LUA_COLOR_DETECT_HUE_STEP);
    count = count == 0 ? 1 : count;
    count = count > LUA_COLOR_DETECT_MAX_THRESHOLDS ? LUA_COLOR_DETECT_MAX_THRESHOLDS : count;

    for (size_t i = 0; i < count; i++) {
        float hue_min = config->h_min + ((float)hue_span * i / count);
        float hue_max = config->h_min + ((float)hue_span * (i + 1) / count);
        int l_min = 100;
        int l_max = 0;
        int a_min = 127;
        int a_max = -128;
        int b_min = 127;
        int b_max = -128;

        for (int h_sample = 0; h_sample < 3; h_sample++) {
            float h = hue_min + (hue_max - hue_min) * h_sample * 0.5f;
            for (int s_sample = 0; s_sample < 3; s_sample++) {
                float s = config->s_min + (config->s_max - config->s_min) * s_sample * 0.5f;
                for (int v_sample = 0; v_sample < 3; v_sample++) {
                    float v = config->v_min + (config->v_max - config->v_min) * v_sample * 0.5f;
                    lua_color_detect_lab_t lab = lua_color_detect_hsv_to_lab(h, s, v);
                    l_min = lab.l < l_min ? lab.l : l_min;
                    l_max = lab.l > l_max ? lab.l : l_max;
                    a_min = lab.a < a_min ? lab.a : a_min;
                    a_max = lab.a > a_max ? lab.a : a_max;
                    b_min = lab.b < b_min ? lab.b : b_min;
                    b_max = lab.b > b_max ? lab.b : b_max;
                }
            }
        }

        thresholds[i] = (esp_vision_color_threshold_t) {
            .l_min = (uint8_t)lua_color_detect_clamp(l_min - LUA_COLOR_DETECT_LAB_PADDING, 0, 100),
            .l_max = (uint8_t)lua_color_detect_clamp(l_max + LUA_COLOR_DETECT_LAB_PADDING, 0, 100),
            .a_min = (int8_t)lua_color_detect_clamp(a_min - LUA_COLOR_DETECT_LAB_PADDING, -128, 127),
            .a_max = (int8_t)lua_color_detect_clamp(a_max + LUA_COLOR_DETECT_LAB_PADDING, -128, 127),
            .b_min = (int8_t)lua_color_detect_clamp(b_min - LUA_COLOR_DETECT_LAB_PADDING, -128, 127),
            .b_max = (int8_t)lua_color_detect_clamp(b_max + LUA_COLOR_DETECT_LAB_PADDING, -128, 127),
        };
    }
    return count;
}

static bool lua_color_detect_thresholds_match(const lua_color_detect_config_t *config)
{
    if (!s_thresholds_valid || config->use_lab != s_threshold_config.use_lab) {
        return false;
    }
    if (config->use_lab) {
        return config->lab.l_min == s_threshold_config.lab.l_min && config->lab.l_max == s_threshold_config.lab.l_max &&
               config->lab.a_min == s_threshold_config.lab.a_min && config->lab.a_max == s_threshold_config.lab.a_max &&
               config->lab.b_min == s_threshold_config.lab.b_min && config->lab.b_max == s_threshold_config.lab.b_max;
    }
    return config->h_min == s_threshold_config.h_min && config->h_max == s_threshold_config.h_max &&
           config->s_min == s_threshold_config.s_min && config->s_max == s_threshold_config.s_max &&
           config->v_min == s_threshold_config.v_min && config->v_max == s_threshold_config.v_max;
}

static void lua_color_detect_update_thresholds_locked(const lua_color_detect_config_t *config)
{
    if (lua_color_detect_thresholds_match(config)) {
        return;
    }
    s_threshold_count = lua_color_detect_build_thresholds(config, s_thresholds);
    s_threshold_config = *config;
    s_thresholds_valid = true;
}

static void lua_color_detect_resize_roi(const lua_image_view_t *view, const lua_color_detect_config_t *config, int detect_width, int detect_height)
{
    const uint16_t *source = (const uint16_t *)view->data;
    for (int y = 0; y < detect_height; y++) {
        int source_y = config->source_y + (int)((int64_t)y * config->source_height / detect_height);
        const uint16_t *source_row = source + source_y * view->width + config->source_x;
        uint16_t *dest_row = s_detect_buffer + y * detect_width;
        for (int x = 0; x < detect_width; x++) {
            int source_x = (int)((int64_t)x * config->source_width / detect_width);
            dest_row[x] = source_row[source_x];
        }
    }
}

static unsigned int lua_color_detect_scale_min_pixels(const lua_color_detect_config_t *config, int detect_width, int detect_height)
{
    int64_t source_pixels = (int64_t)config->source_width * config->source_height;
    int64_t detect_pixels = (int64_t)detect_width * detect_height;
    int64_t scaled = ((int64_t)config->min_pixels * detect_pixels + source_pixels - 1) / source_pixels;
    return (unsigned int)lua_color_detect_clamp((int)scaled, 1, (int)detect_pixels - 1);
}

static int lua_color_detect_map_floor(int value, int source_size, int detect_size)
{
    return (int)((int64_t)value * source_size / detect_size);
}

static int lua_color_detect_map_ceil(int value, int source_size, int detect_size)
{
    return (int)(((int64_t)value * source_size + detect_size - 1) / detect_size);
}

static bool lua_color_detect_select_best(const lua_color_detect_config_t *config, int detect_width, int detect_height, size_t result_count,
                                         lua_color_detect_result_t *out)
{
    bool detected = false;

    if (result_count > LUA_COLOR_DETECT_RESULT_CAPACITY) {
        ESP_LOGW(TAG, "blob results truncated: total=%u capacity=%u", (unsigned)result_count, LUA_COLOR_DETECT_RESULT_CAPACITY);
        result_count = LUA_COLOR_DETECT_RESULT_CAPACITY;
    }
    for (size_t i = 0; i < result_count; i++) {
        const esp_vision_rect_t *rect = &s_blobs[i].rect;
        if (rect->x < 0 || rect->y < 0 || rect->w <= 0 || rect->h <= 0 || rect->x + rect->w > detect_width || rect->y + rect->h > detect_height) {
            ESP_LOGW(TAG, "invalid blob rect skipped");
            continue;
        }
        int left = config->source_x + lua_color_detect_map_floor(rect->x, config->source_width, detect_width);
        int top = config->source_y + lua_color_detect_map_floor(rect->y, config->source_height, detect_height);
        int right = config->source_x + lua_color_detect_map_ceil(rect->x + rect->w, config->source_width, detect_width) - 1;
        int bottom = config->source_y + lua_color_detect_map_ceil(rect->y + rect->h, config->source_height, detect_height) - 1;
        right = lua_color_detect_clamp(right, left, config->source_x + config->source_width - 1);
        bottom = lua_color_detect_clamp(bottom, top, config->source_y + config->source_height - 1);
        int area = (right - left + 1) * (bottom - top + 1);
        if (area <= 0 || area > config->max_blob_pixels || (detected && area <= out->area)) {
            continue;
        }
        *out = (lua_color_detect_result_t) {
            .left = left,
            .top = top,
            .right = right,
            .bottom = bottom,
            .area = area,
        };
        detected = true;
    }
    return detected;
}

static void lua_color_detect_push_common(lua_State *L, const lua_color_detect_config_t *config, int frame_width, int frame_height, bool detected)
{
    lua_newtable(L);
    lua_pushinteger(L, detected ? 1 : 0);
    lua_setfield(L, -2, "count");
    lua_pushboolean(L, detected);
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

static void lua_color_detect_push_result(lua_State *L, const lua_color_detect_config_t *config, const lua_color_detect_result_t *result, int frame_width, int frame_height)
{
    int box_width = result->right - result->left + 1;
    int box_height = result->bottom - result->top + 1;

    lua_color_detect_push_common(L, config, frame_width, frame_height, true);
    lua_pushinteger(L, result->area);
    lua_setfield(L, -2, "pixels");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "category");
    lua_pushnumber(L, 1.0);
    lua_setfield(L, -2, "score");
    lua_pushinteger(L, result->left);
    lua_setfield(L, -2, "left");
    lua_pushinteger(L, result->top);
    lua_setfield(L, -2, "top");
    lua_pushinteger(L, result->right);
    lua_setfield(L, -2, "right");
    lua_pushinteger(L, result->bottom);
    lua_setfield(L, -2, "bottom");
    lua_pushinteger(L, result->left);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, result->top);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, box_width);
    lua_setfield(L, -2, "box_width");
    lua_pushinteger(L, box_height);
    lua_setfield(L, -2, "box_height");
    lua_pushnumber(L, ((lua_Number)result->left + result->right) * 0.5);
    lua_setfield(L, -2, "cx");
    lua_pushnumber(L, ((lua_Number)result->top + result->bottom) * 0.5);
    lua_setfield(L, -2, "cy");

    lua_createtable(L, 4, 0);
    lua_pushinteger(L, result->left);
    lua_rawseti(L, -2, 1);
    lua_pushinteger(L, result->top);
    lua_rawseti(L, -2, 2);
    lua_pushinteger(L, result->right);
    lua_rawseti(L, -2, 3);
    lua_pushinteger(L, result->bottom);
    lua_rawseti(L, -2, 4);
    lua_setfield(L, -2, "box");
}

static int lua_color_detect_detect(lua_State *L)
{
    lua_image_view_t view = {0};
    lua_color_detect_config_t config = {0};
    lua_color_detect_result_t best = {0};
    size_t result_count = 0;
    bool detected = false;
    int detect_width = 0;
    int detect_height = 0;

    esp_err_t err = lua_image_require_format(L, 1, LUA_IMAGE_FORMAT_RGB565LE, &view);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame conversion failed: %s", esp_err_to_name(err));
        return luaL_error(L, "color_detect unsupported frame: %s", esp_err_to_name(err));
    }
    err = lua_color_detect_parse_config(L, lua_istable(L, 2) ? 2 : 0, view.width, view.height, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "invalid detection options");
        lua_image_release_view(&view);
        return luaL_error(L, "invalid color_detect options");
    }
    err = lua_vision_core_lock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "core lock failed: %s", esp_err_to_name(err));
        lua_image_release_view(&view);
        return luaL_error(L, "color_detect lock failed");
    }
    err = lua_color_detect_init_locked();
    if (err == ESP_OK) {
        detect_width = config.source_width < LUA_COLOR_DETECT_SIZE ? config.source_width : LUA_COLOR_DETECT_SIZE;
        detect_height = config.source_height < LUA_COLOR_DETECT_SIZE ? config.source_height : LUA_COLOR_DETECT_SIZE;
        lua_color_detect_resize_roi(&view, &config, detect_width, detect_height);
        lua_color_detect_update_thresholds_locked(&config);
        unsigned int min_pixels = lua_color_detect_scale_min_pixels(&config, detect_width, detect_height);
        esp_vision_image_t image = {
            .width = detect_width,
            .height = detect_height,
            .pixformat = ESP_VISION_PIXFORMAT_RGB565,
            .data = (uint8_t *)s_detect_buffer,
            .size = (size_t)detect_width * detect_height * sizeof(*s_detect_buffer),
        };
        esp_vision_rect_t roi = {
            .x = 0,
            .y = 0,
            .w = (int16_t)detect_width,
            .h = (int16_t)detect_height,
        };
        esp_vision_find_blobs_config_t find_config = {
            .thresholds = s_thresholds,
            .threshold_count = s_threshold_count,
            .x_stride = (unsigned int)config.x_stride,
            .y_stride = (unsigned int)config.y_stride,
            .area_threshold = min_pixels,
            .pixels_threshold = min_pixels,
            .merge = true,
        };
        err = esp_vision_image_find_blobs(&image, &roi, &find_config, s_blobs, LUA_COLOR_DETECT_RESULT_CAPACITY, &result_count);
        if (err == ESP_OK || err == ESP_ERR_INVALID_SIZE) {
            detected = lua_color_detect_select_best(&config, detect_width, detect_height, result_count, &best);
        }
    }
    lua_vision_core_unlock();

    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        ESP_LOGE(TAG, "blob detection failed: %s", esp_err_to_name(err));
        lua_image_release_view(&view);
        return luaL_error(L, "color_detect failed: %s", esp_err_to_name(err));
    }
    if (detected) {
        lua_color_detect_push_result(L, &config, &best, view.width, view.height);
    } else {
        lua_color_detect_push_common(L, &config, view.width, view.height, false);
    }
    lua_image_release_view(&view);
    return 1;
}

static int lua_color_detect_release(lua_State *L)
{
    (void)L;
    esp_err_t err = lua_vision_core_lock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "release lock failed: %s", esp_err_to_name(err));
        return 0;
    }
    lua_color_detect_release_locked();
    lua_vision_core_unlock();
    return 0;
}

int luaopen_color_detect(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"detect", lua_color_detect_detect},
        {"release", lua_color_detect_release},
        {NULL, NULL},
    };
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}
