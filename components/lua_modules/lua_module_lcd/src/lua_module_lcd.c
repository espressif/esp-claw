/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_lcd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_gc9107.h"
#include "esp_lcd_gc9b71.h"
#include "esp_lcd_gc9d01.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_nt35510.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_st77922.h"
#include "lauxlib.h"

#define LUA_MODULE_LCD_METATABLE "lcd.device"
#define LUA_MODULE_LCD_PANEL_IF_IO 0

typedef enum {
    LUA_LCD_BUS_MODE_SPI = 0,
    LUA_LCD_BUS_MODE_QSPI,
} lua_lcd_bus_mode_t;

typedef enum {
    LUA_LCD_CONTROLLER_GC9107 = 0,
    LUA_LCD_CONTROLLER_GC9B71,
    LUA_LCD_CONTROLLER_GC9D01,
    LUA_LCD_CONTROLLER_NT35510,
    LUA_LCD_CONTROLLER_CO5300,
    LUA_LCD_CONTROLLER_SPD2010,
    LUA_LCD_CONTROLLER_SH8601,
    LUA_LCD_CONTROLLER_ST7789,
    LUA_LCD_CONTROLLER_ST77916,
    LUA_LCD_CONTROLLER_ST77922,
} lua_lcd_controller_id_t;

typedef struct {
    const char *name;
    lua_lcd_controller_id_t id;
    bool supports_qspi;
    unsigned int default_pclk_hz;
    uint32_t default_bits_per_pixel;
    lcd_rgb_element_order_t default_rgb_order;
} lua_lcd_controller_preset_t;

typedef struct {
    int host;
    lua_lcd_bus_mode_t mode;
    int sclk;
    int mosi;
    int data0;
    int data1;
    int data2;
    int data3;
    int max_transfer_sz;
    int cs;
    int dc;
    int spi_mode;
    int trans_queue_depth;
    unsigned int pclk_hz;
    int reset_gpio_num;
    lcd_rgb_element_order_t rgb_order;
    uint32_t bits_per_pixel;
    bool reset_active_high;
    int x_gap;
    int y_gap;
    bool mirror_x;
    bool mirror_y;
    bool swap_xy;
    bool invert_color;
    bool disp_on;
    int width;
    int height;
    const lua_lcd_controller_preset_t *preset;
} lua_lcd_create_config_t;

typedef struct {
    bool initialized;
    bool spi_bus_initialized;
    int host;
    int width;
    int height;
    int panel_if;
    lua_lcd_controller_id_t controller_id;
    lua_lcd_bus_mode_t bus_mode;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} lua_lcd_device_t;

static const lua_lcd_controller_preset_t s_lcd_presets[] = {
    { "gc9107", LUA_LCD_CONTROLLER_GC9107, false, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "gc9b71", LUA_LCD_CONTROLLER_GC9B71, true, 80 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "gc9d01", LUA_LCD_CONTROLLER_GC9D01, false, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "nt35510", LUA_LCD_CONTROLLER_NT35510, false, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "co5300", LUA_LCD_CONTROLLER_CO5300, true, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "spd2010", LUA_LCD_CONTROLLER_SPD2010, true, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "sh8601", LUA_LCD_CONTROLLER_SH8601, true, 80 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "st7789", LUA_LCD_CONTROLLER_ST7789, false, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "st77916", LUA_LCD_CONTROLLER_ST77916, true, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
    { "st77922", LUA_LCD_CONTROLLER_ST77922, true, 40 * 1000 * 1000U, 16, LCD_RGB_ELEMENT_ORDER_RGB },
};

static int lua_lcd_device_delete(lua_State *L);

static lua_lcd_device_t *lua_lcd_check_device(lua_State *L, int index)
{
    return (lua_lcd_device_t *)luaL_checkudata(L, index, LUA_MODULE_LCD_METATABLE);
}

static const char *lua_lcd_controller_name(lua_lcd_controller_id_t id)
{
    for (size_t i = 0; i < sizeof(s_lcd_presets) / sizeof(s_lcd_presets[0]); ++i) {
        if (s_lcd_presets[i].id == id) {
            return s_lcd_presets[i].name;
        }
    }
    return "unknown";
}

static const char *lua_lcd_bus_mode_name(lua_lcd_bus_mode_t mode)
{
    return mode == LUA_LCD_BUS_MODE_QSPI ? "qspi" : "spi";
}

static const lua_lcd_controller_preset_t *lua_lcd_find_preset(const char *name)
{
    for (size_t i = 0; i < sizeof(s_lcd_presets) / sizeof(s_lcd_presets[0]); ++i) {
        if (strcmp(name, s_lcd_presets[i].name) == 0) {
            return &s_lcd_presets[i];
        }
    }
    return NULL;
}

static void lua_lcd_get_required_table(lua_State *L, int index, const char *field)
{
    lua_getfield(L, index, field);
    if (!lua_istable(L, -1)) {
        luaL_error(L, "lcd config.%s table is required", field);
    }
}

static int lua_lcd_get_required_int_field(lua_State *L, int index, const char *field)
{
    int value;

    lua_getfield(L, index, field);
    if (!lua_isnumber(L, -1)) {
        luaL_error(L, "lcd config field '%s' must be an integer", field);
    }
    value = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return value;
}

static int lua_lcd_get_optional_int_field(lua_State *L, int index, const char *field, int default_value)
{
    int value = default_value;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1)) {
            luaL_error(L, "lcd config field '%s' must be an integer", field);
        }
        value = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static bool lua_lcd_get_optional_bool_field(lua_State *L, int index, const char *field, bool default_value)
{
    bool value = default_value;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static const char *lua_lcd_get_optional_string_field(lua_State *L,
                                                     int index,
                                                     const char *field,
                                                     const char *default_value)
{
    const char *value = default_value;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        value = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static lua_lcd_bus_mode_t lua_lcd_parse_bus_mode(lua_State *L, const char *value)
{
    if (strcmp(value, "spi") == 0) {
        return LUA_LCD_BUS_MODE_SPI;
    }
    if (strcmp(value, "qspi") == 0) {
        return LUA_LCD_BUS_MODE_QSPI;
    }
    luaL_error(L, "lcd bus.mode must be 'spi' or 'qspi'");
    return LUA_LCD_BUS_MODE_SPI;
}

static lcd_rgb_element_order_t lua_lcd_parse_color_space(lua_State *L,
                                                         const char *value,
                                                         lcd_rgb_element_order_t default_value)
{
    if (value == NULL) {
        return default_value;
    }
    if (strcmp(value, "rgb") == 0) {
        return LCD_RGB_ELEMENT_ORDER_RGB;
    }
    if (strcmp(value, "bgr") == 0) {
        return LCD_RGB_ELEMENT_ORDER_BGR;
    }
    luaL_error(L, "lcd panel.color_space must be 'rgb' or 'bgr'");
    return default_value;
}

static void lua_lcd_parse_config(lua_State *L, int index, lua_lcd_create_config_t *out_cfg)
{
    const char *controller = NULL;
    const char *bus_mode = NULL;
    const char *color_space = NULL;

    memset(out_cfg, 0, sizeof(*out_cfg));

    luaL_checktype(L, index, LUA_TTABLE);
    lua_getfield(L, index, "controller");
    controller = luaL_checkstring(L, -1);
    out_cfg->preset = lua_lcd_find_preset(controller);
    lua_pop(L, 1);
    if (out_cfg->preset == NULL) {
        luaL_error(L, "lcd controller '%s' is not supported", controller);
    }

    lua_lcd_get_required_table(L, index, "bus");
    bus_mode = lua_lcd_get_optional_string_field(L, -1, "mode", "spi");
    out_cfg->host = lua_lcd_get_required_int_field(L, -1, "host");
    out_cfg->mode = lua_lcd_parse_bus_mode(L, bus_mode);
    out_cfg->sclk = lua_lcd_get_required_int_field(L, -1, "sclk");
    out_cfg->max_transfer_sz = lua_lcd_get_optional_int_field(L, -1, "max_transfer_sz", 0);
    if (out_cfg->mode == LUA_LCD_BUS_MODE_SPI) {
        out_cfg->mosi = lua_lcd_get_required_int_field(L, -1, "mosi");
        out_cfg->data0 = out_cfg->mosi;
        out_cfg->data1 = -1;
        out_cfg->data2 = -1;
        out_cfg->data3 = -1;
    } else {
        if (!out_cfg->preset->supports_qspi) {
            luaL_error(L, "lcd controller '%s' does not support qspi", out_cfg->preset->name);
        }
        out_cfg->data0 = lua_lcd_get_required_int_field(L, -1, "data0");
        out_cfg->data1 = lua_lcd_get_required_int_field(L, -1, "data1");
        out_cfg->data2 = lua_lcd_get_required_int_field(L, -1, "data2");
        out_cfg->data3 = lua_lcd_get_required_int_field(L, -1, "data3");
        out_cfg->mosi = out_cfg->data0;
    }
    lua_pop(L, 1);

    lua_lcd_get_required_table(L, index, "io");
    out_cfg->cs = lua_lcd_get_required_int_field(L, -1, "cs");
    out_cfg->spi_mode = lua_lcd_get_optional_int_field(L, -1, "spi_mode", 0);
    out_cfg->trans_queue_depth = lua_lcd_get_optional_int_field(L, -1, "trans_queue_depth", 10);
    out_cfg->pclk_hz = (unsigned int)lua_lcd_get_optional_int_field(
        L, -1, "pclk_hz", (int)out_cfg->preset->default_pclk_hz);
    if (out_cfg->mode == LUA_LCD_BUS_MODE_SPI) {
        out_cfg->dc = lua_lcd_get_required_int_field(L, -1, "dc");
    } else {
        out_cfg->dc = -1;
    }
    lua_pop(L, 1);

    lua_lcd_get_required_table(L, index, "panel");
    color_space = lua_lcd_get_optional_string_field(L, -1, "color_space", NULL);
    out_cfg->reset_gpio_num = lua_lcd_get_optional_int_field(L, -1, "reset", -1);
    out_cfg->rgb_order = lua_lcd_parse_color_space(L, color_space, out_cfg->preset->default_rgb_order);
    out_cfg->bits_per_pixel = (uint32_t)lua_lcd_get_optional_int_field(
        L, -1, "bits_per_pixel", (int)out_cfg->preset->default_bits_per_pixel);
    out_cfg->reset_active_high = lua_lcd_get_optional_bool_field(L, -1, "reset_active_high", false);
    out_cfg->x_gap = lua_lcd_get_optional_int_field(L, -1, "x_gap", 0);
    out_cfg->y_gap = lua_lcd_get_optional_int_field(L, -1, "y_gap", 0);
    out_cfg->mirror_x = lua_lcd_get_optional_bool_field(L, -1, "mirror_x", false);
    out_cfg->mirror_y = lua_lcd_get_optional_bool_field(L, -1, "mirror_y", false);
    out_cfg->swap_xy = lua_lcd_get_optional_bool_field(L, -1, "swap_xy", false);
    out_cfg->invert_color = lua_lcd_get_optional_bool_field(L, -1, "invert_color", false);
    out_cfg->disp_on = lua_lcd_get_optional_bool_field(L, -1, "disp_on", true);
    lua_pop(L, 1);

    lua_lcd_get_required_table(L, index, "resolution");
    out_cfg->width = lua_lcd_get_required_int_field(L, -1, "width");
    out_cfg->height = lua_lcd_get_required_int_field(L, -1, "height");
    lua_pop(L, 1);

    if (out_cfg->width <= 0 || out_cfg->height <= 0) {
        luaL_error(L, "lcd resolution width and height must be positive");
    }
    if (out_cfg->trans_queue_depth <= 0) {
        luaL_error(L, "lcd io.trans_queue_depth must be positive");
    }
    if (out_cfg->pclk_hz == 0) {
        luaL_error(L, "lcd io.pclk_hz must be positive");
    }
    if (out_cfg->spi_mode < 0 || out_cfg->spi_mode > 3) {
        luaL_error(L, "lcd io.spi_mode must be in range 0-3");
    }
    switch (out_cfg->bits_per_pixel) {
    case 16:
    case 18:
    case 24:
        break;
    default:
        luaL_error(L, "lcd panel.bits_per_pixel must be 16, 18, or 24");
        break;
    }
}

static spi_bus_config_t lua_lcd_make_bus_config(const lua_lcd_create_config_t *cfg)
{
    spi_bus_config_t bus_cfg = { 0 };
    int max_transfer_sz = cfg->max_transfer_sz;

    if (max_transfer_sz <= 0) {
        max_transfer_sz = cfg->width * cfg->height * 2;
    }

    bus_cfg.sclk_io_num = cfg->sclk;
    bus_cfg.max_transfer_sz = max_transfer_sz;
    if (cfg->mode == LUA_LCD_BUS_MODE_QSPI) {
        bus_cfg.data0_io_num = cfg->data0;
        bus_cfg.data1_io_num = cfg->data1;
        bus_cfg.data2_io_num = cfg->data2;
        bus_cfg.data3_io_num = cfg->data3;
    } else {
        bus_cfg.mosi_io_num = cfg->mosi;
        bus_cfg.miso_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.quadwp_io_num = -1;
    }

    return bus_cfg;
}

static esp_lcd_panel_io_spi_config_t lua_lcd_make_io_config(const lua_lcd_create_config_t *cfg)
{
    esp_lcd_panel_io_spi_config_t io_cfg = { 0 };

    io_cfg.cs_gpio_num = cfg->cs;
    io_cfg.dc_gpio_num = cfg->dc;
    io_cfg.spi_mode = cfg->spi_mode;
    io_cfg.pclk_hz = cfg->pclk_hz;
    io_cfg.trans_queue_depth = (size_t)cfg->trans_queue_depth;
    io_cfg.lcd_param_bits = 8;
    if (cfg->mode == LUA_LCD_BUS_MODE_QSPI) {
        io_cfg.lcd_cmd_bits = 32;
        io_cfg.flags.quad_mode = 1;
    } else {
        io_cfg.lcd_cmd_bits = 8;
    }

    return io_cfg;
}

static esp_err_t lua_lcd_create_panel(const lua_lcd_create_config_t *cfg,
                                      esp_lcd_panel_io_handle_t io_handle,
                                      esp_lcd_panel_handle_t *out_panel_handle)
{
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = cfg->reset_gpio_num,
        .rgb_ele_order = cfg->rgb_order,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = cfg->bits_per_pixel,
        .flags.reset_active_high = cfg->reset_active_high,
        .vendor_config = NULL,
    };

    switch (cfg->preset->id) {
    case LUA_LCD_CONTROLLER_GC9107:
        return esp_lcd_new_panel_gc9107(io_handle, &panel_cfg, out_panel_handle);
    case LUA_LCD_CONTROLLER_GC9B71: {
        gc9b71_vendor_config_t vendor_cfg = { 0 };
        vendor_cfg.flags.use_qspi_interface = cfg->mode == LUA_LCD_BUS_MODE_QSPI;
        panel_cfg.vendor_config = &vendor_cfg;
        return esp_lcd_new_panel_gc9b71(io_handle, &panel_cfg, out_panel_handle);
    }
    case LUA_LCD_CONTROLLER_GC9D01:
        return esp_lcd_new_panel_gc9d01(io_handle, &panel_cfg, out_panel_handle);
    case LUA_LCD_CONTROLLER_NT35510:
        return esp_lcd_new_panel_nt35510(io_handle, &panel_cfg, out_panel_handle);
    case LUA_LCD_CONTROLLER_CO5300: {
        co5300_vendor_config_t vendor_cfg = { 0 };
        vendor_cfg.flags.use_qspi_interface = cfg->mode == LUA_LCD_BUS_MODE_QSPI;
        panel_cfg.vendor_config = &vendor_cfg;
        return esp_lcd_new_panel_co5300(io_handle, &panel_cfg, out_panel_handle);
    }
    case LUA_LCD_CONTROLLER_SPD2010: {
        spd2010_vendor_config_t vendor_cfg = { 0 };
        vendor_cfg.flags.use_qspi_interface = cfg->mode == LUA_LCD_BUS_MODE_QSPI;
        panel_cfg.vendor_config = &vendor_cfg;
        return esp_lcd_new_panel_spd2010(io_handle, &panel_cfg, out_panel_handle);
    }
    case LUA_LCD_CONTROLLER_SH8601: {
        sh8601_vendor_config_t vendor_cfg = { 0 };
        vendor_cfg.flags.use_qspi_interface = cfg->mode == LUA_LCD_BUS_MODE_QSPI;
        panel_cfg.vendor_config = &vendor_cfg;
        return esp_lcd_new_panel_sh8601(io_handle, &panel_cfg, out_panel_handle);
    }
    case LUA_LCD_CONTROLLER_ST7789:
        return esp_lcd_new_panel_st7789(io_handle, &panel_cfg, out_panel_handle);
    case LUA_LCD_CONTROLLER_ST77916: {
        st77916_vendor_config_t vendor_cfg = { 0 };
        vendor_cfg.flags.use_qspi_interface = cfg->mode == LUA_LCD_BUS_MODE_QSPI;
        panel_cfg.vendor_config = &vendor_cfg;
        return esp_lcd_new_panel_st77916(io_handle, &panel_cfg, out_panel_handle);
    }
    case LUA_LCD_CONTROLLER_ST77922: {
        st77922_vendor_config_t vendor_cfg = { 0 };
        vendor_cfg.flags.use_qspi_interface = cfg->mode == LUA_LCD_BUS_MODE_QSPI;
        panel_cfg.vendor_config = &vendor_cfg;
        return esp_lcd_new_panel_st77922(io_handle, &panel_cfg, out_panel_handle);
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t lua_lcd_apply_panel_settings(const lua_lcd_create_config_t *cfg,
                                              esp_lcd_panel_handle_t panel_handle)
{
    esp_err_t err;

    err = esp_lcd_panel_set_gap(panel_handle, cfg->x_gap, cfg->y_gap);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_mirror(panel_handle, cfg->mirror_x, cfg->mirror_y);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_swap_xy(panel_handle, cfg->swap_xy);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_invert_color(panel_handle, cfg->invert_color);
    if (err != ESP_OK) {
        return err;
    }
    return esp_lcd_panel_disp_on_off(panel_handle, cfg->disp_on);
}

static esp_err_t lua_lcd_device_destroy(lua_lcd_device_t *dev)
{
    esp_err_t err = ESP_OK;
    esp_err_t tmp_err;

    if (dev->panel_handle != NULL) {
        tmp_err = esp_lcd_panel_del(dev->panel_handle);
        if (err == ESP_OK) {
            err = tmp_err;
        }
        dev->panel_handle = NULL;
    }
    if (dev->io_handle != NULL) {
        tmp_err = esp_lcd_panel_io_del(dev->io_handle);
        if (err == ESP_OK) {
            err = tmp_err;
        }
        dev->io_handle = NULL;
    }
    if (dev->spi_bus_initialized) {
        tmp_err = spi_bus_free((spi_host_device_t)dev->host);
        if (err == ESP_OK) {
            err = tmp_err;
        }
        dev->spi_bus_initialized = false;
    }

    dev->initialized = false;
    return err;
}

static int lua_lcd_device_gc(lua_State *L)
{
    lua_lcd_device_t *dev =
        (lua_lcd_device_t *)luaL_testudata(L, 1, LUA_MODULE_LCD_METATABLE);
    if (dev != NULL) {
        (void)lua_lcd_device_destroy(dev);
    }
    return 0;
}

static int lua_lcd_device_delete(lua_State *L)
{
    lua_lcd_device_t *dev = lua_lcd_check_device(L, 1);
    esp_err_t err = lua_lcd_device_destroy(dev);
    if (err != ESP_OK) {
        return luaL_error(L, "lcd delete failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lcd_device_reset(lua_State *L)
{
    lua_lcd_device_t *dev = lua_lcd_check_device(L, 1);
    esp_err_t err;

    if (!dev->initialized || dev->panel_handle == NULL) {
        return luaL_error(L, "lcd reset failed: device is not initialized");
    }

    err = esp_lcd_panel_reset(dev->panel_handle);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(dev->panel_handle);
    }
    if (err != ESP_OK) {
        return luaL_error(L, "lcd reset failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lcd_device_get_info(lua_State *L)
{
    lua_lcd_device_t *dev = lua_lcd_check_device(L, 1);

    lua_newtable(L);
    lua_pushstring(L, lua_lcd_controller_name(dev->controller_id));
    lua_setfield(L, -2, "controller");
    lua_pushinteger(L, dev->width);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, dev->height);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, dev->panel_if);
    lua_setfield(L, -2, "panel_if");
    lua_pushstring(L, lua_lcd_bus_mode_name(dev->bus_mode));
    lua_setfield(L, -2, "bus_mode");
    lua_pushinteger(L, dev->host);
    lua_setfield(L, -2, "host");
    lua_pushboolean(L, dev->initialized);
    lua_setfield(L, -2, "initialized");
    return 1;
}

static int lua_lcd_new(lua_State *L)
{
    lua_lcd_create_config_t cfg;
    spi_bus_config_t bus_cfg;
    esp_lcd_panel_io_spi_config_t io_cfg;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;
    bool spi_bus_initialized = false;
    esp_err_t err;
    lua_lcd_device_t *dev = NULL;

    lua_lcd_parse_config(L, 1, &cfg);

    bus_cfg = lua_lcd_make_bus_config(&cfg);
    err = spi_bus_initialize((spi_host_device_t)cfg.host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return luaL_error(L, "lcd spi_bus_initialize failed: %s", esp_err_to_name(err));
    }
    spi_bus_initialized = true;

    io_cfg = lua_lcd_make_io_config(&cfg);
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)cfg.host, &io_cfg, &io_handle);
    if (err != ESP_OK) {
        spi_bus_free((spi_host_device_t)cfg.host);
        return luaL_error(L, "lcd new panel io failed: %s", esp_err_to_name(err));
    }

    err = lua_lcd_create_panel(&cfg, io_handle, &panel_handle);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(io_handle);
        spi_bus_free((spi_host_device_t)cfg.host);
        return luaL_error(L, "lcd new panel failed: %s", esp_err_to_name(err));
    }

    err = esp_lcd_panel_reset(panel_handle);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel_handle);
    }
    if (err == ESP_OK) {
        err = lua_lcd_apply_panel_settings(&cfg, panel_handle);
    }
    if (err != ESP_OK) {
        esp_lcd_panel_del(panel_handle);
        esp_lcd_panel_io_del(io_handle);
        spi_bus_free((spi_host_device_t)cfg.host);
        return luaL_error(L, "lcd panel init failed: %s", esp_err_to_name(err));
    }

    dev = (lua_lcd_device_t *)lua_newuserdata(L, sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->initialized = true;
    dev->spi_bus_initialized = spi_bus_initialized;
    dev->host = cfg.host;
    dev->width = cfg.width;
    dev->height = cfg.height;
    dev->panel_if = LUA_MODULE_LCD_PANEL_IF_IO;
    dev->controller_id = cfg.preset->id;
    dev->bus_mode = cfg.mode;
    dev->io_handle = io_handle;
    dev->panel_handle = panel_handle;

    luaL_getmetatable(L, LUA_MODULE_LCD_METATABLE);
    lua_setmetatable(L, -2);

    lua_pushlightuserdata(L, panel_handle);
    lua_pushlightuserdata(L, io_handle);
    lua_pushinteger(L, cfg.width);
    lua_pushinteger(L, cfg.height);
    lua_pushinteger(L, LUA_MODULE_LCD_PANEL_IF_IO);
    return 6;
}

int luaopen_lcd(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_LCD_METATABLE)) {
        lua_pushcfunction(L, lua_lcd_device_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_lcd_device_delete);
        lua_setfield(L, -2, "delete");
        lua_pushcfunction(L, lua_lcd_device_reset);
        lua_setfield(L, -2, "reset");
        lua_pushcfunction(L, lua_lcd_device_get_info);
        lua_setfield(L, -2, "get_info");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_lcd_new);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, lua_lcd_device_delete);
    lua_setfield(L, -2, "delete");
    lua_pushcfunction(L, lua_lcd_device_reset);
    lua_setfield(L, -2, "reset");
    lua_pushcfunction(L, lua_lcd_device_get_info);
    lua_setfield(L, -2, "get_info");
    return 1;
}

esp_err_t lua_module_lcd_register(void)
{
    return cap_lua_register_module("lcd", luaopen_lcd);
}
