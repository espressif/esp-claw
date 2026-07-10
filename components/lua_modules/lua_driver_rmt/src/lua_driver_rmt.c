/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_rmt.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cap_lua.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lauxlib.h"

#define LUA_DRIVER_RMT_TX_METATABLE "rmt.tx"
#define LUA_DRIVER_RMT_RX_METATABLE "rmt.rx"

#define LUA_DRIVER_RMT_DEFAULT_RESOLUTION_HZ       1000000U
#define LUA_DRIVER_RMT_DEFAULT_TX_MEM_SYMBOLS      64
#define LUA_DRIVER_RMT_DEFAULT_TX_QUEUE_DEPTH      4
#define LUA_DRIVER_RMT_DEFAULT_RX_MEM_SYMBOLS      128
#define LUA_DRIVER_RMT_DEFAULT_RX_MAX_SYMBOLS      256
#define LUA_DRIVER_RMT_DEFAULT_RX_MIN_NS           1250
#define LUA_DRIVER_RMT_DEFAULT_RX_MAX_NS           32000000
#define LUA_DRIVER_RMT_MAX_SYMBOLS                 4096
#define LUA_DRIVER_RMT_DEFAULT_TX_TIMEOUT_MS       5000

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    int gpio_num;
    uint32_t resolution_hz;
    int mem_block_symbols;
    int trans_queue_depth;
    uint32_t carrier_hz;
    float carrier_duty;
    bool enabled;
} lua_driver_rmt_tx_ud_t;

typedef struct {
    rmt_channel_handle_t channel;
    QueueHandle_t queue;
    rmt_symbol_word_t *buffer;
    int gpio_num;
    uint32_t resolution_hz;
    int mem_block_symbols;
    int max_symbols;
    uint32_t signal_range_min_ns;
    uint32_t signal_range_max_ns;
    bool enabled;
} lua_driver_rmt_rx_ud_t;

static bool IRAM_ATTR lua_driver_rmt_rx_done_cb(rmt_channel_handle_t channel,
                                                const rmt_rx_done_event_data_t *edata,
                                                void *user_ctx)
{
    (void)channel;
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_ctx;
    xQueueSendFromISR(queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static lua_driver_rmt_tx_ud_t *lua_driver_rmt_tx_get_ud(lua_State *L, int idx)
{
    lua_driver_rmt_tx_ud_t *ud =
        (lua_driver_rmt_tx_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_RMT_TX_METATABLE);
    if (!ud || !ud->channel) {
        luaL_error(L, "rmt tx: invalid or closed handle");
    }
    return ud;
}

static lua_driver_rmt_rx_ud_t *lua_driver_rmt_rx_get_ud(lua_State *L, int idx)
{
    lua_driver_rmt_rx_ud_t *ud =
        (lua_driver_rmt_rx_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_RMT_RX_METATABLE);
    if (!ud || !ud->channel) {
        luaL_error(L, "rmt rx: invalid or closed handle");
    }
    return ud;
}

static int lua_driver_rmt_table_opt_int(lua_State *L, int idx, const char *key, int def)
{
    lua_getfield(L, idx, key);
    int value = lua_isnil(L, -1) ? def : (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

static uint32_t lua_driver_rmt_table_opt_u32(lua_State *L, int idx, const char *key, uint32_t def)
{
    lua_getfield(L, idx, key);
    uint32_t value = lua_isnil(L, -1) ? def : (uint32_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

static float lua_driver_rmt_table_opt_float(lua_State *L, int idx, const char *key, float def)
{
    lua_getfield(L, idx, key);
    float value = lua_isnil(L, -1) ? def : (float)luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return value;
}

static int lua_driver_rmt_table_req_int(lua_State *L, int idx, const char *key)
{
    lua_getfield(L, idx, key);
    if (lua_isnil(L, -1)) {
        luaL_error(L, "rmt option '%s' is required", key);
    }
    int value = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

static int lua_driver_rmt_timeout_ms(lua_State *L, int idx, int default_ms)
{
    lua_Integer ms = luaL_optinteger(L, idx, default_ms);
    if (ms < 0) {
        luaL_error(L, "rmt timeout must be >= 0");
    }
    if (ms > INT32_MAX) {
        luaL_error(L, "rmt timeout is too large");
    }
    return (int)ms;
}

static TickType_t lua_driver_rmt_timeout_ticks(lua_State *L, int idx, int default_ms)
{
    int ms = lua_driver_rmt_timeout_ms(L, idx, default_ms);
    return ms == 0 ? 0 : pdMS_TO_TICKS((TickType_t)ms);
}

static rmt_symbol_word_t lua_driver_rmt_read_symbol(lua_State *L, int idx, lua_Integer symbol_idx)
{
    rmt_symbol_word_t symbol = { 0 };

    if (!lua_istable(L, idx)) {
        luaL_error(L, "rmt symbol[%d] must be a table", (int)symbol_idx);
    }

    lua_getfield(L, idx, "level0");
    lua_Integer level0 = luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "duration0");
    lua_Integer duration0 = luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "level1");
    lua_Integer level1 = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    lua_getfield(L, idx, "duration1");
    lua_Integer duration1 = luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    if ((level0 != 0 && level0 != 1) || (level1 != 0 && level1 != 1)) {
        luaL_error(L, "rmt symbol[%d] levels must be 0 or 1", (int)symbol_idx);
    }
    if (duration0 < 0 || duration0 > 0x7FFF || duration1 < 0 || duration1 > 0x7FFF) {
        luaL_error(L, "rmt symbol[%d] durations must be 0-32767", (int)symbol_idx);
    }

    symbol.level0 = (uint32_t)level0;
    symbol.duration0 = (uint32_t)duration0;
    symbol.level1 = (uint32_t)level1;
    symbol.duration1 = (uint32_t)duration1;
    return symbol;
}

static int lua_driver_rmt_push_symbols(lua_State *L, const rmt_symbol_word_t *symbols, size_t count)
{
    lua_newtable(L);
    for (size_t i = 0; i < count; i++) {
        lua_newtable(L);
        lua_pushinteger(L, symbols[i].level0 ? 1 : 0);
        lua_setfield(L, -2, "level0");
        lua_pushinteger(L, symbols[i].duration0);
        lua_setfield(L, -2, "duration0");
        lua_pushinteger(L, symbols[i].level1 ? 1 : 0);
        lua_setfield(L, -2, "level1");
        lua_pushinteger(L, symbols[i].duration1);
        lua_setfield(L, -2, "duration1");
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
    return 1;
}

static esp_err_t lua_driver_rmt_tx_release(lua_driver_rmt_tx_ud_t *ud)
{
    esp_err_t first_err = ESP_OK;
    esp_err_t err;

    if (!ud) {
        return ESP_OK;
    }
    if (ud->enabled && ud->channel) {
        err = rmt_disable(ud->channel);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->enabled = false;
    }
    if (ud->encoder) {
        err = rmt_del_encoder(ud->encoder);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->encoder = NULL;
    }
    if (ud->channel) {
        err = rmt_del_channel(ud->channel);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->channel = NULL;
    }
    return first_err;
}

static esp_err_t lua_driver_rmt_rx_release(lua_driver_rmt_rx_ud_t *ud)
{
    esp_err_t first_err = ESP_OK;
    esp_err_t err;

    if (!ud) {
        return ESP_OK;
    }
    if (ud->enabled && ud->channel) {
        err = rmt_disable(ud->channel);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->enabled = false;
    }
    if (ud->channel) {
        err = rmt_del_channel(ud->channel);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->channel = NULL;
    }
    if (ud->queue) {
        vQueueDelete(ud->queue);
        ud->queue = NULL;
    }
    if (ud->buffer) {
        free(ud->buffer);
        ud->buffer = NULL;
    }
    return first_err;
}

static int lua_driver_rmt_tx_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    int gpio_num = lua_driver_rmt_table_req_int(L, 1, "gpio");
    uint32_t resolution_hz =
        lua_driver_rmt_table_opt_u32(L, 1, "resolution_hz", LUA_DRIVER_RMT_DEFAULT_RESOLUTION_HZ);
    int mem_block_symbols =
        lua_driver_rmt_table_opt_int(L, 1, "mem_block_symbols", LUA_DRIVER_RMT_DEFAULT_TX_MEM_SYMBOLS);
    int trans_queue_depth =
        lua_driver_rmt_table_opt_int(L, 1, "trans_queue_depth", LUA_DRIVER_RMT_DEFAULT_TX_QUEUE_DEPTH);
    uint32_t carrier_hz = lua_driver_rmt_table_opt_u32(L, 1, "carrier_hz", 0);
    float carrier_duty = lua_driver_rmt_table_opt_float(L, 1, "carrier_duty", 0.33f);

    if (resolution_hz == 0 || mem_block_symbols <= 0 || trans_queue_depth <= 0) {
        return luaL_error(L, "rmt tx: invalid resolution_hz/mem_block_symbols/trans_queue_depth");
    }
    if (carrier_hz > 0 && (carrier_duty <= 0.0f || carrier_duty >= 1.0f)) {
        return luaL_error(L, "rmt tx: carrier_duty must be > 0 and < 1");
    }

    lua_driver_rmt_tx_ud_t *ud = (lua_driver_rmt_tx_ud_t *)lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_driver_rmt_tx_ud_t) {
        .gpio_num = gpio_num,
        .resolution_hz = resolution_hz,
        .mem_block_symbols = mem_block_symbols,
        .trans_queue_depth = trans_queue_depth,
        .carrier_hz = carrier_hz,
        .carrier_duty = carrier_duty,
    };

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution_hz,
        .mem_block_symbols = mem_block_symbols,
        .trans_queue_depth = trans_queue_depth,
        .gpio_num = gpio_num,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &ud->channel);
    if (err != ESP_OK) {
        return luaL_error(L, "rmt tx: rmt_new_tx_channel failed: %s", esp_err_to_name(err));
    }

    if (carrier_hz > 0) {
        rmt_carrier_config_t carrier_cfg = {
            .frequency_hz = carrier_hz,
            .duty_cycle = carrier_duty,
        };
        err = rmt_apply_carrier(ud->channel, &carrier_cfg);
        if (err != ESP_OK) {
            lua_driver_rmt_tx_release(ud);
            return luaL_error(L, "rmt tx: rmt_apply_carrier failed: %s", esp_err_to_name(err));
        }
    }

    rmt_copy_encoder_config_t encoder_cfg = {};
    err = rmt_new_copy_encoder(&encoder_cfg, &ud->encoder);
    if (err != ESP_OK) {
        lua_driver_rmt_tx_release(ud);
        return luaL_error(L, "rmt tx: rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
    }

    err = rmt_enable(ud->channel);
    if (err != ESP_OK) {
        lua_driver_rmt_tx_release(ud);
        return luaL_error(L, "rmt tx: rmt_enable failed: %s", esp_err_to_name(err));
    }
    ud->enabled = true;

    luaL_getmetatable(L, LUA_DRIVER_RMT_TX_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_driver_rmt_rx_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    int gpio_num = lua_driver_rmt_table_req_int(L, 1, "gpio");
    uint32_t resolution_hz =
        lua_driver_rmt_table_opt_u32(L, 1, "resolution_hz", LUA_DRIVER_RMT_DEFAULT_RESOLUTION_HZ);
    int mem_block_symbols =
        lua_driver_rmt_table_opt_int(L, 1, "mem_block_symbols", LUA_DRIVER_RMT_DEFAULT_RX_MEM_SYMBOLS);
    int max_symbols =
        lua_driver_rmt_table_opt_int(L, 1, "max_symbols", LUA_DRIVER_RMT_DEFAULT_RX_MAX_SYMBOLS);
    uint32_t min_ns =
        lua_driver_rmt_table_opt_u32(L, 1, "signal_range_min_ns", LUA_DRIVER_RMT_DEFAULT_RX_MIN_NS);
    uint32_t max_ns =
        lua_driver_rmt_table_opt_u32(L, 1, "signal_range_max_ns", LUA_DRIVER_RMT_DEFAULT_RX_MAX_NS);

    if (resolution_hz == 0 || mem_block_symbols <= 0 ||
        max_symbols <= 0 || max_symbols > LUA_DRIVER_RMT_MAX_SYMBOLS || min_ns >= max_ns) {
        return luaL_error(L, "rmt rx: invalid resolution/range/buffer options");
    }

    lua_driver_rmt_rx_ud_t *ud = (lua_driver_rmt_rx_ud_t *)lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_driver_rmt_rx_ud_t) {
        .gpio_num = gpio_num,
        .resolution_hz = resolution_hz,
        .mem_block_symbols = mem_block_symbols,
        .max_symbols = max_symbols,
        .signal_range_min_ns = min_ns,
        .signal_range_max_ns = max_ns,
    };

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution_hz,
        .mem_block_symbols = mem_block_symbols,
        .gpio_num = gpio_num,
    };
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &ud->channel);
    if (err != ESP_OK) {
        return luaL_error(L, "rmt rx: rmt_new_rx_channel failed: %s", esp_err_to_name(err));
    }

    ud->queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (ud->queue == NULL) {
        lua_driver_rmt_rx_release(ud);
        return luaL_error(L, "rmt rx: out of memory");
    }

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = lua_driver_rmt_rx_done_cb,
    };
    err = rmt_rx_register_event_callbacks(ud->channel, &cbs, ud->queue);
    if (err != ESP_OK) {
        lua_driver_rmt_rx_release(ud);
        return luaL_error(L, "rmt rx: register callback failed: %s", esp_err_to_name(err));
    }

    ud->buffer = calloc((size_t)max_symbols, sizeof(rmt_symbol_word_t));
    if (ud->buffer == NULL) {
        lua_driver_rmt_rx_release(ud);
        return luaL_error(L, "rmt rx: out of memory");
    }

    err = rmt_enable(ud->channel);
    if (err != ESP_OK) {
        lua_driver_rmt_rx_release(ud);
        return luaL_error(L, "rmt rx: rmt_enable failed: %s", esp_err_to_name(err));
    }
    ud->enabled = true;
    (void)xQueueReset(ud->queue);

    luaL_getmetatable(L, LUA_DRIVER_RMT_RX_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_driver_rmt_tx_send(lua_State *L)
{
    lua_driver_rmt_tx_ud_t *ud = lua_driver_rmt_tx_get_ud(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_Integer count = luaL_len(L, 2);
    if (count <= 0 || count > LUA_DRIVER_RMT_MAX_SYMBOLS) {
        return luaL_error(L, "rmt tx: symbol count must be 1-%d", LUA_DRIVER_RMT_MAX_SYMBOLS);
    }

    rmt_symbol_word_t *symbols =
        (rmt_symbol_word_t *)lua_newuserdata(L, (size_t)count * sizeof(rmt_symbol_word_t));

    for (lua_Integer i = 0; i < count; i++) {
        lua_rawgeti(L, 2, i + 1);
        symbols[i] = lua_driver_rmt_read_symbol(L, -1, i + 1);
        lua_pop(L, 1);
    }

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(ud->channel, ud->encoder, symbols,
                                 (size_t)count * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (err != ESP_OK) {
        return luaL_error(L, "rmt tx: rmt_transmit failed: %s", esp_err_to_name(err));
    }

    err = rmt_tx_wait_all_done(ud->channel, lua_driver_rmt_timeout_ms(
                                  L, 3, LUA_DRIVER_RMT_DEFAULT_TX_TIMEOUT_MS));
    if (err != ESP_OK) {
        return luaL_error(L, "rmt tx: wait failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_driver_rmt_rx_receive(lua_State *L)
{
    lua_driver_rmt_rx_ud_t *ud = lua_driver_rmt_rx_get_ud(L, 1);

    rmt_receive_config_t rcv_cfg = {
        .signal_range_min_ns = ud->signal_range_min_ns,
        .signal_range_max_ns = ud->signal_range_max_ns,
    };
    (void)xQueueReset(ud->queue);
    esp_err_t err = rmt_receive(ud->channel, ud->buffer,
                                (size_t)ud->max_symbols * sizeof(rmt_symbol_word_t),
                                &rcv_cfg);
    if (err != ESP_OK) {
        return luaL_error(L, "rmt rx: rmt_receive failed: %s", esp_err_to_name(err));
    }

    TickType_t timeout_ticks = lua_driver_rmt_timeout_ticks(L, 2, 1000);
    rmt_rx_done_event_data_t evt;
    if (xQueueReceive(ud->queue, &evt, timeout_ticks) != pdPASS) {
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }

    return lua_driver_rmt_push_symbols(L, evt.received_symbols, evt.num_symbols);
}

static int lua_driver_rmt_rx_start(lua_State *L)
{
    lua_driver_rmt_rx_ud_t *ud = lua_driver_rmt_rx_get_ud(L, 1);

    rmt_receive_config_t rcv_cfg = {
        .signal_range_min_ns = ud->signal_range_min_ns,
        .signal_range_max_ns = ud->signal_range_max_ns,
    };
    (void)xQueueReset(ud->queue);
    esp_err_t err = rmt_receive(ud->channel, ud->buffer,
                                (size_t)ud->max_symbols * sizeof(rmt_symbol_word_t),
                                &rcv_cfg);
    if (err != ESP_OK) {
        return luaL_error(L, "rmt rx: rmt_receive failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_driver_rmt_rx_read(lua_State *L)
{
    lua_driver_rmt_rx_ud_t *ud = lua_driver_rmt_rx_get_ud(L, 1);
    TickType_t timeout_ticks = lua_driver_rmt_timeout_ticks(L, 2, 1000);

    rmt_rx_done_event_data_t evt;
    if (xQueueReceive(ud->queue, &evt, timeout_ticks) != pdPASS) {
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }

    return lua_driver_rmt_push_symbols(L, evt.received_symbols, evt.num_symbols);
}

static int lua_driver_rmt_tx_info(lua_State *L)
{
    lua_driver_rmt_tx_ud_t *ud = lua_driver_rmt_tx_get_ud(L, 1);
    lua_newtable(L);
    lua_pushstring(L, "tx");
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, ud->gpio_num);
    lua_setfield(L, -2, "gpio");
    lua_pushinteger(L, ud->resolution_hz);
    lua_setfield(L, -2, "resolution_hz");
    lua_pushinteger(L, ud->mem_block_symbols);
    lua_setfield(L, -2, "mem_block_symbols");
    lua_pushinteger(L, ud->trans_queue_depth);
    lua_setfield(L, -2, "trans_queue_depth");
    lua_pushinteger(L, ud->carrier_hz);
    lua_setfield(L, -2, "carrier_hz");
    lua_pushnumber(L, ud->carrier_duty);
    lua_setfield(L, -2, "carrier_duty");
    return 1;
}

static int lua_driver_rmt_rx_info(lua_State *L)
{
    lua_driver_rmt_rx_ud_t *ud = lua_driver_rmt_rx_get_ud(L, 1);
    lua_newtable(L);
    lua_pushstring(L, "rx");
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, ud->gpio_num);
    lua_setfield(L, -2, "gpio");
    lua_pushinteger(L, ud->resolution_hz);
    lua_setfield(L, -2, "resolution_hz");
    lua_pushinteger(L, ud->mem_block_symbols);
    lua_setfield(L, -2, "mem_block_symbols");
    lua_pushinteger(L, ud->max_symbols);
    lua_setfield(L, -2, "max_symbols");
    lua_pushinteger(L, ud->signal_range_min_ns);
    lua_setfield(L, -2, "signal_range_min_ns");
    lua_pushinteger(L, ud->signal_range_max_ns);
    lua_setfield(L, -2, "signal_range_max_ns");
    return 1;
}

static int lua_driver_rmt_tx_close(lua_State *L)
{
    lua_driver_rmt_tx_ud_t *ud =
        (lua_driver_rmt_tx_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_RMT_TX_METATABLE);
    esp_err_t err = lua_driver_rmt_tx_release(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "rmt tx: close failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_rmt_rx_close(lua_State *L)
{
    lua_driver_rmt_rx_ud_t *ud =
        (lua_driver_rmt_rx_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_RMT_RX_METATABLE);
    esp_err_t err = lua_driver_rmt_rx_release(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "rmt rx: close failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_rmt_tx_gc(lua_State *L)
{
    lua_driver_rmt_tx_ud_t *ud =
        (lua_driver_rmt_tx_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_RMT_TX_METATABLE);
    (void)lua_driver_rmt_tx_release(ud);
    return 0;
}

static int lua_driver_rmt_rx_gc(lua_State *L)
{
    lua_driver_rmt_rx_ud_t *ud =
        (lua_driver_rmt_rx_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_RMT_RX_METATABLE);
    (void)lua_driver_rmt_rx_release(ud);
    return 0;
}

int luaopen_rmt(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_RMT_TX_METATABLE)) {
        lua_pushcfunction(L, lua_driver_rmt_tx_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_rmt_tx_send);
        lua_setfield(L, -2, "send");
        lua_pushcfunction(L, lua_driver_rmt_tx_info);
        lua_setfield(L, -2, "info");
        lua_pushcfunction(L, lua_driver_rmt_tx_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, LUA_DRIVER_RMT_RX_METATABLE)) {
        lua_pushcfunction(L, lua_driver_rmt_rx_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_rmt_rx_receive);
        lua_setfield(L, -2, "receive");
        lua_pushcfunction(L, lua_driver_rmt_rx_start);
        lua_setfield(L, -2, "start");
        lua_pushcfunction(L, lua_driver_rmt_rx_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_rmt_rx_info);
        lua_setfield(L, -2, "info");
        lua_pushcfunction(L, lua_driver_rmt_rx_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_rmt_tx_new);
    lua_setfield(L, -2, "tx");
    lua_pushcfunction(L, lua_driver_rmt_rx_new);
    lua_setfield(L, -2, "rx");
    return 1;
}

esp_err_t lua_driver_rmt_register(void)
{
    return cap_lua_register_module("rmt", luaopen_rmt);
}
