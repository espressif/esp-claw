/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_esp_now_priv.h"

#include "cap_lua.h"

const char *TAG = "lua_espnow";

lua_espnow_runtime_t s_espnow_rt = {
    .event_callback_ref = LUA_NOREF,
};

/* ------------------------------------------------------------------ */
/* Runtime lock and error helpers                                     */
/* ------------------------------------------------------------------ */

esp_err_t lua_espnow_runtime_ensure(void)
{
    if (s_espnow_rt.mutex == NULL) {
        s_espnow_rt.mutex = xSemaphoreCreateRecursiveMutex();
        if (s_espnow_rt.mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void lua_espnow_runtime_lock(void)
{
    if (lua_espnow_runtime_ensure() != ESP_OK) {
        return;
    }
    (void)xSemaphoreTakeRecursive(s_espnow_rt.mutex, portMAX_DELAY);
}

void lua_espnow_runtime_unlock(void)
{
    if (s_espnow_rt.mutex) {
        (void)xSemaphoreGiveRecursive(s_espnow_rt.mutex);
    }
}

static const char *lua_espnow_err_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return NULL;
    case ESP_ERR_ESPNOW_NOT_INIT:
        return "espnow_not_init";
    case ESP_ERR_ESPNOW_ARG:
        return "espnow_invalid_arg";
    case ESP_ERR_ESPNOW_NO_MEM:
        return "espnow_no_mem";
    case ESP_ERR_ESPNOW_FULL:
        return "espnow_peer_list_full";
    case ESP_ERR_ESPNOW_NOT_FOUND:
        return "espnow_peer_not_found";
    case ESP_ERR_ESPNOW_EXIST:
        return "espnow_peer_exists";
    case ESP_ERR_ESPNOW_IF:
        return "espnow_interface_mismatch";
    case ESP_ERR_ESPNOW_CHAN:
        return "espnow_channel_mismatch";
    case ESP_ERR_ESPNOW_INTERNAL:
        return "espnow_internal";
    case ESP_ERR_INVALID_STATE:
        return "espnow_invalid_state";
    case ESP_ERR_NO_MEM:
        return "espnow_no_mem";
    default:
        return esp_err_to_name(err);
    }
}

int lua_espnow_push_ok_or_err(lua_State *L, esp_err_t err)
{
    if (err == ESP_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, lua_espnow_err_name(err));
    return 2;
}

/* ------------------------------------------------------------------ */
/* Argument parsing helpers                                           */
/* ------------------------------------------------------------------ */

/* Read a required 6-byte raw-string MAC from the given stack index. */
static void lua_espnow_check_mac(lua_State *L, int index, uint8_t out_mac[ESP_NOW_ETH_ALEN])
{
    size_t len = 0;
    const char *mac = luaL_checklstring(L, index, &len);

    if (len != ESP_NOW_ETH_ALEN) {
        luaL_error(L, "mac must be a 6-byte raw string");
    }
    memcpy(out_mac, mac, ESP_NOW_ETH_ALEN);
}

/* Ensure ESP-NOW has been initialized before a peer/data operation. */
static bool lua_espnow_is_inited(void)
{
    bool inited;

    lua_espnow_runtime_lock();
    inited = s_espnow_rt.inited;
    lua_espnow_runtime_unlock();
    return inited;
}

/*
 * Parse an options table (at stack index 1) into an esp_now_peer_info_t.
 *   { peer_addr = <6 bytes>, channel = <int>, ifidx = <0|1>,
 *     encrypt = <bool>, lmk = <16 bytes> }
 */
static int lua_espnow_parse_peer(lua_State *L, esp_now_peer_info_t *peer)
{
    memset(peer, 0, sizeof(*peer));

    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "peer_addr");
    {
        size_t len = 0;
        const char *addr = luaL_checklstring(L, -1, &len);
        if (len != ESP_NOW_ETH_ALEN) {
            return luaL_error(L, "peer_addr must be a 6-byte raw string");
        }
        memcpy(peer->peer_addr, addr, ESP_NOW_ETH_ALEN);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "channel");
    if (!lua_isnil(L, -1)) {
        lua_Integer channel = luaL_checkinteger(L, -1);
        if (channel < 0 || channel > 14) {
            return luaL_error(L, "channel must be 0..14");
        }
        peer->channel = (uint8_t)channel;
    }
    lua_pop(L, 1);

    peer->ifidx = WIFI_IF_STA;
    lua_getfield(L, 1, "ifidx");
    if (!lua_isnil(L, -1)) {
        lua_Integer ifidx = luaL_checkinteger(L, -1);
        if (ifidx != WIFI_IF_STA && ifidx != WIFI_IF_AP) {
            return luaL_error(L, "ifidx must be esp_now.IFIDX.STA or esp_now.IFIDX.AP");
        }
        peer->ifidx = (wifi_interface_t)ifidx;
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "encrypt");
    if (!lua_isnil(L, -1)) {
        peer->encrypt = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "lmk");
    if (!lua_isnil(L, -1)) {
        size_t len = 0;
        const char *lmk = luaL_checklstring(L, -1, &len);
        if (len != ESP_NOW_KEY_LEN) {
            return luaL_error(L, "lmk must be a %d-byte raw string", ESP_NOW_KEY_LEN);
        }
        memcpy(peer->lmk, lmk, ESP_NOW_KEY_LEN);
        peer->encrypt = true;
    }
    lua_pop(L, 1);

    return 0;
}

static void lua_espnow_push_peer(lua_State *L, const esp_now_peer_info_t *peer)
{
    lua_newtable(L);
    lua_pushlstring(L, (const char *)peer->peer_addr, ESP_NOW_ETH_ALEN);
    lua_setfield(L, -2, "peer_addr");
    lua_pushinteger(L, peer->channel);
    lua_setfield(L, -2, "channel");
    lua_pushinteger(L, peer->ifidx);
    lua_setfield(L, -2, "ifidx");
    lua_pushboolean(L, peer->encrypt);
    lua_setfield(L, -2, "encrypt");
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static int lua_espnow_init(lua_State *L)
{
    esp_err_t err;
    wifi_mode_t mode;

    if (lua_espnow_runtime_ensure() != ESP_OK) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_NO_MEM);
    }

    /* ESP-NOW requires the WiFi stack to be initialized and started. In this
     * firmware wifi_manager brings WiFi up before Lua runs; fail clearly if
     * that is not the case instead of returning an opaque internal error. */
    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_INVALID_STATE);
    }
    (void)mode;

    lua_espnow_runtime_lock();
    if (s_espnow_rt.inited) {
        lua_espnow_runtime_unlock();
        lua_pushboolean(L, 1);
        return 1;
    }
    if (s_event_queue == NULL) {
        s_event_queue = xQueueCreate(LUA_ESPNOW_EVENT_QUEUE_LEN, sizeof(lua_espnow_event_t *));
        if (s_event_queue == NULL) {
            lua_espnow_runtime_unlock();
            return lua_espnow_push_ok_or_err(L, ESP_ERR_NO_MEM);
        }
    }
    lua_espnow_runtime_unlock();

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return lua_espnow_push_ok_or_err(L, err);
    }

    err = esp_now_register_recv_cb(lua_espnow_recv_cb);
    if (err != ESP_OK) {
        esp_now_deinit();
        return lua_espnow_push_ok_or_err(L, err);
    }
    err = esp_now_register_send_cb(lua_espnow_send_cb);
    if (err != ESP_OK) {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        return lua_espnow_push_ok_or_err(L, err);
    }

    lua_espnow_runtime_lock();
    s_espnow_rt.inited = true;
    lua_espnow_runtime_unlock();

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_espnow_deinit(lua_State *L)
{
    if (!lua_espnow_is_inited()) {
        lua_pushboolean(L, 1);
        return 1;
    }

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();

    lua_espnow_runtime_lock();
    s_espnow_rt.inited = false;
    lua_espnow_runtime_unlock();

    lua_espnow_events_clear();

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_espnow_get_version(lua_State *L)
{
    uint32_t version = 0;
    esp_err_t err = esp_now_get_version(&version);

    if (err != ESP_OK) {
        return lua_espnow_push_ok_or_err(L, err);
    }
    lua_pushinteger(L, (lua_Integer)version);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Data path                                                          */
/* ------------------------------------------------------------------ */

static int lua_espnow_send(lua_State *L)
{
    uint8_t mac[ESP_NOW_ETH_ALEN];
    size_t len = 0;
    const char *data;
    esp_err_t err;

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    lua_espnow_check_mac(L, 1, mac);
    data = luaL_checklstring(L, 2, &len);
    if (len == 0 || len > ESP_NOW_MAX_DATA_LEN) {
        return luaL_error(L, "data length must be 1..%d bytes", ESP_NOW_MAX_DATA_LEN);
    }

    err = esp_now_send(mac, (const uint8_t *)data, len);
    return lua_espnow_push_ok_or_err(L, err);
}

/* ------------------------------------------------------------------ */
/* Peer management                                                    */
/* ------------------------------------------------------------------ */

static int lua_espnow_add_peer(lua_State *L)
{
    esp_now_peer_info_t peer;

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    lua_espnow_parse_peer(L, &peer);
    return lua_espnow_push_ok_or_err(L, esp_now_add_peer(&peer));
}

static int lua_espnow_mod_peer(lua_State *L)
{
    esp_now_peer_info_t peer;

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    lua_espnow_parse_peer(L, &peer);
    return lua_espnow_push_ok_or_err(L, esp_now_mod_peer(&peer));
}

static int lua_espnow_del_peer(lua_State *L)
{
    uint8_t mac[ESP_NOW_ETH_ALEN];

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    lua_espnow_check_mac(L, 1, mac);
    return lua_espnow_push_ok_or_err(L, esp_now_del_peer(mac));
}

static int lua_espnow_get_peer(lua_State *L)
{
    uint8_t mac[ESP_NOW_ETH_ALEN];
    esp_now_peer_info_t peer;
    esp_err_t err;

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    lua_espnow_check_mac(L, 1, mac);
    err = esp_now_get_peer(mac, &peer);
    if (err != ESP_OK) {
        return lua_espnow_push_ok_or_err(L, err);
    }
    lua_espnow_push_peer(L, &peer);
    return 1;
}

static int lua_espnow_peer_exists(lua_State *L)
{
    uint8_t mac[ESP_NOW_ETH_ALEN];

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    lua_espnow_check_mac(L, 1, mac);
    lua_pushboolean(L, esp_now_is_peer_exist(mac));
    return 1;
}

static int lua_espnow_get_peer_num(lua_State *L)
{
    esp_now_peer_num_t num = { 0 };
    esp_err_t err;

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    err = esp_now_get_peer_num(&num);
    if (err != ESP_OK) {
        return lua_espnow_push_ok_or_err(L, err);
    }
    lua_newtable(L);
    lua_pushinteger(L, num.total_num);
    lua_setfield(L, -2, "total");
    lua_pushinteger(L, num.encrypt_num);
    lua_setfield(L, -2, "encrypt");
    return 1;
}

static int lua_espnow_fetch_peers(lua_State *L)
{
    esp_now_peer_info_t peer;
    bool from_head = true;
    int count = 0;

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    lua_newtable(L);
    while (esp_now_fetch_peer(from_head, &peer) == ESP_OK) {
        from_head = false;
        lua_espnow_push_peer(L, &peer);
        lua_rawseti(L, -2, ++count);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Security / low power                                               */
/* ------------------------------------------------------------------ */

static int lua_espnow_set_pmk(lua_State *L)
{
    size_t len = 0;
    const char *pmk;

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    pmk = luaL_checklstring(L, 1, &len);
    if (len != ESP_NOW_KEY_LEN) {
        return luaL_error(L, "pmk must be a %d-byte raw string", ESP_NOW_KEY_LEN);
    }
    return lua_espnow_push_ok_or_err(L, esp_now_set_pmk((const uint8_t *)pmk));
}

static int lua_espnow_set_wake_window(lua_State *L)
{
    lua_Integer window = luaL_checkinteger(L, 1);

    if (!lua_espnow_is_inited()) {
        return lua_espnow_push_ok_or_err(L, ESP_ERR_ESPNOW_NOT_INIT);
    }
    if (window < 0 || window > UINT16_MAX) {
        return luaL_error(L, "window must be 0..65535");
    }
    return lua_espnow_push_ok_or_err(L, esp_now_set_wake_window((uint16_t)window));
}

/* ------------------------------------------------------------------ */
/* WiFi radio helpers (channel / MAC)                                 */
/* ------------------------------------------------------------------ */

/*
 * esp_now.set_channel(channel) -> true | nil, err
 *
 * Lock the WiFi radio to a primary channel (1..14) so a peer board can be
 * aligned to the same channel. ESP-NOW frames are exchanged on the current
 * channel, so both ends must match. Requires the WiFi stack to be started.
 * Note: if this device is STA-connected to an AP, the driver keeps the AP's
 * channel and a manual override may not persist.
 */
static int lua_espnow_set_channel(lua_State *L)
{
    lua_Integer channel = luaL_checkinteger(L, 1);

    if (channel < 1 || channel > 14) {
        return luaL_error(L, "channel must be 1..14");
    }
    return lua_espnow_push_ok_or_err(
        L, esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE));
}

/* esp_now.get_channel() -> primary_channel(int) | nil, err */
static int lua_espnow_get_channel(lua_State *L)
{
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_get_channel(&primary, &second);

    if (err != ESP_OK) {
        return lua_espnow_push_ok_or_err(L, err);
    }
    lua_pushinteger(L, (lua_Integer)primary);
    return 1;
}

/*
 * esp_now.get_mac([ifidx]) -> 6-byte raw string | nil, err
 *
 * Return the interface MAC used for ESP-NOW addressing. Defaults to the STA
 * interface, matching the default peer ifidx, so a remote peer can target
 * this value (or learn it from a received frame's src_mac).
 */
static int lua_espnow_get_mac(lua_State *L)
{
    uint8_t mac[ESP_NOW_ETH_ALEN] = { 0 };
    wifi_interface_t ifidx = WIFI_IF_STA;
    esp_err_t err;

    if (!lua_isnoneornil(L, 1)) {
        lua_Integer v = luaL_checkinteger(L, 1);
        if (v != WIFI_IF_STA && v != WIFI_IF_AP) {
            return luaL_error(L, "ifidx must be esp_now.IFIDX.STA or esp_now.IFIDX.AP");
        }
        ifidx = (wifi_interface_t)v;
    }

    err = esp_wifi_get_mac(ifidx, mac);
    if (err != ESP_OK) {
        return lua_espnow_push_ok_or_err(L, err);
    }
    lua_pushlstring(L, (const char *)mac, ESP_NOW_ETH_ALEN);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                      */
/* ------------------------------------------------------------------ */

static int lua_espnow_stats(lua_State *L)
{
    lua_espnow_runtime_lock();
    lua_newtable(L);
    lua_pushboolean(L, s_espnow_rt.inited);
    lua_setfield(L, -2, "inited");
    lua_pushboolean(L, s_event_callback_ref != LUA_NOREF);
    lua_setfield(L, -2, "callback_set");
    lua_pushinteger(L, s_espnow_rt.recv_count);
    lua_setfield(L, -2, "recv_count");
    lua_pushinteger(L, s_espnow_rt.send_count);
    lua_setfield(L, -2, "send_count");
    lua_pushinteger(L, s_event_dropped);
    lua_setfield(L, -2, "event_dropped");
    lua_pushinteger(L, ESP_NOW_MAX_DATA_LEN);
    lua_setfield(L, -2, "max_data_len");
    lua_espnow_runtime_unlock();
    return 1;
}

/* ------------------------------------------------------------------ */
/* Constant tables                                                    */
/* ------------------------------------------------------------------ */

static void lua_espnow_register_constants(lua_State *L)
{
    static const uint8_t broadcast[ESP_NOW_ETH_ALEN] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };

    lua_pushlstring(L, (const char *)broadcast, sizeof(broadcast));
    lua_setfield(L, -2, "BROADCAST_MAC");

    lua_pushinteger(L, ESP_NOW_MAX_DATA_LEN);
    lua_setfield(L, -2, "MAX_DATA_LEN");

    lua_newtable(L);
    lua_pushinteger(L, WIFI_IF_STA);
    lua_setfield(L, -2, "STA");
    lua_pushinteger(L, WIFI_IF_AP);
    lua_setfield(L, -2, "AP");
    lua_setfield(L, -2, "IFIDX");

    lua_newtable(L);
    lua_pushinteger(L, ESP_NOW_SEND_SUCCESS);
    lua_setfield(L, -2, "SUCCESS");
    lua_pushinteger(L, ESP_NOW_SEND_FAIL);
    lua_setfield(L, -2, "FAIL");
    lua_setfield(L, -2, "SEND_STATUS");
}

/* ------------------------------------------------------------------ */
/* Module registration                                                */
/* ------------------------------------------------------------------ */

int luaopen_esp_now(lua_State *L)
{
    if (lua_espnow_runtime_ensure() != ESP_OK) {
        return luaL_error(L, "esp_now runtime init failed");
    }
    lua_newtable(L);
    lua_pushcfunction(L, lua_espnow_init);
    lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_espnow_deinit);
    lua_setfield(L, -2, "deinit");
    lua_pushcfunction(L, lua_espnow_get_version);
    lua_setfield(L, -2, "get_version");
    lua_pushcfunction(L, lua_espnow_send);
    lua_setfield(L, -2, "send");
    lua_pushcfunction(L, lua_espnow_add_peer);
    lua_setfield(L, -2, "add_peer");
    lua_pushcfunction(L, lua_espnow_mod_peer);
    lua_setfield(L, -2, "mod_peer");
    lua_pushcfunction(L, lua_espnow_del_peer);
    lua_setfield(L, -2, "del_peer");
    lua_pushcfunction(L, lua_espnow_get_peer);
    lua_setfield(L, -2, "get_peer");
    lua_pushcfunction(L, lua_espnow_peer_exists);
    lua_setfield(L, -2, "peer_exists");
    lua_pushcfunction(L, lua_espnow_get_peer_num);
    lua_setfield(L, -2, "get_peer_num");
    lua_pushcfunction(L, lua_espnow_fetch_peers);
    lua_setfield(L, -2, "fetch_peers");
    lua_pushcfunction(L, lua_espnow_set_pmk);
    lua_setfield(L, -2, "set_pmk");
    lua_pushcfunction(L, lua_espnow_set_wake_window);
    lua_setfield(L, -2, "set_wake_window");
    lua_pushcfunction(L, lua_espnow_set_channel);
    lua_setfield(L, -2, "set_channel");
    lua_pushcfunction(L, lua_espnow_get_channel);
    lua_setfield(L, -2, "get_channel");
    lua_pushcfunction(L, lua_espnow_get_mac);
    lua_setfield(L, -2, "get_mac");
    lua_pushcfunction(L, lua_espnow_on_event);
    lua_setfield(L, -2, "on_event");
    lua_pushcfunction(L, lua_espnow_process_events);
    lua_setfield(L, -2, "process_events");
    lua_pushcfunction(L, lua_espnow_stats);
    lua_setfield(L, -2, "stats");
    lua_espnow_register_constants(L);
    return 1;
}

esp_err_t lua_module_esp_now_register(void)
{
    return cap_lua_register_module(LUA_MODULE_ESP_NOW_NAME, luaopen_esp_now);
}
