/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_ble_priv.h"

#include "esp_heap_caps.h"

typedef int (*lua_ble_gap_event_handler_t)(struct ble_gap_event *event);

typedef struct {
    int type;
    lua_ble_gap_event_handler_t handler;
} lua_ble_gap_event_handler_entry_t;

typedef struct {
    ble_uuid16_t uuid16[LUA_BLE_ADV_UUID16_MAX];
    ble_uuid32_t uuid32[LUA_BLE_ADV_UUID32_MAX];
    ble_uuid128_t uuid128[LUA_BLE_ADV_UUID128_MAX];
    uint8_t uuid16_count;
    uint8_t uuid32_count;
    uint8_t uuid128_count;
    uint8_t mfg_data[LUA_BLE_ADV_MFG_DATA_MAX];
    uint8_t mfg_data_len;
} lua_ble_adv_build_ctx_t;

static int lua_ble_dispatch_gap_event(struct ble_gap_event *event);
static esp_err_t lua_ble_adv_apply(const lua_ble_adv_config_t *config);
static esp_err_t lua_ble_adv_stop_internal(void);

static int lua_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    return lua_ble_dispatch_gap_event(event);
}

void lua_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset reason=%d", reason);
}

void lua_ble_on_sync(void)
{
    int rc;

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
    }
    rc = ble_hs_id_copy_addr(s_own_addr_type, s_own_addr, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_hs_id_copy_addr failed rc=%d", rc);
        memset(s_own_addr, 0, sizeof(s_own_addr));
    }

    s_host_synced = true;
    if (s_host_sync_sem) {
        (void)xSemaphoreGive(s_host_sync_sem);
    }
}

static bool lua_ble_get_bool_field(lua_State *L, int table_index, const char *field, bool default_value)
{
    bool value = default_value;

    lua_getfield(L, table_index, field);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static bool lua_ble_has_non_nil_field(lua_State *L, int table_index, const char *field)
{
    bool has;

    lua_getfield(L, table_index, field);
    has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has;
}

static int lua_ble_ms_to_adv_slots(lua_State *L, int ms)
{
    if (ms < 20 || ms > 10240) {
        return luaL_error(L, "advertising interval must be in 20..10240 ms");
    }
    return (ms * 8) / 5;
}

static int lua_ble_parse_channel_map(lua_State *L, int table_index, struct ble_gap_adv_params *params)
{
    lua_getfield(L, table_index, "channel_map");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *channel_map = lua_tostring(L, -1);

        if (strcmp(channel_map, "all") != 0) {
            return luaL_error(L, "channel_map must be \"all\" or a table of channels 37..39");
        }
        lua_pop(L, 1);
        params->channel_map = 0;
        return 0;
    }
    luaL_checktype(L, -1, LUA_TTABLE);
    params->channel_map = 0;
    for (uint8_t channel = 37; channel <= 39; channel++) {
        lua_geti(L, -1, channel);
        if (lua_toboolean(L, -1)) {
            params->channel_map |= (uint8_t)(1U << (channel - 37));
        }
        lua_pop(L, 1);
    }
    if (params->channel_map == 0) {
        return luaL_error(L, "channel_map table must enable at least one channel");
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_parse_own_addr_type(lua_State *L, int table_index, uint8_t *own_addr_type)
{
    lua_getfield(L, table_index, "addr_mode");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    if (lua_type(L, -1) != LUA_TSTRING) {
        return luaL_error(L, "addr_mode must be a string");
    }

    const char *addr_mode = lua_tostring(L, -1);
    if (strcmp(addr_mode, "public") == 0) {
        *own_addr_type = BLE_OWN_ADDR_PUBLIC;
    } else if (strcmp(addr_mode, "random") == 0) {
        *own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else if (strcmp(addr_mode, "rpa") == 0 || strcmp(addr_mode, "nrpa") == 0) {
        lua_pop(L, 1);
        return lua_ble_push_err_string(L, "ble_unsupported");
    } else {
        return luaL_error(L, "addr_mode must be public or random");
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_adv_parse_service_uuids(lua_State *L, int table_index, lua_ble_adv_build_ctx_t *ctx,
                                           bool add_default)
{
    (void)add_default;

    lua_getfield(L, table_index, "service_uuids");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    luaL_checktype(L, -1, LUA_TTABLE);

    size_t count = lua_rawlen(L, -1);
    for (size_t i = 1; i <= count; i++) {
        ble_uuid_any_t uuid;
        const char *uuid_str;
        int rc;

        lua_rawgeti(L, -1, i);
        uuid_str = luaL_checkstring(L, -1);
        rc = ble_uuid_from_str(&uuid, uuid_str);
        if (rc != 0) {
            return luaL_error(L, "invalid service UUID: %s", uuid_str);
        }
        switch (uuid.u.type) {
        case BLE_UUID_TYPE_16:
            if (ctx->uuid16_count >= LUA_BLE_ADV_UUID16_MAX) {
                return luaL_error(L, "too many 16-bit service UUIDs");
            }
            ctx->uuid16[ctx->uuid16_count++] = uuid.u16;
            break;
        case BLE_UUID_TYPE_32:
            if (ctx->uuid32_count >= LUA_BLE_ADV_UUID32_MAX) {
                return luaL_error(L, "too many 32-bit service UUIDs");
            }
            ctx->uuid32[ctx->uuid32_count++] = uuid.u32;
            break;
        case BLE_UUID_TYPE_128:
            if (ctx->uuid128_count >= LUA_BLE_ADV_UUID128_MAX) {
                return luaL_error(L, "too many 128-bit service UUIDs");
            }
            ctx->uuid128[ctx->uuid128_count++] = uuid.u128;
            break;
        default:
            lua_pop(L, 1);
            return luaL_error(L, "invalid service UUID type");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_adv_apply_name_field(lua_State *L, int table_index, struct ble_hs_adv_fields *fields,
                                        lua_ble_adv_config_t *config, bool is_scan_response)
{
    lua_getfield(L, table_index, "name");
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t name_len;
        const char *name = lua_tolstring(L, -1, &name_len);

        if (name_len > LUA_BLE_ADV_NAME_MAX) {
            return luaL_error(L, "name length must be <= %d", LUA_BLE_ADV_NAME_MAX);
        }
        fields->name = (const uint8_t *)name;
        fields->name_len = name_len;
        fields->name_is_complete = 1;
        if (!is_scan_response) {
            strlcpy(config->name, name, sizeof(config->name));
        }
    } else if (!is_scan_response) {
        fields->name = (const uint8_t *)config->name;
        fields->name_len = strlen(config->name);
        fields->name_is_complete = 1;
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_adv_apply_uuid_fields(lua_State *L, int table_index, struct ble_hs_adv_fields *fields,
                                         lua_ble_adv_build_ctx_t *ctx, bool is_scan_response)
{
    int rc = lua_ble_adv_parse_service_uuids(L, table_index, ctx, !is_scan_response);
    if (rc != 0) {
        return rc;
    }
    if (ctx->uuid16_count > 0) {
        fields->uuids16 = ctx->uuid16;
        fields->num_uuids16 = ctx->uuid16_count;
        fields->uuids16_is_complete = 1;
    }
    if (ctx->uuid32_count > 0) {
        fields->uuids32 = ctx->uuid32;
        fields->num_uuids32 = ctx->uuid32_count;
        fields->uuids32_is_complete = 1;
    }
    if (ctx->uuid128_count > 0) {
        fields->uuids128 = ctx->uuid128;
        fields->num_uuids128 = ctx->uuid128_count;
        fields->uuids128_is_complete = 1;
    }
    return 0;
}

static int lua_ble_adv_apply_manufacturer_data(lua_State *L, int table_index, struct ble_hs_adv_fields *fields,
                                               lua_ble_adv_build_ctx_t *ctx)
{
    lua_getfield(L, table_index, "manufacturer_data");
    if (!lua_isnil(L, -1)) {
        size_t mfg_len;
        const char *mfg = luaL_checklstring(L, -1, &mfg_len);

        if (mfg_len > sizeof(ctx->mfg_data)) {
            lua_pop(L, 1);
            return lua_ble_push_err_string(L, "ble_adv_data_too_long");
        }
        memcpy(ctx->mfg_data, mfg, mfg_len);
        ctx->mfg_data_len = mfg_len;
        fields->mfg_data = ctx->mfg_data;
        fields->mfg_data_len = ctx->mfg_data_len;
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_adv_apply_appearance(lua_State *L, int table_index, struct ble_hs_adv_fields *fields)
{
    lua_getfield(L, table_index, "appearance");
    if (!lua_isnil(L, -1)) {
        int appearance = (int)luaL_checkinteger(L, -1);

        if (appearance < 0 || appearance > UINT16_MAX) {
            return luaL_error(L, "appearance must be in 0..65535");
        }
        fields->appearance = (uint16_t)appearance;
        fields->appearance_is_present = 1;
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_adv_apply_tx_power(lua_State *L, int table_index, struct ble_hs_adv_fields *fields)
{
    lua_getfield(L, table_index, "tx_power");
    if (!lua_isnil(L, -1)) {
        int tx_power = (int)luaL_checkinteger(L, -1);

        if (tx_power < INT8_MIN || tx_power > INT8_MAX) {
            return luaL_error(L, "tx_power must fit in int8");
        }
        fields->tx_pwr_lvl = (int8_t)tx_power;
        fields->tx_pwr_lvl_is_present = 1;
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_adv_build_high_level(lua_State *L, int table_index, bool include_flags,
                                        lua_ble_adv_config_t *config, bool is_scan_response)
{
    struct ble_hs_adv_fields fields;
    lua_ble_adv_build_ctx_t ctx = { 0 };
    uint8_t *dst = is_scan_response ? config->scan_rsp_data : config->adv_data;
    uint8_t *dst_len = is_scan_response ? &config->scan_rsp_len : &config->adv_data_len;
    int rc;

    memset(&fields, 0, sizeof(fields));
    if (include_flags) {
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    }

    rc = lua_ble_adv_apply_name_field(L, table_index, &fields, config, is_scan_response);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_adv_apply_uuid_fields(L, table_index, &fields, &ctx, is_scan_response);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_adv_apply_manufacturer_data(L, table_index, &fields, &ctx);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_adv_apply_appearance(L, table_index, &fields);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_adv_apply_tx_power(L, table_index, &fields);
    if (rc != 0) {
        return rc;
    }

    rc = ble_hs_adv_set_fields(&fields, dst, dst_len, LUA_BLE_ADV_DATA_MAX);
    if (rc != 0) {
        return rc == BLE_HS_EMSGSIZE ? lua_ble_push_err_string(L, "ble_adv_data_too_long") :
                                       lua_ble_push_err_string(L, "ble_resource_busy");
    }
    return 0;
}

static int lua_ble_adv_build_tlv(lua_State *L, int table_index, lua_ble_adv_config_t *config)
{
    size_t count = lua_rawlen(L, table_index);
    uint8_t len = 0;

    for (size_t i = 1; i <= count; i++) {
        int ad_type;
        size_t value_len;
        const char *value;

        lua_rawgeti(L, table_index, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "ad_type");
        ad_type = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        if (ad_type < 0 || ad_type > UINT8_MAX) {
            return luaL_error(L, "data_tlv ad_type must be in 0..255");
        }
        lua_getfield(L, -1, "value");
        value = luaL_checklstring(L, -1, &value_len);
        if (value_len > UINT8_MAX - 1 || len + value_len + 2 > LUA_BLE_ADV_DATA_MAX) {
            lua_pop(L, 2);
            return lua_ble_push_err_string(L, "ble_adv_data_too_long");
        }
        config->adv_data[len++] = (uint8_t)(value_len + 1);
        config->adv_data[len++] = (uint8_t)ad_type;
        memcpy(&config->adv_data[len], value, value_len);
        len += value_len;
        lua_pop(L, 2);
    }
    config->adv_data_len = len;
    return 0;
}

static int lua_ble_parse_directed_addr(lua_State *L, int opts_index, lua_ble_adv_config_t *config)
{
    size_t addr_len;
    const char *addr;

    lua_getfield(L, opts_index, "directed_addr");
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "directed advertising requires directed_addr");
    }
    addr = luaL_checklstring(L, -1, &addr_len);
    if (addr_len != 6) {
        return luaL_error(L, "directed_addr must be a 6-byte raw string");
    }
    memcpy(config->directed_addr.val, addr, 6);
    lua_pop(L, 1);

    config->directed_addr.type = BLE_ADDR_PUBLIC;
    lua_getfield(L, opts_index, "directed_addr_type");
    if (!lua_isnil(L, -1)) {
        const char *type = luaL_checkstring(L, -1);

        if (strcmp(type, "public") == 0) {
            config->directed_addr.type = BLE_ADDR_PUBLIC;
        } else if (strcmp(type, "random") == 0) {
            config->directed_addr.type = BLE_ADDR_RANDOM;
        } else {
            return luaL_error(L, "directed_addr_type must be public or random");
        }
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_adv_parse_basic_opts(lua_State *L, int opts_index, lua_ble_adv_config_t *config,
                                        bool *scannable)
{
    int rc;
    bool connectable;
    bool directed;

    lua_getfield(L, opts_index, "name");
    if (!lua_isnil(L, -1)) {
        size_t name_len;
        const char *name = luaL_checklstring(L, -1, &name_len);

        if (name_len > LUA_BLE_ADV_NAME_MAX) {
            return luaL_error(L, "name length must be <= %d", LUA_BLE_ADV_NAME_MAX);
        }
        strlcpy(config->name, name, sizeof(config->name));
    }
    lua_pop(L, 1);

    rc = lua_ble_parse_own_addr_type(L, opts_index, &config->own_addr_type);
    if (rc != 0) {
        return rc;
    }
    connectable = lua_ble_get_bool_field(L, opts_index, "connectable", true);
    *scannable = lua_ble_get_bool_field(L, opts_index, "scannable", true);
    directed = lua_ble_get_bool_field(L, opts_index, "directed", false);
    if (directed) {
        config->directed = true;
        config->params.conn_mode = BLE_GAP_CONN_MODE_DIR;
        config->params.disc_mode = BLE_GAP_DISC_MODE_NON;
        rc = lua_ble_parse_directed_addr(L, opts_index, config);
        if (rc != 0) {
            return rc;
        }
    } else {
        config->params.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
        config->params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    }

    lua_getfield(L, opts_index, "interval_min_ms");
    if (!lua_isnil(L, -1)) {
        config->params.itvl_min = lua_ble_ms_to_adv_slots(L, (int)luaL_checkinteger(L, -1));
    }
    lua_pop(L, 1);
    lua_getfield(L, opts_index, "interval_max_ms");
    if (!lua_isnil(L, -1)) {
        config->params.itvl_max = lua_ble_ms_to_adv_slots(L, (int)luaL_checkinteger(L, -1));
    }
    lua_pop(L, 1);
    if (config->params.itvl_min != 0 && config->params.itvl_max != 0 &&
        config->params.itvl_min > config->params.itvl_max) {
        return luaL_error(L, "interval_min_ms must be <= interval_max_ms");
    }

    return lua_ble_parse_channel_map(L, opts_index, &config->params);
}

static int lua_ble_adv_build_data_from_opts(lua_State *L, int opts_index, lua_ble_adv_config_t *config)
{
    int rc;

    if (lua_ble_has_non_nil_field(L, opts_index, "data") &&
        lua_ble_has_non_nil_field(L, opts_index, "data_tlv")) {
        return luaL_error(L, "data and data_tlv are mutually exclusive");
    }

    lua_getfield(L, opts_index, "data_tlv");
    if (!lua_isnil(L, -1)) {
        int data_tlv_index = lua_absindex(L, -1);

        luaL_checktype(L, -1, LUA_TTABLE);
        rc = lua_ble_adv_build_tlv(L, data_tlv_index, config);
        if (rc != 0) {
            lua_remove(L, data_tlv_index);
            return rc;
        }
        lua_pop(L, 1);
    } else {
        bool data_is_table;
        int data_index;
        lua_pop(L, 1);
        lua_getfield(L, opts_index, "data");
        data_is_table = lua_istable(L, -1);
        data_index = lua_absindex(L, -1);
        if (data_is_table) {
            rc = lua_ble_adv_build_high_level(L, data_index, true, config, false);
        } else if (!lua_isnil(L, -1)) {
            return luaL_error(L, "data must be a table");
        } else {
            struct ble_hs_adv_fields fields;
            lua_pop(L, 1);
            memset(&fields, 0, sizeof(fields));
            fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
            fields.name = (const uint8_t *)config->name;
            fields.name_len = strlen(config->name);
            fields.name_is_complete = 1;
            rc = ble_hs_adv_set_fields(&fields, config->adv_data, &config->adv_data_len, LUA_BLE_ADV_DATA_MAX);
        }
        if (rc != 0) {
            if (data_is_table) {
                lua_remove(L, data_index);
            }
            if (rc == BLE_HS_EMSGSIZE) {
                return lua_ble_push_err_string(L, "ble_adv_data_too_long");
            }
            if (rc == 2) {
                return rc;
            }
            return lua_ble_push_err_string(L, "ble_resource_busy");
        }
        if (data_is_table) {
            lua_pop(L, 1);
        }
    }
    return 0;
}

static int lua_ble_adv_parse_opts(lua_State *L, int opts_index, lua_ble_adv_config_t *config)
{
    int rc;
    bool scannable = true;

    memset(config, 0, sizeof(*config));
    strlcpy(config->name, s_adv_name, sizeof(config->name));
    config->own_addr_type = s_own_addr_type;
    config->params.conn_mode = BLE_GAP_CONN_MODE_UND;
    config->params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    if (lua_isnoneornil(L, opts_index)) {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.name = (const uint8_t *)config->name;
        fields.name_len = strlen(config->name);
        fields.name_is_complete = 1;
        rc = ble_hs_adv_set_fields(&fields, config->adv_data, &config->adv_data_len, LUA_BLE_ADV_DATA_MAX);
        if (rc != 0) {
            return rc == BLE_HS_EMSGSIZE ? lua_ble_push_err_string(L, "ble_adv_data_too_long") :
                                           lua_ble_push_err_string(L, "ble_resource_busy");
        }
        return 0;
    }

    luaL_checktype(L, opts_index, LUA_TTABLE);
    opts_index = lua_absindex(L, opts_index);
    if (lua_ble_table_has_field(L, opts_index, "mode") || lua_ble_table_has_field(L, opts_index, "set_id") ||
        lua_ble_table_has_field(L, opts_index, "primary_phy") || lua_ble_table_has_field(L, opts_index, "secondary_phy") ||
        lua_ble_table_has_field(L, opts_index, "anonymous") || lua_ble_table_has_field(L, opts_index, "tx_power_included")) {
        return lua_ble_push_err_string(L, "ble_unsupported");
    }

    rc = lua_ble_adv_parse_basic_opts(L, opts_index, config, &scannable);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_adv_build_data_from_opts(L, opts_index, config);
    if (rc != 0) {
        return rc;
    }

    lua_getfield(L, opts_index, "scan_response");
    if (lua_istable(L, -1)) {
        int scan_response_index = lua_absindex(L, -1);

        rc = lua_ble_adv_build_high_level(L, scan_response_index, false, config, true);
        if (rc != 0) {
            lua_remove(L, scan_response_index);
            return rc;
        }
        lua_pop(L, 1);
    } else if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
    } else {
        return luaL_error(L, "scan_response must be a table");
    }
    if (!scannable) {
        config->scan_rsp_len = 0;
    }

    return 0;
}

int lua_ble_set_name(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int rc;

    if (strlen(name) > LUA_BLE_ADV_NAME_MAX) {
        return luaL_error(L, "name length must be <= %d", LUA_BLE_ADV_NAME_MAX);
    }
    if (!s_stack_inited || !s_host_synced) {
        return lua_ble_push_err_string(L, "ble_init_not_initialized");
    }

    rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        return lua_ble_push_err_string(L, "ble_resource_busy");
    }
    ESP_LOGI(TAG, "[gap]: set_name name=%s", name);
    lua_ble_runtime_lock();
    strlcpy(s_adv_name, name, sizeof(s_adv_name));
    lua_ble_runtime_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_ble_set_max_mtu(lua_State *L)
{
    int mtu = (int)luaL_checkinteger(L, 1);
    int rc;

    if (mtu < 23 || mtu > LUA_BLE_MAX_MTU) {
        return lua_ble_push_err_string(L, "ble_mtu_invalid");
    }
    if (!s_stack_inited || !s_host_synced) {
        return lua_ble_push_err_string(L, "ble_init_not_initialized");
    }
    if (lua_ble_has_connection()) {
        return lua_ble_push_err_string(L, "ble_invalid_state");
    }

    rc = ble_att_set_preferred_mtu((uint16_t)mtu);
    if (rc != 0) {
        return lua_ble_push_err_string(L, "ble_resource_busy");
    }
    lua_ble_runtime_lock();
    s_preferred_mtu = (uint16_t)mtu;
    lua_ble_runtime_unlock();
    ESP_LOGI(TAG, "[mtu]: set preferred_mtu=%u", s_preferred_mtu);
    lua_pushboolean(L, 1);
    return 1;
}

int lua_ble_adv_start(lua_State *L)
{
    lua_ble_adv_config_t config;
    esp_err_t err;
    int rc;

    if (!s_stack_inited || !s_host_synced) {
        return lua_ble_push_err_string(L, "ble_init_not_initialized");
    }
    lua_ble_runtime_lock();
    if (s_advertising) {
        lua_ble_runtime_unlock();
        return lua_ble_push_err_string(L, "ble_adv_busy");
    }
    lua_ble_runtime_unlock();

    rc = lua_ble_adv_parse_opts(L, 1, &config);
    if (rc != 0) {
        return rc;
    }

    rc = ble_svc_gap_device_name_set(config.name);
    if (rc != 0) {
        return lua_ble_push_err_string(L, "ble_resource_busy");
    }

    ESP_LOGI(TAG, "[adv]: start name=%s", config.name);
    err = lua_ble_adv_apply(&config);
    if (err != ESP_OK) {
        return lua_ble_push_ok_or_err(L, err);
    }
    lua_ble_runtime_lock();
    strlcpy(s_adv_name, config.name, sizeof(s_adv_name));
    s_adv_config = config;
    s_adv_config.configured = true;
    s_advertising_requested = true;
    s_advertising_paused_for_connection_full = false;
    lua_ble_runtime_unlock();
    lua_ble_event_simple_adv(LUA_BLE_EVENT_ADV_STARTED, "user_start", 0);
    lua_pushboolean(L, 1);
    return 1;
}

int lua_ble_adv_stop(lua_State *L)
{
    bool was_requested_or_active = s_advertising_requested || s_advertising;
    esp_err_t err = lua_ble_adv_stop_internal();

    if (err != ESP_OK) {
        return lua_ble_push_ok_or_err(L, err);
    }
    if (was_requested_or_active) {
        ESP_LOGI(TAG, "[adv]: stop reason=user_stop");
        lua_ble_event_simple_adv(LUA_BLE_EVENT_ADV_STOPPED, "user_stop", 0);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_resolve_conn_index(lua_State *L, int arg, uint8_t *out)
{
    arg = lua_absindex(L, arg);
    if (lua_istable(L, arg)) {
        lua_getfield(L, arg, "conn_index");
        if (!lua_isnil(L, -1)) {
            int idx = (int)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            if (idx < 0 || idx >= LUA_BLE_MAX_CONNECTIONS) {
                return lua_ble_push_err_string(L, "ble_conn_invalid_index");
            }
            lua_ble_runtime_lock();
            if (!s_conns[idx].connected) {
                lua_ble_runtime_unlock();
                return lua_ble_push_err_string(L, "ble_conn_not_found");
            }
            lua_ble_runtime_unlock();
            *out = (uint8_t)idx;
            return 0;
        }
        lua_pop(L, 1);
    }

    int count = 0;
    int found = -1;
    lua_ble_runtime_lock();
    for (uint8_t i = 0; i < LUA_BLE_MAX_CONNECTIONS; i++) {
        if (s_conns[i].connected) {
            count++;
            found = i;
        }
    }
    lua_ble_runtime_unlock();
    if (count == 0) {
        return lua_ble_push_err_string(L, "ble_conn_not_found");
    }
    if (count > 1) {
        return lua_ble_push_err_string(L, "ble_conn_index_required");
    }
    *out = (uint8_t)found;
    return 0;
}

int lua_ble_disconnect(lua_State *L)
{
    uint8_t conn_index = 0;
    uint16_t conn_handle;
    int rc;
    int errc;

    if (!s_stack_inited || !s_host_synced) {
        return lua_ble_push_err_string(L, "ble_init_not_initialized");
    }
    errc = lua_ble_resolve_conn_index(L, 1, &conn_index);
    if (errc != 0) {
        return errc;
    }
    lua_ble_runtime_lock();
    if (!s_conns[conn_index].connected) {
        lua_ble_runtime_unlock();
        return lua_ble_push_err_string(L, "ble_conn_not_found");
    }
    conn_handle = s_conns[conn_index].conn_handle;
    lua_ble_runtime_unlock();
    rc = ble_gap_terminate(conn_handle, BLE_ERR_CONN_TERM_LOCAL);
    if (rc != 0) {
        return lua_ble_push_err_string(L, "ble_resource_busy");
    }
    ESP_LOGI(TAG, "[disconnect]: conn_index=%u", conn_index);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_handle_gap_connect(struct ble_gap_event *event)
{
    int conn_index;
    esp_err_t err;
    bool adv_requested;

    ESP_LOGI(TAG, "gap connect conn_handle=%u status=%d",
             event->connect.conn_handle,
             event->connect.status);
    if (event->connect.status == 0) {
        lua_ble_runtime_lock();
        s_advertising = false;
        lua_ble_runtime_unlock();
        conn_index = lua_ble_connection_set_connected(event->connect.conn_handle);
        if (conn_index < 0) {
            (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
            return 0;
        }
        lua_ble_runtime_lock();
        adv_requested = s_advertising_requested;
        lua_ble_runtime_unlock();
        if (adv_requested) {
            if (lua_ble_connection_count() >= lua_ble_slot_count()) {
                lua_ble_runtime_lock();
                s_advertising_paused_for_connection_full = true;
                lua_ble_runtime_unlock();
                lua_ble_event_simple_adv(LUA_BLE_EVENT_ADV_STOPPED, "connection_full", 0);
            } else {
                lua_ble_adv_config_t adv_config;
                lua_ble_runtime_lock();
                adv_config = s_adv_config;
                lua_ble_runtime_unlock();
                err = lua_ble_adv_apply(&adv_config);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "restart advertising after connect failed: %s", esp_err_to_name(err));
                } else {
                    lua_ble_runtime_lock();
                    s_advertising_paused_for_connection_full = false;
                    lua_ble_runtime_unlock();
                    ESP_LOGI(TAG, "[adv]: continued after connect conn_index=%d", conn_index);
                }
            }
        }
    } else {
        lua_ble_runtime_lock();
        s_advertising = false;
        adv_requested = s_advertising_requested;
        lua_ble_runtime_unlock();
        if (adv_requested) {
            lua_ble_event_simple_adv(LUA_BLE_EVENT_ADV_STOPPED, "error", event->connect.status);
        }
    }
    return 0;
}

static int lua_ble_handle_gap_disconnect(struct ble_gap_event *event)
{
    int rc;
    int conn_index = lua_ble_find_conn_index_by_handle(event->disconnect.conn.conn_handle);
    bool draining;
    bool should_restart_adv = false;
    lua_ble_adv_config_t adv_config;

    ESP_LOGI(TAG, "gap disconnect conn_handle=%u reason=%d",
             event->disconnect.conn.conn_handle,
             event->disconnect.reason);
    lua_ble_runtime_lock();
    draining = s_draining;
    lua_ble_runtime_unlock();
    if (conn_index >= 0) {
        lua_ble_event_t *disconnect_event = lua_ble_event_alloc();
        if (disconnect_event) {
            disconnect_event->type = LUA_BLE_EVENT_DISCONNECTED;
            disconnect_event->conn_index = (uint8_t)conn_index;
            if (draining) {
                strlcpy(disconnect_event->reason, "deinit", sizeof(disconnect_event->reason));
            } else if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL)) {
                strlcpy(disconnect_event->reason, "local", sizeof(disconnect_event->reason));
            } else if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO)) {
                strlcpy(disconnect_event->reason, "timeout", sizeof(disconnect_event->reason));
            } else if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM) ||
                       event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_RESRCS) ||
                       event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_PWROFF)) {
                strlcpy(disconnect_event->reason, "remote", sizeof(disconnect_event->reason));
            } else {
                strlcpy(disconnect_event->reason, "error", sizeof(disconnect_event->reason));
                disconnect_event->error_code = event->disconnect.reason;
            }
            lua_ble_event_enqueue(disconnect_event);
        } else {
            ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_DISCONNECTED);
        }
        lua_ble_connection_clear_index((uint8_t)conn_index);
        if (s_disconnect_sem) {
            (void)xSemaphoreGive(s_disconnect_sem);
        }
    }
    lua_ble_runtime_lock();
    if (conn_index >= 0 && s_advertising_requested && !s_advertising &&
        s_advertising_paused_for_connection_full) {
        uint8_t slots = lua_ble_slot_count();
        int active_connections = 0;

        for (uint8_t i = 0; i < slots; i++) {
            if (s_conns[i].connected) {
                active_connections++;
            }
        }
        if (active_connections < slots) {
            adv_config = s_adv_config;
            should_restart_adv = true;
        }
    }
    lua_ble_runtime_unlock();
    if (should_restart_adv) {
        rc = lua_ble_adv_apply(&adv_config);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "restart advertising after disconnect failed: %s", esp_err_to_name(rc));
        } else {
            lua_ble_runtime_lock();
            s_advertising_paused_for_connection_full = false;
            lua_ble_runtime_unlock();
            lua_ble_event_simple_adv(LUA_BLE_EVENT_ADV_STARTED, "re_advertise", 0);
        }
    }
    return 0;
}

static int lua_ble_handle_gap_adv_complete(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap adv_complete reason=%d", event->adv_complete.reason);
    lua_ble_runtime_lock();
    s_advertising = false;
    lua_ble_runtime_unlock();
    if (s_advertising_requested) {
        lua_ble_event_simple_adv(LUA_BLE_EVENT_ADV_STOPPED,
                                 (event->adv_complete.reason == 0 ||
                                  event->adv_complete.reason == BLE_HS_ETIMEOUT) ? "complete" : "error",
                                 (event->adv_complete.reason == 0 ||
                                  event->adv_complete.reason == BLE_HS_ETIMEOUT) ? 0 : event->adv_complete.reason);
    }
    return 0;
}

static int lua_ble_handle_gap_enc_change(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap enc_change conn_handle=%u status=%d",
             event->enc_change.conn_handle,
             event->enc_change.status);
    if (event->enc_change.status == 0) {
        lua_ble_connection_update_security(event->enc_change.conn_handle, "enc_change");
    } else {
        lua_ble_connection_update_security(event->enc_change.conn_handle, "security_failed");
    }
    return 0;
}

static int lua_ble_handle_gap_identity_resolved(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap identity_resolved conn_handle=%u", event->identity_resolved.conn_handle);
    lua_ble_connection_update_security(event->identity_resolved.conn_handle, "identity_resolved");
    return 0;
}

static int lua_ble_handle_gap_repeat_pairing(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap repeat_pairing conn_handle=%u key_size=%u authenticated=%u sc=%u",
             event->repeat_pairing.conn_handle,
             event->repeat_pairing.cur_key_size,
             event->repeat_pairing.cur_authenticated,
             event->repeat_pairing.cur_sc);
    return lua_ble_smp_handle_repeat_pairing(&event->repeat_pairing);
}

static int lua_ble_handle_gap_passkey_action(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap passkey_action conn_handle=%u action=%u",
             event->passkey.conn_handle,
             event->passkey.params.action);
    lua_ble_smp_handle_passkey_action(event->passkey.conn_handle, &event->passkey.params);
    return 0;
}

static int lua_ble_handle_gap_subscribe(struct ble_gap_event *event)
{
    int conn_index;

    ESP_LOGI(TAG, "gap subscribe conn_handle=%u attr_handle=%u reason=%u prev_notify=%u cur_notify=%u "
             "prev_indicate=%u cur_indicate=%u",
             event->subscribe.conn_handle,
             event->subscribe.attr_handle,
             event->subscribe.reason,
             event->subscribe.prev_notify,
             event->subscribe.cur_notify,
             event->subscribe.prev_indicate,
             event->subscribe.cur_indicate);
    conn_index = lua_ble_find_conn_index_by_handle(event->subscribe.conn_handle);
    if (conn_index >= 0) {
        lua_ble_notify_subscription_update((uint8_t)conn_index, event->subscribe.attr_handle,
                                           event->subscribe.cur_notify, event->subscribe.cur_indicate);
        lua_ble_event_t *ev = lua_ble_event_alloc();
        if (ev) {
            ev->type = LUA_BLE_EVENT_SUBSCRIBE_CHANGED;
            ev->conn_index = (uint8_t)conn_index;
            ev->conn_handle = event->subscribe.conn_handle;
            ev->notify = event->subscribe.cur_notify;
            ev->indicate = event->subscribe.cur_indicate;
            (void)lua_ble_gatts_fill_event_by_handle(ev, event->subscribe.attr_handle);
            lua_ble_event_enqueue(ev);
        } else {
            ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_SUBSCRIBE_CHANGED);
        }
    }
    return 0;
}

static int lua_ble_handle_gap_notify_tx(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap notify_tx conn_handle=%u attr_handle=%u indication=%u status=%d",
             event->notify_tx.conn_handle,
             event->notify_tx.attr_handle,
             event->notify_tx.indication,
             event->notify_tx.status);
    lua_ble_gatts_on_notify_tx(event->notify_tx.conn_handle, event->notify_tx.attr_handle,
                               event->notify_tx.indication, event->notify_tx.status);
    return 0;
}

static int lua_ble_handle_gap_mtu(struct ble_gap_event *event)
{
    int conn_index;

    ESP_LOGI(TAG, "gap mtu conn_handle=%u channel_id=%u mtu=%u",
             event->mtu.conn_handle,
             event->mtu.channel_id,
             event->mtu.value);
    conn_index = lua_ble_find_conn_index_by_handle(event->mtu.conn_handle);
    if (conn_index >= 0) {
        lua_ble_runtime_lock();
        s_conns[conn_index].mtu = event->mtu.value;
        lua_ble_runtime_unlock();
        lua_ble_event_t *ev = lua_ble_event_alloc();
        if (ev) {
            ev->type = LUA_BLE_EVENT_MTU_CHANGED;
            ev->conn_index = (uint8_t)conn_index;
            ev->conn_handle = event->mtu.conn_handle;
            ev->mtu = event->mtu.value;
            lua_ble_event_enqueue(ev);
        } else {
            ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_MTU_CHANGED);
        }
    }
    return 0;
}

static const lua_ble_gap_event_handler_entry_t s_gap_event_handlers[] = {
    { BLE_GAP_EVENT_CONNECT, lua_ble_handle_gap_connect },
    { BLE_GAP_EVENT_DISCONNECT, lua_ble_handle_gap_disconnect },
    { BLE_GAP_EVENT_ADV_COMPLETE, lua_ble_handle_gap_adv_complete },
    { BLE_GAP_EVENT_ENC_CHANGE, lua_ble_handle_gap_enc_change },
    { BLE_GAP_EVENT_IDENTITY_RESOLVED, lua_ble_handle_gap_identity_resolved },
    { BLE_GAP_EVENT_REPEAT_PAIRING, lua_ble_handle_gap_repeat_pairing },
    { BLE_GAP_EVENT_PASSKEY_ACTION, lua_ble_handle_gap_passkey_action },
    { BLE_GAP_EVENT_SUBSCRIBE, lua_ble_handle_gap_subscribe },
    { BLE_GAP_EVENT_NOTIFY_TX, lua_ble_handle_gap_notify_tx },
    { BLE_GAP_EVENT_MTU, lua_ble_handle_gap_mtu },
};

static int lua_ble_dispatch_gap_event(struct ble_gap_event *event)
{
    for (size_t i = 0; i < sizeof(s_gap_event_handlers) / sizeof(s_gap_event_handlers[0]); i++) {
        if (s_gap_event_handlers[i].type == event->type) {
            return s_gap_event_handlers[i].handler(event);
        }
    }
    return 0;
}

static esp_err_t lua_ble_adv_stop_internal(void)
{
    int rc;

    if (!s_stack_inited) {
        lua_ble_runtime_lock();
        s_advertising = false;
        s_advertising_requested = false;
        s_advertising_paused_for_connection_full = false;
        lua_ble_runtime_unlock();
        return ESP_OK;
    }

    rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        return ESP_FAIL;
    }

    lua_ble_runtime_lock();
    s_advertising = false;
    s_advertising_requested = false;
    s_advertising_paused_for_connection_full = false;
    lua_ble_runtime_unlock();
    return ESP_OK;
}

static esp_err_t lua_ble_adv_apply(const lua_ble_adv_config_t *config)
{
    int rc;

    if (!s_stack_inited || !s_host_synced) {
        ESP_LOGE(TAG, "[adv]: apply invalid state stack_inited=%u host_synced=%u internal_free=%u largest=%u",
                 (unsigned)s_stack_inited, (unsigned)s_host_synced,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || config->adv_data_len > LUA_BLE_ADV_DATA_MAX ||
        config->scan_rsp_len > LUA_BLE_ADV_DATA_MAX) {
        ESP_LOGE(TAG, "[adv]: apply invalid arg config_null=%u adv_len=%u scan_rsp_len=%u max=%u internal_free=%u largest=%u",
                 (unsigned)(config == NULL), config ? (unsigned)config->adv_data_len : 0,
                 config ? (unsigned)config->scan_rsp_len : 0, (unsigned)LUA_BLE_ADV_DATA_MAX,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_INVALID_ARG;
    }

    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }
    rc = ble_gap_adv_set_data(config->adv_data, config->adv_data_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "[adv]: ble_gap_adv_set_data failed rc=%d adv_len=%u internal_free=%u largest=%u",
                 rc, (unsigned)config->adv_data_len,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return rc == BLE_HS_EMSGSIZE ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }
    rc = ble_gap_adv_rsp_set_data(config->scan_rsp_data, config->scan_rsp_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "[adv]: ble_gap_adv_rsp_set_data failed rc=%d scan_rsp_len=%u internal_free=%u largest=%u",
                 rc, (unsigned)config->scan_rsp_len,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return rc == BLE_HS_EMSGSIZE ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }

    rc = ble_gap_adv_start(config->own_addr_type, config->directed ? &config->directed_addr : NULL,
                           BLE_HS_FOREVER, &config->params, lua_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG,
                 "[adv]: ble_gap_adv_start failed rc=%d own_addr_type=%u directed=%u conn_mode=%u disc_mode=%u "
                 "itvl_min=%u itvl_max=%u adv_len=%u scan_rsp_len=%u internal_free=%u largest=%u",
                 rc, (unsigned)config->own_addr_type, (unsigned)config->directed, (unsigned)config->params.conn_mode,
                 (unsigned)config->params.disc_mode, (unsigned)config->params.itvl_min, (unsigned)config->params.itvl_max,
                 (unsigned)config->adv_data_len, (unsigned)config->scan_rsp_len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_FAIL;
    }

    lua_ble_runtime_lock();
    s_advertising = true;
    lua_ble_runtime_unlock();
    return ESP_OK;
}

