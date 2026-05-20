/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_twai.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_err.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hal/twai_types.h"
#include "lauxlib.h"

#define LUA_DRIVER_TWAI_METATABLE   "twai.node"
#define LUA_DRIVER_TWAI_RX_QUEUE    32
#define LUA_DRIVER_TWAI_DATA_MAX    8     /* Classic CAN: max 8 bytes */

typedef struct {
    uint32_t id;
    uint8_t  data[LUA_DRIVER_TWAI_DATA_MAX];
    uint8_t  len;
    bool     is_extended;
    bool     is_rtr;
} twai_rx_item_t;

typedef struct {
    twai_node_handle_t node;
    QueueHandle_t      rx_queue;
    bool               enabled;
} lua_driver_twai_ud_t;

/* ── ISR receive callback ─────────────────────────────────────────────────── */

static bool twai_on_rx_done(twai_node_handle_t handle,
                             const twai_rx_done_event_data_t *edata,
                             void *user_ctx)
{
    lua_driver_twai_ud_t *ud = (lua_driver_twai_ud_t *)user_ctx;

    uint8_t buf[LUA_DRIVER_TWAI_DATA_MAX];
    twai_frame_t frame = {
        .buffer     = buf,
        .buffer_len = sizeof(buf),
    };
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }

    twai_rx_item_t item = {
        .id          = frame.header.id,
        .is_extended = frame.header.ide,
        .is_rtr      = frame.header.rtr,
        .len         = (uint8_t)(frame.header.dlc > LUA_DRIVER_TWAI_DATA_MAX
                                 ? LUA_DRIVER_TWAI_DATA_MAX : frame.header.dlc),
    };
    memcpy(item.data, buf, item.len);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(ud->rx_queue, &item, &woken);
    return woken == pdTRUE;
}

/* ── Userdata helpers ─────────────────────────────────────────────────────── */

static lua_driver_twai_ud_t *twai_check_ud(lua_State *L, int idx)
{
    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_TWAI_METATABLE);
    if (!ud->node) {
        luaL_error(L, "twai: node already closed");
    }
    return ud;
}

/* ── twai.new(tx_gpio, rx_gpio, bitrate [, opts_table]) ──────────────────── */

static int twai_new(lua_State *L)
{
    int tx_gpio  = (int)luaL_checkinteger(L, 1);
    int rx_gpio  = (int)luaL_checkinteger(L, 2);
    uint32_t bps = (uint32_t)luaL_checkinteger(L, 3);

    bool listen_only = false;
    bool loopback    = false;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "listen_only");
        listen_only = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "loopback");
        loopback = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)lua_newuserdata(L, sizeof(lua_driver_twai_ud_t));
    memset(ud, 0, sizeof(*ud));
    luaL_setmetatable(L, LUA_DRIVER_TWAI_METATABLE);

    ud->rx_queue = xQueueCreate(LUA_DRIVER_TWAI_RX_QUEUE, sizeof(twai_rx_item_t));
    if (!ud->rx_queue) {
        return luaL_error(L, "twai: failed to create RX queue");
    }

    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = (gpio_num_t)tx_gpio,
            .rx = (gpio_num_t)rx_gpio,
            .quanta_clk_out    = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = { .bitrate = bps },
        .tx_queue_depth = 8,
        .fail_retry_cnt = 3,
        .flags = {
            .enable_listen_only = listen_only,
            .enable_loopback    = loopback,
        },
    };

    esp_err_t err = twai_new_node_onchip(&cfg, &ud->node);
    if (err != ESP_OK) {
        vQueueDelete(ud->rx_queue);
        ud->rx_queue = NULL;
        return luaL_error(L, "twai: twai_new_node_onchip failed (%d)", err);
    }

    twai_event_callbacks_t cbs = {
        .on_rx_done = twai_on_rx_done,
    };
    twai_node_register_event_callbacks(ud->node, &cbs, ud);

    return 1;
}

/* ── node:enable() ────────────────────────────────────────────────────────── */

static int twai_enable(lua_State *L)
{
    lua_driver_twai_ud_t *ud = twai_check_ud(L, 1);
    esp_err_t err = twai_node_enable(ud->node);
    if (err != ESP_OK) {
        return luaL_error(L, "twai: enable failed (%d)", err);
    }
    ud->enabled = true;
    return 0;
}

/* ── node:disable() ───────────────────────────────────────────────────────── */

static int twai_disable(lua_State *L)
{
    lua_driver_twai_ud_t *ud = twai_check_ud(L, 1);
    if (ud->enabled) {
        twai_node_disable(ud->node);
        ud->enabled = false;
    }
    return 0;
}

/* ── node:send(id, data [, is_extended [, is_rtr]]) ──────────────────────── */

static int twai_send(lua_State *L)
{
    lua_driver_twai_ud_t *ud = twai_check_ud(L, 1);
    uint32_t    id          = (uint32_t)luaL_checkinteger(L, 2);
    size_t      data_len    = 0;
    const char *data        = luaL_optlstring(L, 3, "", &data_len);
    bool        is_extended = lua_toboolean(L, 4);
    bool        is_rtr      = lua_toboolean(L, 5);

    if (data_len > LUA_DRIVER_TWAI_DATA_MAX) {
        return luaL_error(L, "twai: data too long (max %d bytes)", LUA_DRIVER_TWAI_DATA_MAX);
    }
    if (!ud->enabled) {
        return luaL_error(L, "twai: node not enabled");
    }

    uint8_t buf[LUA_DRIVER_TWAI_DATA_MAX];
    memcpy(buf, data, data_len);

    twai_frame_t frame = {
        .header = {
            .id  = id,
            .dlc = (uint16_t)data_len,
            .ide = is_extended ? 1 : 0,
            .rtr = is_rtr     ? 1 : 0,
        },
        .buffer     = buf,
        .buffer_len = data_len,
    };

    esp_err_t err = twai_node_transmit(ud->node, &frame, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        return luaL_error(L, "twai: transmit failed (%d)", err);
    }
    return 0;
}

/* ── node:receive([timeout_ms]) → {id, data, extended, rtr} | nil ────────── */

static int twai_receive(lua_State *L)
{
    lua_driver_twai_ud_t *ud = twai_check_ud(L, 1);
    lua_Integer timeout_ms = luaL_optinteger(L, 2, 0);

    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY
                     : (timeout_ms == 0) ? 0
                     : pdMS_TO_TICKS((uint32_t)timeout_ms);

    twai_rx_item_t item;
    if (xQueueReceive(ud->rx_queue, &item, ticks) != pdTRUE) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)item.id);
    lua_setfield(L, -2, "id");
    lua_pushlstring(L, (const char *)item.data, item.len);
    lua_setfield(L, -2, "data");
    lua_pushboolean(L, item.is_extended);
    lua_setfield(L, -2, "extended");
    lua_pushboolean(L, item.is_rtr);
    lua_setfield(L, -2, "rtr");
    return 1;
}

/* ── node:status() → {state, tx_errors, rx_errors, tx_queue_remaining} ───── */

static int twai_status(lua_State *L)
{
    lua_driver_twai_ud_t *ud = twai_check_ud(L, 1);

    twai_node_status_t status = {0};
    twai_node_record_t record = {0};
    esp_err_t err = twai_node_get_info(ud->node, &status, &record);
    if (err != ESP_OK) {
        return luaL_error(L, "twai: get_info failed (%d)", err);
    }

    const char *state_str;
    switch (status.state) {
    case TWAI_ERROR_ACTIVE:   state_str = "active";   break;
    case TWAI_ERROR_WARNING:  state_str = "warning";  break;
    case TWAI_ERROR_PASSIVE:  state_str = "passive";  break;
    case TWAI_ERROR_BUS_OFF:  state_str = "bus_off";  break;
    default:                  state_str = "unknown";  break;
    }

    lua_newtable(L);
    lua_pushstring(L, state_str);
    lua_setfield(L, -2, "state");
    lua_pushinteger(L, status.tx_error_count);
    lua_setfield(L, -2, "tx_errors");
    lua_pushinteger(L, status.rx_error_count);
    lua_setfield(L, -2, "rx_errors");
    lua_pushinteger(L, (lua_Integer)status.tx_queue_remaining);
    lua_setfield(L, -2, "tx_queue_remaining");
    lua_pushinteger(L, (lua_Integer)record.bus_err_num);
    lua_setfield(L, -2, "bus_errors");
    return 1;
}

/* ── node:recover() — trigger bus-off recovery ────────────────────────────── */

static int twai_recover(lua_State *L)
{
    lua_driver_twai_ud_t *ud = twai_check_ud(L, 1);
    esp_err_t err = twai_node_recover(ud->node);
    if (err != ESP_OK) {
        return luaL_error(L, "twai: recover failed (%d)", err);
    }
    return 0;
}

/* ── node:close() ─────────────────────────────────────────────────────────── */

static int twai_close(lua_State *L)
{
    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_TWAI_METATABLE);
    if (!ud->node) {
        return 0;
    }
    if (ud->enabled) {
        twai_node_disable(ud->node);
        ud->enabled = false;
    }
    twai_node_delete(ud->node);
    ud->node = NULL;
    if (ud->rx_queue) {
        vQueueDelete(ud->rx_queue);
        ud->rx_queue = NULL;
    }
    return 0;
}

/* ── __gc ─────────────────────────────────────────────────────────────────── */

static int twai_gc(lua_State *L)
{
    return twai_close(L);
}

/* ── __tostring ───────────────────────────────────────────────────────────── */

static int twai_tostring(lua_State *L)
{
    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_TWAI_METATABLE);
    if (ud->node) {
        lua_pushfstring(L, "twai: enabled=%s", ud->enabled ? "true" : "false");
    } else {
        lua_pushstring(L, "twai: closed");
    }
    return 1;
}

/* ── Module open ──────────────────────────────────────────────────────────── */

static const luaL_Reg twai_methods[] = {
    { "enable",     twai_enable    },
    { "disable",    twai_disable   },
    { "send",       twai_send      },
    { "receive",    twai_receive   },
    { "status",     twai_status    },
    { "recover",    twai_recover   },
    { "close",      twai_close     },
    { "__gc",       twai_gc        },
    { "__tostring", twai_tostring  },
    { NULL, NULL }
};

int luaopen_twai(lua_State *L)
{
    luaL_newmetatable(L, LUA_DRIVER_TWAI_METATABLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, twai_methods, 0);
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, twai_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_twai_register(void)
{
    return cap_lua_register_module("twai", luaopen_twai);
}
