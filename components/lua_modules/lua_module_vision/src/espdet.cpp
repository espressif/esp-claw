/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "espdet.h"

#include <list>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <vector>

#include "dl_detect_base.hpp"
#include "dl_detect_espdet_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C" {
#include "lauxlib.h"
#include "lua_image.h"
}

#define LUA_ESPDET_DEFAULT_SCORE_THR 0.6f
#define LUA_ESPDET_DEFAULT_NMS_THR   0.7f

static const char *TAG = "lua_espdet";

class LuaEspDetDetector : public dl::detect::DetectImpl {
public:
    LuaEspDetDetector(const char *model_path,
                      const char *model_name,
                      float score_thr,
                      float nms_thr)
    {
        if (model_name != nullptr && model_name[0] != '\0') {
            m_model = new dl::Model(model_path, model_name, fbs::MODEL_LOCATION_IN_SDCARD);
        } else {
            m_model = new dl::Model(model_path, fbs::MODEL_LOCATION_IN_SDCARD);
        }
        m_model->minimize();
        m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
        m_image_preprocessor->enable_letterbox({114, 114, 114});
        m_postprocessor = new dl::detect::ESPDetPostProcessor(
            m_model, m_image_preprocessor, score_thr, nms_thr, 10, {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
    }
};

typedef struct {
    std::string model_path;
    std::string model_name;
    float score_threshold;
    float nms_threshold;
} lua_espdet_options_t;

static LuaEspDetDetector *s_detector = nullptr;
static std::string s_detector_model_path;
static std::string s_detector_model_name;
static std::string s_default_model_path;
static std::string s_default_model_name;
static StaticSemaphore_t s_detector_mutex_buffer;
static SemaphoreHandle_t s_detector_mutex;

static SemaphoreHandle_t lua_espdet_get_mutex(void)
{
    if (s_detector_mutex == nullptr) {
        s_detector_mutex = xSemaphoreCreateMutexStatic(&s_detector_mutex_buffer);
    }
    return s_detector_mutex;
}

static int lua_espdet_error(lua_State *L, const char *msg, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", msg, esp_err_to_name(err));
    return luaL_error(L, "%s: %s", msg, esp_err_to_name(err));
}

static bool lua_espdet_get_number_field(lua_State *L, int table_idx, const char *name, lua_Number *out)
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

static bool lua_espdet_get_string_field(lua_State *L, int table_idx, const char *name, std::string *out)
{
    bool ok = false;

    lua_getfield(L, table_idx, name);
    if (lua_isstring(L, -1)) {
        out->assign(lua_tostring(L, -1));
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

static int lua_espdet_parse_options(lua_State *L, int opts_idx, lua_espdet_options_t *opts)
{
    lua_Number value = 0;

    opts->model_path = s_default_model_path;
    opts->model_name = s_default_model_name;
    opts->score_threshold = LUA_ESPDET_DEFAULT_SCORE_THR;
    opts->nms_threshold = LUA_ESPDET_DEFAULT_NMS_THR;

    if (opts_idx <= 0 || lua_isnoneornil(L, opts_idx)) {
        return LUA_OK;
    }
    if (!lua_istable(L, opts_idx)) {
        return luaL_error(L, "espdet options must be a table");
    }

    opts_idx = lua_absindex(L, opts_idx);

    lua_espdet_get_string_field(L, opts_idx, "model_path", &opts->model_path);
    lua_espdet_get_string_field(L, opts_idx, "path", &opts->model_path);
    lua_espdet_get_string_field(L, opts_idx, "model_name", &opts->model_name);

    if (lua_espdet_get_number_field(L, opts_idx, "score_threshold", &value) ||
        lua_espdet_get_number_field(L, opts_idx, "score_thr", &value)) {
        if (value < 0.0 || value > 1.0) {
            return luaL_error(L, "espdet score_threshold must be in [0, 1]");
        }
        opts->score_threshold = (float)value;
    }

    if (lua_espdet_get_number_field(L, opts_idx, "nms_threshold", &value) ||
        lua_espdet_get_number_field(L, opts_idx, "nms_thr", &value)) {
        if (value < 0.0 || value > 1.0) {
            return luaL_error(L, "espdet nms_threshold must be in [0, 1]");
        }
        opts->nms_threshold = (float)value;
    }

    return LUA_OK;
}

static esp_err_t lua_espdet_check_model_file(const char *path)
{
    FILE *file;

    if (path == nullptr || path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "failed to open model file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fclose(file);
    return ESP_OK;
}

static esp_err_t lua_espdet_ensure_detector(const lua_espdet_options_t *opts)
{
    esp_err_t err;
    bool changed;

    err = lua_espdet_check_model_file(opts->model_path.c_str());
    if (err != ESP_OK) {
        return err;
    }

    changed = s_detector == nullptr ||
              s_detector_model_path != opts->model_path ||
              s_detector_model_name != opts->model_name;
    if (changed) {
        delete s_detector;
        s_detector = new LuaEspDetDetector(opts->model_path.c_str(),
                                           opts->model_name.empty() ? nullptr : opts->model_name.c_str(),
                                           opts->score_threshold,
                                           opts->nms_threshold);
        s_detector_model_path = opts->model_path;
        s_detector_model_name = opts->model_name;
        if (s_detector == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_detector->set_score_thr(opts->score_threshold);
    s_detector->set_nms_thr(opts->nms_threshold);
    return ESP_OK;
}

static esp_err_t lua_espdet_drop_detector(void)
{
    SemaphoreHandle_t mutex = lua_espdet_get_mutex();

    if (mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    delete s_detector;
    s_detector = nullptr;
    s_detector_model_path.clear();
    s_detector_model_name.clear();

    xSemaphoreGive(mutex);
    return ESP_OK;
}

static void lua_espdet_push_int_vector(lua_State *L, const std::vector<int> &values)
{
    int index = 1;

    lua_createtable(L, (int)values.size(), 0);
    for (int value : values) {
        lua_pushinteger(L, value);
        lua_rawseti(L, -2, index++);
    }
}

static void lua_espdet_set_integer_field(lua_State *L, const char *name, int value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

static void lua_espdet_push_detection(lua_State *L, const dl::detect::result_t &result)
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    lua_newtable(L);
    lua_espdet_set_integer_field(L, "category", result.category);
    lua_pushnumber(L, result.score);
    lua_setfield(L, -2, "score");

    lua_espdet_push_int_vector(L, result.box);
    lua_setfield(L, -2, "box");

    if (result.box.size() >= 4) {
        left = result.box[0];
        top = result.box[1];
        right = result.box[2];
        bottom = result.box[3];

        lua_espdet_set_integer_field(L, "left", left);
        lua_espdet_set_integer_field(L, "top", top);
        lua_espdet_set_integer_field(L, "right", right);
        lua_espdet_set_integer_field(L, "bottom", bottom);
        lua_espdet_set_integer_field(L, "x", left);
        lua_espdet_set_integer_field(L, "y", top);
        lua_espdet_set_integer_field(L, "width", right - left);
        lua_espdet_set_integer_field(L, "height", bottom - top);
    }

    if (!result.keypoint.empty()) {
        lua_espdet_push_int_vector(L, result.keypoint);
        lua_setfield(L, -2, "keypoint");
    }
}

static int lua_espdet_push_results(lua_State *L, const std::list<dl::detect::result_t> &results)
{
    int index = 1;

    lua_createtable(L, (int)results.size(), 1);
    for (const dl::detect::result_t &result : results) {
        lua_espdet_push_detection(L, result);
        lua_rawseti(L, -2, index++);
    }
    lua_pushinteger(L, (lua_Integer)results.size());
    lua_setfield(L, -2, "count");
    return 1;
}

static esp_err_t lua_espdet_validate_image(const uint8_t *data,
                                           size_t bytes,
                                           int width,
                                           int height,
                                           dl::image::pix_type_t pix_type)
{
    size_t bytes_per_pixel = dl::image::get_pix_byte_size(pix_type);

    if (data == nullptr || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width > UINT16_MAX || height > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (bytes_per_pixel == 0 || bytes < (size_t)width * (size_t)height * bytes_per_pixel) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t lua_espdet_run_image_native(const uint8_t *data,
                                             size_t bytes,
                                             int width,
                                             int height,
                                             dl::image::pix_type_t pix_type,
                                             const lua_espdet_options_t *opts,
                                             std::list<dl::detect::result_t> *out_results)
{
    esp_err_t err = lua_espdet_validate_image(data, bytes, width, height, pix_type);
    if (err != ESP_OK) {
        return err;
    }

    SemaphoreHandle_t mutex = lua_espdet_get_mutex();
    if (mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = lua_espdet_ensure_detector(opts);
    if (err != ESP_OK) {
        xSemaphoreGive(mutex);
        return err;
    }

    dl::image::img_t img;
    img.data = const_cast<uint8_t *>(data);
    img.width = (uint16_t)width;
    img.height = (uint16_t)height;
    img.pix_type = pix_type;

    *out_results = s_detector->run(img);
    xSemaphoreGive(mutex);
    return ESP_OK;
}

static int lua_espdet_push_run_error(lua_State *L, esp_err_t err, int width, int height)
{
    if (err == ESP_ERR_INVALID_STATE) {
        return luaL_error(L, "espdet model_path is required; call espdet.load(path) or pass opts.model_path");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return luaL_error(L, "espdet model file not found");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return luaL_error(L, "espdet unsupported input format; use RGB565LE");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return luaL_error(L, "espdet input must be a non-empty image");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return luaL_error(L, "espdet invalid input buffer or dimensions for %dx%d", width, height);
    }
    return lua_espdet_error(L, "espdet run failed", err);
}

static int lua_espdet_detect(lua_State *L)
{
    lua_espdet_options_t opts;
    std::list<dl::detect::result_t> results;

    if (lua_type(L, 1) == LUA_TSTRING) {
        size_t bytes = 0;
        const char *data = luaL_checklstring(L, 1, &bytes);
        int width = (int)luaL_checkinteger(L, 2);
        int height = (int)luaL_checkinteger(L, 3);
        esp_err_t err;

        if (!lua_isnoneornil(L, 4) && !lua_istable(L, 4)) {
            return luaL_error(L, "espdet raw byte input is RGB565LE; pass options as the 4th argument");
        }
        lua_espdet_parse_options(L, 4, &opts);
        err = lua_espdet_run_image_native((const uint8_t *)data,
                                          bytes,
                                          width,
                                          height,
                                          dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
                                          &opts,
                                          &results);
        if (err != ESP_OK) {
            return lua_espdet_push_run_error(L, err, width, height);
        }
        return lua_espdet_push_results(L, results);
    }

    lua_espdet_parse_options(L, 2, &opts);

    lua_image_view_t view = {};
    esp_err_t err = lua_image_require_format(L, 1, LUA_IMAGE_FORMAT_RGB565LE, &view);
    if (err != ESP_OK) {
        return lua_espdet_error(L, "espdet input frame conversion failed", err);
    }

    int width = view.width;
    int height = view.height;
    err = lua_espdet_run_image_native(view.data,
                                      view.bytes,
                                      view.width,
                                      view.height,
                                      dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
                                      &opts,
                                      &results);
    lua_image_release_view(&view);
    if (err != ESP_OK) {
        return lua_espdet_push_run_error(L, err, width, height);
    }
    return lua_espdet_push_results(L, results);
}

static int lua_espdet_load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_espdet_options_t opts;
    esp_err_t err;

    lua_espdet_parse_options(L, 2, &opts);
    opts.model_path = path;

    SemaphoreHandle_t mutex = lua_espdet_get_mutex();
    if (mutex == nullptr) {
        return lua_espdet_error(L, "espdet mutex allocation failed", ESP_ERR_NO_MEM);
    }
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return lua_espdet_error(L, "espdet mutex lock failed", ESP_ERR_TIMEOUT);
    }

    err = lua_espdet_ensure_detector(&opts);
    if (err == ESP_OK) {
        s_default_model_path = opts.model_path;
        s_default_model_name = opts.model_name;
    }
    xSemaphoreGive(mutex);

    if (err != ESP_OK) {
        return lua_espdet_push_run_error(L, err, 0, 0);
    }

    lua_pushboolean(L, true);
    return 1;
}

static int lua_espdet_unload(lua_State *L)
{
    esp_err_t err;

    s_default_model_path.clear();
    s_default_model_name.clear();

    err = lua_espdet_drop_detector();
    if (err != ESP_OK) {
        return lua_espdet_error(L, "espdet unload failed", err);
    }
    return 0;
}

extern "C" int luaopen_espdet(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"load", lua_espdet_load},
        {"unload", lua_espdet_unload},
        {"detect", lua_espdet_detect},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_pushnumber(L, LUA_ESPDET_DEFAULT_SCORE_THR);
    lua_setfield(L, -2, "DEFAULT_SCORE_THRESHOLD");
    lua_pushnumber(L, LUA_ESPDET_DEFAULT_NMS_THR);
    lua_setfield(L, -2, "DEFAULT_NMS_THRESHOLD");
    lua_pushinteger(L, LUA_IMAGE_FORMAT_RGB565LE);
    lua_setfield(L, -2, "RGB565");
    return 1;
}
