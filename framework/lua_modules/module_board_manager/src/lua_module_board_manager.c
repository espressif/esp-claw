/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_board_manager.h"

#include <string.h>
#include "esp_err.h"
#include "lauxlib.h"
#include "esp_board_manager_includes.h"
#include "cap_lua.h"

#define LUA_BM_DISPLAY_PANEL_IF_IO        0
#define LUA_BM_DISPLAY_PANEL_IF_RGB       1
#define LUA_BM_DISPLAY_PANEL_IF_MIPI_DSI  2

/** Push nil + error message string, return 2. Convenience for error paths. */
static int push_err(lua_State *L, esp_err_t err, const char *msg)
{
    lua_pushnil(L);
    if (msg) {
        lua_pushfstring(L, "%s: %s", msg, esp_err_to_name(err));
    } else {
        lua_pushstring(L, esp_err_to_name(err));
    }
    return 2;
}

static esp_err_t lua_module_board_manager_get_camera_paths(const char **dev_path,
                                                           const char **meta_path)
{
#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
    dev_camera_handle_t *camera_handle = NULL;
    esp_err_t err;

    if (dev_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *dev_path = NULL;
    if (meta_path != NULL) {
        *meta_path = NULL;
    }

    err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_CAMERA,
                                                     (void **)&camera_handle);
    if (err != ESP_OK) {
        return err;
    }
    if (camera_handle == NULL || camera_handle->dev_path == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    *dev_path = camera_handle->dev_path;
    if (meta_path != NULL) {
        *meta_path = camera_handle->meta_path;
    }
    return ESP_OK;
#else
    if (dev_path != NULL) {
        *dev_path = NULL;
    }
    if (meta_path != NULL) {
        *meta_path = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* --------------------------------------------------------------------------
 * board_manager.get_board_info() -> table
 *
 * Returns a table:
 *   { name, chip, version, description, manufacturer }
 * -------------------------------------------------------------------------- */
static int lua_bm_get_board_info(lua_State *L)
{
    esp_board_info_t info = {0};
    esp_err_t err = esp_board_manager_get_board_info(&info);
    if (err != ESP_OK) {
        return push_err(L, err, "get_board_info");
    }
    lua_newtable(L);
    lua_pushstring(L, info.name         ? info.name         : "");
    lua_setfield(L, -2, "name");
    lua_pushstring(L, info.chip         ? info.chip         : "");
    lua_setfield(L, -2, "chip");
    lua_pushstring(L, info.version      ? info.version      : "");
    lua_setfield(L, -2, "version");
    lua_pushstring(L, info.description  ? info.description  : "");
    lua_setfield(L, -2, "description");
    lua_pushstring(L, info.manufacturer ? info.manufacturer : "");
    lua_setfield(L, -2, "manufacturer");
    return 1;
}

/* --------------------------------------------------------------------------
 * board_manager.init_device(name) -> true | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_bm_init_device(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    esp_err_t err = esp_board_manager_init_device_by_name(name);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* --------------------------------------------------------------------------
 * board_manager.deinit_device(name) -> true | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_bm_deinit_device(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    esp_err_t err = esp_board_manager_deinit_device_by_name(name);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* --------------------------------------------------------------------------
 * board_manager.get_device_handle(name) -> lightuserdata | nil, errmsg
 *
 * Returns the raw device handle as a lightuserdata pointer.
 * Useful when another C-backed Lua module needs to wrap it.
 * -------------------------------------------------------------------------- */
static int lua_bm_get_device_handle(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    void *handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(name, &handle);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }
    if (handle == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "device handle is NULL");
        return 2;
    }
    lua_pushlightuserdata(L, handle);
    return 1;
}

/* --------------------------------------------------------------------------
 * board_manager.get_device_config_handle(name) -> lightuserdata | nil, errmsg
 *
 * Returns the raw device config pointer as lightuserdata.
 * Useful for passing board-manager-generated config structs into other modules.
 * -------------------------------------------------------------------------- */
static int lua_bm_get_device_config_handle(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    void *config = NULL;
    esp_err_t err = esp_board_manager_get_device_config(name, &config);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }
    if (config == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "device config is NULL");
        return 2;
    }
    lua_pushlightuserdata(L, config);
    return 1;
}

/* --------------------------------------------------------------------------
 * board_manager.get_display_lcd_params(name)
 *   -> panel_handle, io_handle, lcd_width, lcd_height, panel_if | nil, errmsg
 * -------------------------------------------------------------------------- */
static int lua_bm_get_display_lcd_params(lua_State *L)
{
#ifdef CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    const char *name = luaL_checkstring(L, 1);
    void *handle = NULL;
    void *config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(name, &handle);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }
    err = esp_board_manager_get_device_config(name, &config);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)handle;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)config;
    if (lcd_handles == NULL || lcd_cfg == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "display_lcd '%s' handle/config is NULL", name);
        return 2;
    }
    if (lcd_handles->panel_handle == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "display_lcd '%s' panel handle is NULL", name);
        return 2;
    }

    lua_pushlightuserdata(L, lcd_handles->panel_handle);
    if (lcd_handles->io_handle != NULL) {
        lua_pushlightuserdata(L, lcd_handles->io_handle);
    } else {
        lua_pushnil(L);
    }
    const char *sub_type = lcd_cfg->sub_type;
    int panel_if = LUA_BM_DISPLAY_PANEL_IF_IO;

    if (sub_type != NULL) {
        if (strcmp(sub_type, "dsi") == 0 || strcmp(sub_type, "mipi_dsi") == 0) {
            panel_if = LUA_BM_DISPLAY_PANEL_IF_MIPI_DSI;
        } else if (strcmp(sub_type, "rgb") == 0) {
            panel_if = LUA_BM_DISPLAY_PANEL_IF_RGB;
        }
    }

    lua_pushinteger(L, lcd_cfg->lcd_width);
    lua_pushinteger(L, lcd_cfg->lcd_height);
    lua_pushinteger(L, panel_if);
    return 5;
#else
    lua_pushnil(L);
    lua_pushstring(L, "display lcd support is disabled");
    return 2;
#endif
}

/* --------------------------------------------------------------------------
 * board_manager.get_lcd_touch_handle(name) -> lightuserdata | nil, errmsg
 *
 * Returns the raw esp_lcd_touch_handle_t from the board-manager wrapper handle.
 * -------------------------------------------------------------------------- */
static int lua_bm_get_lcd_touch_handle(lua_State *L)
{
#if defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT) && defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUB_I2C_SUPPORT)
    const char *name = luaL_checkstring(L, 1);
    void *handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(name, &handle);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }

    dev_lcd_touch_handles_t *touch_handles = (dev_lcd_touch_handles_t *)handle;
    if (touch_handles == NULL || touch_handles->touch_handle == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "lcd_touch '%s' handle is NULL", name);
        return 2;
    }

    lua_pushlightuserdata(L, touch_handles->touch_handle);
    return 1;
#else
    lua_pushnil(L);
    lua_pushstring(L, "lcd touch support is disabled");
    return 2;
#endif
}

#ifdef CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
static esp_err_t lua_bm_get_i2s_audio_format(const periph_i2s_config_t *i2s_cfg,
                                             uint32_t *sample_rate,
                                             uint8_t *channels,
                                             uint8_t *bits_per_sample)
{
    if (i2s_cfg == NULL || sample_rate == NULL || channels == NULL || bits_per_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (i2s_cfg->mode == I2S_COMM_MODE_STD) {
        *sample_rate = i2s_cfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        *channels = (i2s_cfg->i2s_cfg.std.slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
        *bits_per_sample = (uint8_t)i2s_cfg->i2s_cfg.std.slot_cfg.data_bit_width;
        return ESP_OK;
    }

#if CONFIG_SOC_I2S_SUPPORTS_TDM
    if (i2s_cfg->mode == I2S_COMM_MODE_TDM) {
        *sample_rate = i2s_cfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz;
        *channels = (uint8_t)i2s_cfg->i2s_cfg.tdm.slot_cfg.total_slot;
        *bits_per_sample = (uint8_t)i2s_cfg->i2s_cfg.tdm.slot_cfg.data_bit_width;
        return ESP_OK;
    }
#endif

#if CONFIG_SOC_I2S_SUPPORTS_PDM_TX
    if (i2s_cfg->mode == I2S_COMM_MODE_PDM && (i2s_cfg->direction & I2S_DIR_TX)) {
        *sample_rate = i2s_cfg->i2s_cfg.pdm_tx.clk_cfg.sample_rate_hz;
        *channels = (i2s_cfg->i2s_cfg.pdm_tx.slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
        *bits_per_sample = (uint8_t)i2s_cfg->i2s_cfg.pdm_tx.slot_cfg.data_bit_width;
        return ESP_OK;
    }
#endif

#if CONFIG_SOC_I2S_SUPPORTS_PDM_RX
    if (i2s_cfg->mode == I2S_COMM_MODE_PDM && (i2s_cfg->direction & I2S_DIR_RX)) {
        *sample_rate = i2s_cfg->i2s_cfg.pdm_rx.clk_cfg.sample_rate_hz;
        *channels = (i2s_cfg->i2s_cfg.pdm_rx.slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
        *bits_per_sample = (uint8_t)i2s_cfg->i2s_cfg.pdm_rx.slot_cfg.data_bit_width;
        return ESP_OK;
    }
#endif

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t lua_bm_get_adc_audio_format(const board_codec_adc_t *adc_cfg,
                                             uint32_t *sample_rate,
                                             uint8_t *channels,
                                             uint8_t *bits_per_sample)
{
    if (adc_cfg == NULL || sample_rate == NULL || channels == NULL || bits_per_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc_cfg->sample_rate_hz == 0 || adc_cfg->pattern_num == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *sample_rate = adc_cfg->sample_rate_hz;
    *channels = adc_cfg->pattern_num;
    *bits_per_sample = 16;
    return ESP_OK;
}
#endif // CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT

static int lua_bm_get_audio_codec_common(lua_State *L, bool for_input)
{
#ifdef CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    const char *name = luaL_checkstring(L, 1);
    void *handle = NULL;
    void *config = NULL;
    void *periph_config = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(name, &handle);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }
    err = esp_board_manager_get_device_config(name, &config);
    if (err != ESP_OK) {
        return push_err(L, err, name);
    }

    dev_audio_codec_handles_t *codec_handles = (dev_audio_codec_handles_t *)handle;
    dev_audio_codec_config_t *codec_cfg = (dev_audio_codec_config_t *)config;
    if (codec_handles == NULL || codec_cfg == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "audio_codec '%s' handle/config is NULL", name);
        return 2;
    }
    if (codec_handles->codec_dev == NULL) {
        lua_pushnil(L);
        lua_pushfstring(L, "audio_codec '%s' codec_dev handle is NULL", name);
        return 2;
    }
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bits_per_sample = 0;

    if (codec_cfg->i2s_cfg.name != NULL) {
        err = esp_board_manager_get_periph_config(codec_cfg->i2s_cfg.name, &periph_config);
        if (err != ESP_OK) {
            return push_err(L, err, codec_cfg->i2s_cfg.name);
        }

        periph_i2s_config_t *i2s_cfg = (periph_i2s_config_t *)periph_config;
        err = lua_bm_get_i2s_audio_format(i2s_cfg, &sample_rate, &channels, &bits_per_sample);
        if (err != ESP_OK) {
            return push_err(L, err, "audio codec i2s mode is not supported");
        }
    } else if (for_input && codec_cfg->data_if_type == DEV_AUDIO_CODEC_DATA_IF_TYPE_ADC) {
        err = lua_bm_get_adc_audio_format(&codec_cfg->adc_cfg, &sample_rate, &channels, &bits_per_sample);
        if (err != ESP_OK) {
            return push_err(L, err, "audio codec adc config is invalid");
        }
    } else {
        lua_pushnil(L);
        lua_pushfstring(L, "audio_codec '%s' i2s config name is NULL", name);
        return 2;
    }

    if (for_input) {
        if (!codec_cfg->adc_enabled) {
            lua_pushnil(L);
            lua_pushfstring(L, "audio_codec '%s' does not enable ADC input", name);
            return 2;
        }
        lua_pushlightuserdata(L, codec_handles->codec_dev);
        lua_pushinteger(L, (lua_Integer)sample_rate);
        lua_pushinteger(L, (lua_Integer)channels);
        lua_pushinteger(L, (lua_Integer)bits_per_sample);
        lua_pushnumber(L, (lua_Number)codec_cfg->adc_init_gain);
        return 5;
    }

    if (!codec_cfg->dac_enabled) {
        lua_pushnil(L);
        lua_pushfstring(L, "audio_codec '%s' does not enable DAC output", name);
        return 2;
    }
    lua_pushlightuserdata(L, codec_handles->codec_dev);
    lua_pushinteger(L, (lua_Integer)sample_rate);
    lua_pushinteger(L, (lua_Integer)channels);
    lua_pushinteger(L, (lua_Integer)bits_per_sample);
    return 4;
#else
    (void)for_input;
    lua_pushnil(L);
    lua_pushstring(L, "audio codec support is disabled");
    return 2;
#endif
}

/* --------------------------------------------------------------------------
 * board_manager.get_audio_codec_input_params(name)
 *   -> codec_dev_handle, sample_rate, channels, bits_per_sample, init_gain_db
 * -------------------------------------------------------------------------- */
static int lua_bm_get_audio_codec_input_params(lua_State *L)
{
    return lua_bm_get_audio_codec_common(L, true);
}

/* --------------------------------------------------------------------------
 * board_manager.get_audio_codec_output_params(name)
 *   -> codec_dev_handle, sample_rate, channels, bits_per_sample
 * -------------------------------------------------------------------------- */
static int lua_bm_get_audio_codec_output_params(lua_State *L)
{
    return lua_bm_get_audio_codec_common(L, false);
}

static int lua_bm_get_camera_paths(lua_State *L)
{
    const char *dev_path = NULL;
    const char *meta_path = NULL;
    esp_err_t err = lua_module_board_manager_get_camera_paths(&dev_path, &meta_path);

    if (err != ESP_OK) {
        return push_err(L, err, "get_camera_paths");
    }

    lua_newtable(L);
    lua_pushstring(L, dev_path);
    lua_setfield(L, -2, "dev_path");
    if (meta_path != NULL) {
        lua_pushstring(L, meta_path);
        lua_setfield(L, -2, "meta_path");
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * Module registration
 * -------------------------------------------------------------------------- */
int luaopen_board_manager(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"get_board_info",    lua_bm_get_board_info},
        {"init_device",       lua_bm_init_device},
        {"deinit_device",     lua_bm_deinit_device},
        {"get_device_handle", lua_bm_get_device_handle},
        {"get_device_config_handle", lua_bm_get_device_config_handle},
        {"get_display_lcd_params", lua_bm_get_display_lcd_params},
        {"get_lcd_touch_handle", lua_bm_get_lcd_touch_handle},
        {"get_audio_codec_input_params", lua_bm_get_audio_codec_input_params},
        {"get_audio_codec_output_params", lua_bm_get_audio_codec_output_params},
        {"get_camera_paths",  lua_bm_get_camera_paths},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_pushinteger(L, LUA_BM_DISPLAY_PANEL_IF_IO);
    lua_setfield(L, -2, "PANEL_IF_IO");
    lua_pushinteger(L, LUA_BM_DISPLAY_PANEL_IF_RGB);
    lua_setfield(L, -2, "PANEL_IF_RGB");
    lua_pushinteger(L, LUA_BM_DISPLAY_PANEL_IF_MIPI_DSI);
    lua_setfield(L, -2, "PANEL_IF_MIPI_DSI");
    return 1;
}

esp_err_t lua_module_board_manager_register(void)
{
    return cap_lua_register_module("board_manager", luaopen_board_manager);
}
