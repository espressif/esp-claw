/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_ble_priv.h"

#include <ctype.h>

#include "esp_heap_caps.h"

typedef struct {
    ble_uuid_any_t uuid;
    char uuid_str[LUA_BLE_UUID_STR_MAX];
    char id[LUA_BLE_GATTS_ID_MAX + 1];
    uint8_t first_chr;
    uint8_t chr_count;
} lua_ble_gatts_service_t;

typedef struct {
    ble_uuid_any_t uuid;
    char uuid_str[LUA_BLE_UUID_STR_MAX];
    uint8_t parent_chr;
    bool read_encrypted;
    bool write_encrypted;
    bool read_authenticated;
    bool write_authenticated;
    uint16_t max_len;
    uint16_t value_len;
    uint8_t *value;
} lua_ble_gatts_descriptor_t;

typedef struct {
    ble_uuid_any_t uuid;
    char uuid_str[LUA_BLE_UUID_STR_MAX];
    char id[LUA_BLE_GATTS_ID_MAX + 1];
    uint8_t service_index;
    uint8_t first_desc;
    uint8_t desc_count;
    uint16_t val_handle;
    uint16_t flags;
    bool read_encrypted;
    bool write_encrypted;
    bool read_authenticated;
    bool write_authenticated;
    bool indicate_in_progress[LUA_BLE_MAX_CONNECTIONS];
    uint16_t max_len;
    uint16_t value_len;
    uint8_t *value;
} lua_ble_gatts_char_t;

typedef struct {
    bool active;
    lua_ble_gatts_service_t *services;
    lua_ble_gatts_char_t *chars;
    lua_ble_gatts_descriptor_t *descs;
    uint8_t service_count;
    uint8_t char_count;
    uint8_t desc_count;
    struct ble_gatt_svc_def *svc_defs;
    struct ble_gatt_chr_def (*chr_defs)[LUA_BLE_GATTS_MAX_CHARS + 1];
    struct ble_gatt_dsc_def (*dsc_defs)[LUA_BLE_GATTS_MAX_DESCRIPTORS + 1];
} lua_ble_gatts_runtime_t;

static lua_ble_gatts_runtime_t s_gatts_rt;

#define s_gatts_active (s_gatts_rt.active)
#define s_gatts_services (s_gatts_rt.services)
#define s_gatts_chars (s_gatts_rt.chars)
#define s_gatts_descs (s_gatts_rt.descs)
#define s_gatts_service_count (s_gatts_rt.service_count)
#define s_gatts_char_count (s_gatts_rt.char_count)
#define s_gatts_desc_count (s_gatts_rt.desc_count)
#define s_gatts_svc_defs (s_gatts_rt.svc_defs)
#define s_gatts_chr_defs (s_gatts_rt.chr_defs)
#define s_gatts_dsc_defs (s_gatts_rt.dsc_defs)

static void *lua_ble_gatts_calloc_prefer_psram(size_t count, size_t size)
{
    return heap_caps_calloc_prefer(count,
                                   size,
                                   2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static bool lua_ble_gatts_runtime_allocated(void)
{
    return s_gatts_services && s_gatts_chars && s_gatts_descs &&
           s_gatts_svc_defs && s_gatts_chr_defs && s_gatts_dsc_defs;
}

static void lua_ble_gatts_free_runtime_arrays(void)
{
    heap_caps_free(s_gatts_services);
    heap_caps_free(s_gatts_chars);
    heap_caps_free(s_gatts_descs);
    heap_caps_free(s_gatts_svc_defs);
    heap_caps_free(s_gatts_chr_defs);
    heap_caps_free(s_gatts_dsc_defs);
    s_gatts_services = NULL;
    s_gatts_chars = NULL;
    s_gatts_descs = NULL;
    s_gatts_svc_defs = NULL;
    s_gatts_chr_defs = NULL;
    s_gatts_dsc_defs = NULL;
}

static esp_err_t lua_ble_gatts_alloc_runtime_arrays(void)
{
    s_gatts_services = lua_ble_gatts_calloc_prefer_psram(LUA_BLE_GATTS_MAX_SERVICES,
                                                         sizeof(s_gatts_services[0]));
    s_gatts_chars = lua_ble_gatts_calloc_prefer_psram(LUA_BLE_GATTS_MAX_CHARS,
                                                      sizeof(s_gatts_chars[0]));
    s_gatts_descs = lua_ble_gatts_calloc_prefer_psram(LUA_BLE_GATTS_MAX_CHARS * LUA_BLE_GATTS_MAX_DESCRIPTORS,
                                                      sizeof(s_gatts_descs[0]));
    s_gatts_svc_defs = lua_ble_gatts_calloc_prefer_psram(LUA_BLE_GATTS_MAX_SERVICES + 1,
                                                         sizeof(s_gatts_svc_defs[0]));
    s_gatts_chr_defs = lua_ble_gatts_calloc_prefer_psram(LUA_BLE_GATTS_MAX_SERVICES,
                                                         sizeof(s_gatts_chr_defs[0]));
    s_gatts_dsc_defs = lua_ble_gatts_calloc_prefer_psram(LUA_BLE_GATTS_MAX_CHARS,
                                                         sizeof(s_gatts_dsc_defs[0]));
    if (!lua_ble_gatts_runtime_allocated()) {
        lua_ble_gatts_free_runtime_arrays();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t lua_ble_gatts_event_copy_mbuf(lua_ble_event_t *event, struct os_mbuf *om, uint16_t data_len)
{
    int rc;

    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    heap_caps_free(event->data);
    event->data = NULL;
    event->data_len = 0;
    if (data_len == 0) {
        return ESP_OK;
    }
    event->data = heap_caps_malloc_prefer(data_len,
                                          2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!event->data) {
        return ESP_ERR_NO_MEM;
    }
    rc = os_mbuf_copydata(om, 0, data_len, event->data);
    if (rc != 0) {
        heap_caps_free(event->data);
        event->data = NULL;
        return ESP_FAIL;
    }
    event->data_len = data_len;
    return ESP_OK;
}

static void lua_ble_gatts_free_values(void)
{
    if (s_gatts_chars) {
        for (uint8_t i = 0; i < s_gatts_char_count; i++) {
            heap_caps_free(s_gatts_chars[i].value);
            s_gatts_chars[i].value = NULL;
        }
    }
    if (s_gatts_descs) {
        for (uint8_t i = 0; i < s_gatts_desc_count; i++) {
            heap_caps_free(s_gatts_descs[i].value);
            s_gatts_descs[i].value = NULL;
        }
    }
}

static void lua_ble_gatts_parse_rollback(uint8_t char_count, uint8_t desc_count)
{
    for (uint8_t i = desc_count; s_gatts_descs && i < s_gatts_desc_count; i++) {
        heap_caps_free(s_gatts_descs[i].value);
        s_gatts_descs[i].value = NULL;
    }
    for (uint8_t i = char_count; s_gatts_chars && i < s_gatts_char_count; i++) {
        heap_caps_free(s_gatts_chars[i].value);
        s_gatts_chars[i].value = NULL;
    }
    s_gatts_desc_count = desc_count;
    s_gatts_char_count = char_count;
}

static bool lua_ble_gatts_is_hex_string(const char *s)
{
    for (; *s; s++) {
        if (!isxdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static int lua_ble_gatts_normalize_uuid(lua_State *L, const char *uuid_str, ble_uuid_any_t *uuid,
                                        char out[LUA_BLE_UUID_STR_MAX])
{
    char hex[33];
    size_t hex_len = 0;
    int rc;

    if (!uuid_str || !uuid || !out) {
        return luaL_error(L, "uuid is required");
    }
    for (const char *p = uuid_str; *p; p++) {
        if (*p == '-') {
            continue;
        }
        if (!isxdigit((unsigned char)*p) || hex_len >= sizeof(hex) - 1) {
            return luaL_error(L, "invalid UUID: %s", uuid_str);
        }
        hex[hex_len++] = (char)tolower((unsigned char)*p);
    }
    hex[hex_len] = '\0';
    if (hex_len == 4 || hex_len == 8) {
        strlcpy(out, hex, LUA_BLE_UUID_STR_MAX);
    } else if (hex_len == 32) {
        snprintf(out, LUA_BLE_UUID_STR_MAX,
                 "%.8s-%.4s-%.4s-%.4s-%.12s",
                 hex, hex + 8, hex + 12, hex + 16, hex + 20);
    } else {
        return luaL_error(L, "invalid UUID length: %s", uuid_str);
    }
    if (!lua_ble_gatts_is_hex_string(hex)) {
        return luaL_error(L, "invalid UUID: %s", uuid_str);
    }
    rc = ble_uuid_from_str(uuid, out);
    if (rc != 0) {
        return luaL_error(L, "invalid UUID: %s", uuid_str);
    }
    return 0;
}

static bool lua_ble_gatts_get_bool(lua_State *L, int table_index, const char *field)
{
    bool value = false;

    table_index = lua_absindex(L, table_index);
    lua_getfield(L, table_index, field);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static uint16_t lua_ble_gatts_get_max_len(lua_State *L, int table_index, uint16_t default_len)
{
    uint16_t max_len = default_len;

    table_index = lua_absindex(L, table_index);
    lua_getfield(L, table_index, "max_len");
    if (!lua_isnil(L, -1)) {
        int value = (int)luaL_checkinteger(L, -1);
        if (value < 1 || value > LUA_BLE_GATTS_VALUE_MAX) {
            luaL_error(L, "max_len must be in 1..%d", LUA_BLE_GATTS_VALUE_MAX);
        }
        max_len = (uint16_t)value;
    }
    lua_pop(L, 1);
    return max_len;
}

static void lua_ble_gatts_copy_id(lua_State *L, int table_index, const char *field, char *dst, size_t dst_len)
{
    table_index = lua_absindex(L, table_index);
    lua_getfield(L, table_index, field);
    if (!lua_isnil(L, -1)) {
        size_t len;
        const char *id = luaL_checklstring(L, -1, &len);

        if (len == 0 || len > LUA_BLE_GATTS_ID_MAX) {
            luaL_error(L, "%s length must be 1..%d", field, LUA_BLE_GATTS_ID_MAX);
        }
        strlcpy(dst, id, dst_len);
    }
    lua_pop(L, 1);
}

static int lua_ble_gatts_copy_value(lua_State *L, int table_index, uint8_t **dst, uint16_t *value_len, uint16_t max_len)
{
    uint8_t *value_buf;
    const char *value = NULL;
    size_t len = 0;

    table_index = lua_absindex(L, table_index);
    lua_getfield(L, table_index, "value");
    if (!lua_isnil(L, -1)) {
        value = luaL_checklstring(L, -1, &len);
        if (len > max_len) {
            lua_pop(L, 1);
            return lua_ble_push_err_string(L, "ble_gatt_data_too_long");
        }
    }
    value_buf = lua_ble_gatts_calloc_prefer_psram(1, max_len);
    if (!value_buf) {
        lua_pop(L, 1);
        return lua_ble_push_err_string(L, "ble_no_mem");
    }
    if (len > 0) {
        memcpy(value_buf, value, len);
    }
    *dst = value_buf;
    *value_len = (uint16_t)len;
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_gatts_parse_permissions(lua_State *L, int table_index, bool *read, bool *write,
                                           bool *read_encrypted, bool *write_encrypted,
                                           bool *read_authenticated, bool *write_authenticated)
{
    table_index = lua_absindex(L, table_index);
    lua_getfield(L, table_index, "permissions");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    luaL_checktype(L, -1, LUA_TTABLE);
    *read = *read || lua_ble_gatts_get_bool(L, -1, "read");
    *write = *write || lua_ble_gatts_get_bool(L, -1, "write");
    *read_encrypted = lua_ble_gatts_get_bool(L, -1, "read_encrypted");
    *write_encrypted = lua_ble_gatts_get_bool(L, -1, "write_encrypted");
    *read_authenticated = lua_ble_gatts_get_bool(L, -1, "read_authenticated");
    *write_authenticated = lua_ble_gatts_get_bool(L, -1, "write_authenticated");
    lua_pop(L, 1);
    return 0;
}

static bool lua_ble_gatts_duplicate_char_id(const char *id)
{
    if (!id[0]) {
        return false;
    }
    for (uint8_t i = 0; i < s_gatts_char_count; i++) {
        if (strcmp(s_gatts_chars[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool lua_ble_gatts_duplicate_desc_uuid(uint8_t first_desc, uint8_t desc_count, const char *uuid)
{
    for (uint8_t i = 0; i < desc_count; i++) {
        if (strcmp(s_gatts_descs[first_desc + i].uuid_str, uuid) == 0) {
            return true;
        }
    }
    return false;
}

static int lua_ble_gatts_check_security(uint16_t conn_handle, bool needs_encrypted, bool needs_authenticated)
{
    int conn_index = lua_ble_find_conn_index_by_handle(conn_handle);
    bool encrypted;
    bool authenticated;

    if (conn_index < 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    lua_ble_connection_refresh_security(conn_handle);
    lua_ble_runtime_lock();
    if (!s_conns[conn_index].connected || s_conns[conn_index].conn_handle != conn_handle) {
        lua_ble_runtime_unlock();
        return BLE_ATT_ERR_UNLIKELY;
    }
    encrypted = s_conns[conn_index].encrypted;
    authenticated = s_conns[conn_index].authenticated;
    lua_ble_runtime_unlock();
    if (needs_authenticated && !authenticated) {
        ESP_LOGW(TAG, "[gatt_security]: authenticated access denied conn_handle=%u conn_index=%d encrypted=%u authenticated=%u",
                 conn_handle, conn_index, encrypted, authenticated);
        (void)ble_gap_security_initiate(conn_handle);
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (needs_encrypted && !encrypted) {
        ESP_LOGW(TAG, "[gatt_security]: encrypted access denied conn_handle=%u conn_index=%d encrypted=%u authenticated=%u",
                 conn_handle, conn_index, encrypted, authenticated);
        (void)ble_gap_security_initiate(conn_handle);
        return BLE_ATT_ERR_INSUFFICIENT_ENC;
    }
    return 0;
}

static void lua_ble_gatts_fill_char_event(lua_ble_event_t *event, const lua_ble_gatts_char_t *chr)
{
    const lua_ble_gatts_service_t *svc = &s_gatts_services[chr->service_index];

    if (svc->id[0]) {
        strlcpy(event->service_id, svc->id, sizeof(event->service_id));
    }
    if (chr->id[0]) {
        strlcpy(event->characteristic_id, chr->id, sizeof(event->characteristic_id));
    }
    strlcpy(event->uuid_service, svc->uuid_str, sizeof(event->uuid_service));
    strlcpy(event->uuid_characteristic, chr->uuid_str, sizeof(event->uuid_characteristic));
}

bool lua_ble_gatts_fill_event_by_handle(lua_ble_event_t *event, uint16_t attr_handle)
{
    bool found = false;

    if (!event) {
        return false;
    }
    lua_ble_runtime_lock();
    if (!s_gatts_chars || !s_gatts_services) {
        lua_ble_runtime_unlock();
        return false;
    }
    for (uint8_t i = 0; i < s_gatts_char_count; i++) {
        if (s_gatts_chars[i].val_handle == attr_handle) {
            lua_ble_gatts_fill_char_event(event, &s_gatts_chars[i]);
            found = true;
            break;
        }
    }
    lua_ble_runtime_unlock();
    return found;
}

void lua_ble_gatts_clear_conn_state(uint8_t conn_index)
{
    if (conn_index >= LUA_BLE_MAX_CONNECTIONS) {
        return;
    }

    lua_ble_runtime_lock();
    if (!s_gatts_chars) {
        lua_ble_runtime_unlock();
        return;
    }
    for (uint8_t i = 0; i < s_gatts_char_count; i++) {
        s_gatts_chars[i].indicate_in_progress[conn_index] = false;
    }
    lua_ble_runtime_unlock();
}

static int lua_ble_gatts_access_chr_read(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt,
                                         lua_ble_gatts_char_t *chr, int conn_index)
{
    int rc = lua_ble_gatts_check_security(conn_handle, chr->read_encrypted, chr->read_authenticated);
    uint16_t read_len;

    if (rc != 0) {
        return rc;
    }
    lua_ble_runtime_lock();
    if (ctxt->offset > chr->value_len) {
        lua_ble_runtime_unlock();
        return BLE_ATT_ERR_INVALID_OFFSET;
    }
    read_len = chr->value_len - ctxt->offset;
    if (read_len > 0) {
        rc = os_mbuf_append(ctxt->om, chr->value + ctxt->offset, read_len);
        if (rc != 0) {
            lua_ble_runtime_unlock();
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    lua_ble_runtime_unlock();
    lua_ble_event_t *event = lua_ble_event_alloc();
    if (event) {
        event->type = LUA_BLE_EVENT_GATTS_READ;
        event->conn_index = (uint8_t)conn_index;
        event->conn_handle = conn_handle;
        event->offset = ctxt->offset;
        lua_ble_gatts_fill_char_event(event, chr);
        lua_ble_event_enqueue(event);
    } else {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_GATTS_READ);
    }
    return 0;
}

static int lua_ble_gatts_access_chr_write(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt,
                                          lua_ble_gatts_char_t *chr, int conn_index)
{
    int om_len = OS_MBUF_PKTLEN(ctxt->om);
    int rc = lua_ble_gatts_check_security(conn_handle, chr->write_encrypted, chr->write_authenticated);

    if (rc != 0) {
        return rc;
    }
    if (om_len < 0 || ctxt->offset > chr->max_len || (size_t)om_len > chr->max_len - ctxt->offset) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    lua_ble_event_t *event = lua_ble_event_alloc();
    if (!event) {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_GATTS_WRITE);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    event->type = LUA_BLE_EVENT_GATTS_WRITE;
    event->conn_index = (uint8_t)conn_index;
    event->conn_handle = conn_handle;
    event->offset = ctxt->offset;
    rc = lua_ble_gatts_event_copy_mbuf(event, ctxt->om, (uint16_t)om_len);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "BLE event payload alloc failed type=%d len=%d rc=%s",
                 LUA_BLE_EVENT_GATTS_WRITE, om_len, esp_err_to_name(rc));
        lua_ble_event_free(event);
        return rc == ESP_ERR_NO_MEM ? BLE_ATT_ERR_INSUFFICIENT_RES : BLE_ATT_ERR_UNLIKELY;
    }
    lua_ble_gatts_fill_char_event(event, chr);
    lua_ble_runtime_lock();
    if (!s_event_queue) {
        lua_ble_runtime_unlock();
        lua_ble_event_free(event);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (om_len > 0) {
        memcpy(chr->value + ctxt->offset, event->data, om_len);
    }
    chr->value_len = ctxt->offset + (uint16_t)om_len;
    lua_ble_event_enqueue_force(event);
    lua_ble_runtime_unlock();
    return 0;
}

static int lua_ble_gatts_access_chr(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    lua_ble_gatts_char_t *chr = (lua_ble_gatts_char_t *)arg;
    int conn_index = lua_ble_find_conn_index_by_handle(conn_handle);

    (void)attr_handle;
    if (!chr || conn_index < 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return lua_ble_gatts_access_chr_read(conn_handle, ctxt, chr, conn_index);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return lua_ble_gatts_access_chr_write(conn_handle, ctxt, chr, conn_index);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int lua_ble_gatts_access_dsc_read(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt,
                                         lua_ble_gatts_descriptor_t *dsc, lua_ble_gatts_char_t *chr,
                                         int conn_index)
{
    int rc = lua_ble_gatts_check_security(conn_handle, dsc->read_encrypted, dsc->read_authenticated);
    uint16_t read_len;

    if (rc != 0) {
        return rc;
    }
    lua_ble_runtime_lock();
    if (ctxt->offset > dsc->value_len) {
        lua_ble_runtime_unlock();
        return BLE_ATT_ERR_INVALID_OFFSET;
    }
    read_len = dsc->value_len - ctxt->offset;
    if (read_len > 0) {
        rc = os_mbuf_append(ctxt->om, dsc->value + ctxt->offset, read_len);
        if (rc != 0) {
            lua_ble_runtime_unlock();
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    lua_ble_runtime_unlock();
    lua_ble_event_t *event = lua_ble_event_alloc();
    if (event) {
        event->type = LUA_BLE_EVENT_GATTS_READ;
        event->conn_index = (uint8_t)conn_index;
        event->conn_handle = conn_handle;
        event->offset = ctxt->offset;
        lua_ble_gatts_fill_char_event(event, chr);
        strlcpy(event->uuid_descriptor, dsc->uuid_str, sizeof(event->uuid_descriptor));
        lua_ble_event_enqueue(event);
    } else {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_GATTS_READ);
    }
    return 0;
}

static int lua_ble_gatts_access_dsc_write(uint16_t conn_handle, struct ble_gatt_access_ctxt *ctxt,
                                          lua_ble_gatts_descriptor_t *dsc, lua_ble_gatts_char_t *chr,
                                          int conn_index)
{
    int om_len = OS_MBUF_PKTLEN(ctxt->om);
    int rc = lua_ble_gatts_check_security(conn_handle, dsc->write_encrypted, dsc->write_authenticated);

    if (rc != 0) {
        return rc;
    }
    if (om_len < 0 || ctxt->offset > dsc->max_len || (size_t)om_len > dsc->max_len - ctxt->offset) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    lua_ble_event_t *event = lua_ble_event_alloc();
    if (!event) {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_GATTS_WRITE);
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    event->type = LUA_BLE_EVENT_GATTS_WRITE;
    event->conn_index = (uint8_t)conn_index;
    event->conn_handle = conn_handle;
    event->offset = ctxt->offset;
    rc = lua_ble_gatts_event_copy_mbuf(event, ctxt->om, (uint16_t)om_len);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "BLE event payload alloc failed type=%d len=%d rc=%s",
                 LUA_BLE_EVENT_GATTS_WRITE, om_len, esp_err_to_name(rc));
        lua_ble_event_free(event);
        return rc == ESP_ERR_NO_MEM ? BLE_ATT_ERR_INSUFFICIENT_RES : BLE_ATT_ERR_UNLIKELY;
    }
    lua_ble_gatts_fill_char_event(event, chr);
    strlcpy(event->uuid_descriptor, dsc->uuid_str, sizeof(event->uuid_descriptor));
    lua_ble_runtime_lock();
    if (!s_event_queue) {
        lua_ble_runtime_unlock();
        lua_ble_event_free(event);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (om_len > 0) {
        memcpy(dsc->value + ctxt->offset, event->data, om_len);
    }
    dsc->value_len = ctxt->offset + (uint16_t)om_len;
    lua_ble_event_enqueue_force(event);
    lua_ble_runtime_unlock();
    return 0;
}

static int lua_ble_gatts_access_dsc(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    lua_ble_gatts_descriptor_t *dsc = (lua_ble_gatts_descriptor_t *)arg;
    lua_ble_gatts_char_t *chr;
    int conn_index = lua_ble_find_conn_index_by_handle(conn_handle);

    (void)attr_handle;
    if (!dsc || !s_gatts_chars || conn_index < 0 || dsc->parent_chr >= s_gatts_char_count) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    chr = &s_gatts_chars[dsc->parent_chr];

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        return lua_ble_gatts_access_dsc_read(conn_handle, ctxt, dsc, chr, conn_index);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) {
        return lua_ble_gatts_access_dsc_write(conn_handle, ctxt, dsc, chr, conn_index);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int lua_ble_gatts_parse_descriptor_identity(lua_State *L, int dsc_index, uint8_t parent_chr,
                                                  lua_ble_gatts_descriptor_t *dsc)
{
    char uuid_norm[LUA_BLE_UUID_STR_MAX];

    lua_getfield(L, dsc_index, "uuid");
    const char *uuid_str = luaL_checkstring(L, -1);
    int rc = lua_ble_gatts_normalize_uuid(L, uuid_str, &dsc->uuid, uuid_norm);
    lua_pop(L, 1);
    if (rc != 0) {
        return rc;
    }
    if (strcmp(uuid_norm, "2902") == 0) {
        return luaL_error(L, "descriptor uuid 2902 is managed by NimBLE CCCD");
    }
    if (lua_ble_gatts_duplicate_desc_uuid(s_gatts_chars[parent_chr].first_desc,
                                          s_gatts_chars[parent_chr].desc_count, uuid_norm)) {
        return luaL_error(L, "duplicate descriptor uuid: %s", uuid_norm);
    }
    strlcpy(dsc->uuid_str, uuid_norm, sizeof(dsc->uuid_str));
    return 0;
}

static int lua_ble_gatts_parse_descriptor_flags(lua_State *L, int dsc_index,
                                               lua_ble_gatts_descriptor_t *dsc, uint16_t *att_flags)
{
    bool read = false;
    bool write = false;

    lua_ble_gatts_parse_permissions(L, dsc_index, &read, &write,
                                    &dsc->read_encrypted, &dsc->write_encrypted,
                                    &dsc->read_authenticated, &dsc->write_authenticated);
    if ((dsc->read_authenticated || dsc->write_authenticated) &&
        !lua_ble_smp_allows_authenticated_access()) {
        return lua_ble_push_err_string(L, "ble_smp_invalid_config");
    }
    if (read || dsc->read_encrypted || dsc->read_authenticated) {
        *att_flags |= BLE_ATT_F_READ;
    }
    if (write || dsc->write_encrypted || dsc->write_authenticated) {
        *att_flags |= BLE_ATT_F_WRITE;
    }
    if (dsc->read_encrypted) {
        *att_flags |= BLE_ATT_F_READ_ENC;
    }
    if (dsc->write_encrypted) {
        *att_flags |= BLE_ATT_F_WRITE_ENC;
    }
    if (dsc->read_authenticated) {
        *att_flags |= BLE_ATT_F_READ_AUTHEN;
    }
    if (dsc->write_authenticated) {
        *att_flags |= BLE_ATT_F_WRITE_AUTHEN;
    }
    return 0;
}

static int lua_ble_gatts_parse_descriptor(lua_State *L, int dsc_index, uint8_t parent_chr)
{
    lua_ble_gatts_descriptor_t *dsc;
    uint16_t att_flags = 0;
    int rc;

    if (s_gatts_desc_count >= LUA_BLE_GATTS_MAX_CHARS * LUA_BLE_GATTS_MAX_DESCRIPTORS) {
        return luaL_error(L, "too many GATT descriptors");
    }
    dsc = &s_gatts_descs[s_gatts_desc_count];
    memset(dsc, 0, sizeof(*dsc));
    dsc->parent_chr = parent_chr;

    rc = lua_ble_gatts_parse_descriptor_identity(L, dsc_index, parent_chr, dsc);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_gatts_parse_descriptor_flags(L, dsc_index, dsc, &att_flags);
    if (rc != 0) {
        return rc;
    }
    dsc->max_len = lua_ble_gatts_get_max_len(L, dsc_index, LUA_BLE_GATT_VALUE_MAX);
    rc = lua_ble_gatts_copy_value(L, dsc_index, &dsc->value, &dsc->value_len, dsc->max_len);
    if (rc != 0) {
        return rc;
    }

    struct ble_gatt_dsc_def *def = &s_gatts_dsc_defs[parent_chr][s_gatts_chars[parent_chr].desc_count];
    memset(def, 0, sizeof(*def));
    def->uuid = &dsc->uuid.u;
    def->access_cb = lua_ble_gatts_access_dsc;
    def->arg = dsc;
    def->att_flags = att_flags;

    s_gatts_chars[parent_chr].desc_count++;
    s_gatts_desc_count++;
    return 0;
}

static int lua_ble_gatts_parse_characteristic_identity(lua_State *L, int chr_index, lua_ble_gatts_char_t *chr)
{
    lua_getfield(L, chr_index, "uuid");
    const char *uuid_str = luaL_checkstring(L, -1);
    int rc = lua_ble_gatts_normalize_uuid(L, uuid_str, &chr->uuid, chr->uuid_str);
    lua_pop(L, 1);
    if (rc != 0) {
        return rc;
    }
    lua_ble_gatts_copy_id(L, chr_index, "id", chr->id, sizeof(chr->id));
    if (lua_ble_gatts_duplicate_char_id(chr->id)) {
        return luaL_error(L, "duplicate characteristic id: %s", chr->id);
    }
    return 0;
}

static int lua_ble_gatts_parse_characteristic_flags(lua_State *L, int chr_index, lua_ble_gatts_char_t *chr)
{
    bool read = false;
    bool write = false;
    bool write_no_rsp = false;
    bool notify = false;
    bool indicate = false;
    int rc;

    lua_getfield(L, chr_index, "properties");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        read = lua_ble_gatts_get_bool(L, -1, "read");
        write = lua_ble_gatts_get_bool(L, -1, "write");
        write_no_rsp = lua_ble_gatts_get_bool(L, -1, "write_no_rsp");
        notify = lua_ble_gatts_get_bool(L, -1, "notify");
        indicate = lua_ble_gatts_get_bool(L, -1, "indicate");
    }
    lua_pop(L, 1);

    rc = lua_ble_gatts_parse_permissions(L, chr_index, &read, &write,
                                         &chr->read_encrypted, &chr->write_encrypted,
                                         &chr->read_authenticated, &chr->write_authenticated);
    if (rc != 0) {
        return rc;
    }
    if ((chr->read_authenticated || chr->write_authenticated) &&
        !lua_ble_smp_allows_authenticated_access()) {
        return lua_ble_push_err_string(L, "ble_smp_invalid_config");
    }
    if (read || chr->read_encrypted || chr->read_authenticated) {
        chr->flags |= BLE_GATT_CHR_F_READ;
    }
    if (write || chr->write_encrypted || chr->write_authenticated) {
        chr->flags |= BLE_GATT_CHR_F_WRITE;
    }
    if (write_no_rsp) {
        chr->flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
    }
    if (notify) {
        chr->flags |= BLE_GATT_CHR_F_NOTIFY;
    }
    if (indicate) {
        chr->flags |= BLE_GATT_CHR_F_INDICATE;
    }
    if (chr->read_encrypted) {
        chr->flags |= BLE_GATT_CHR_F_READ_ENC;
    }
    if (chr->write_encrypted) {
        chr->flags |= BLE_GATT_CHR_F_WRITE_ENC;
    }
    if (chr->read_authenticated) {
        chr->flags |= BLE_GATT_CHR_F_READ_AUTHEN;
    }
    if (chr->write_authenticated) {
        chr->flags |= BLE_GATT_CHR_F_WRITE_AUTHEN;
    }
    return 0;
}

static int lua_ble_gatts_parse_characteristic_descriptors(lua_State *L, int chr_index, uint8_t chr_model_index)
{
    int rc;

    lua_getfield(L, chr_index, "descriptors");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        size_t desc_count = lua_rawlen(L, -1);
        if (desc_count > LUA_BLE_GATTS_MAX_DESCRIPTORS) {
            return luaL_error(L, "too many descriptors for characteristic");
        }
        for (size_t i = 1; i <= desc_count; i++) {
            lua_rawgeti(L, -1, i);
            luaL_checktype(L, -1, LUA_TTABLE);
            rc = lua_ble_gatts_parse_descriptor(L, lua_gettop(L), chr_model_index);
            if (rc != 0) {
                return rc;
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 0;
}

static int lua_ble_gatts_parse_characteristic(lua_State *L, int chr_index, uint8_t service_index)
{
    lua_ble_gatts_char_t *chr;
    uint8_t initial_desc_count = s_gatts_desc_count;
    int rc;

    if (s_gatts_char_count >= LUA_BLE_GATTS_MAX_CHARS) {
        return luaL_error(L, "too many GATT characteristics");
    }
    chr = &s_gatts_chars[s_gatts_char_count];
    memset(chr, 0, sizeof(*chr));
    chr->service_index = service_index;
    chr->first_desc = s_gatts_desc_count;

    rc = lua_ble_gatts_parse_characteristic_identity(L, chr_index, chr);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_gatts_parse_characteristic_flags(L, chr_index, chr);
    if (rc != 0) {
        return rc;
    }

    chr->max_len = lua_ble_gatts_get_max_len(L, chr_index, LUA_BLE_GATT_VALUE_MAX);

    uint8_t chr_model_index = s_gatts_char_count;
    rc = lua_ble_gatts_parse_characteristic_descriptors(L, chr_index, chr_model_index);
    if (rc != 0) {
        goto fail;
    }

    rc = lua_ble_gatts_copy_value(L, chr_index, &chr->value, &chr->value_len, chr->max_len);
    if (rc != 0) {
        goto fail;
    }

    struct ble_gatt_chr_def *def =
        &s_gatts_chr_defs[service_index][s_gatts_services[service_index].chr_count];
    memset(def, 0, sizeof(*def));
    def->uuid = &chr->uuid.u;
    def->access_cb = lua_ble_gatts_access_chr;
    def->arg = chr;
    def->descriptors = chr->desc_count > 0 ? s_gatts_dsc_defs[chr_model_index] : NULL;
    def->flags = chr->flags;
    def->val_handle = &chr->val_handle;

    s_gatts_services[service_index].chr_count++;
    s_gatts_char_count++;
    return 0;

fail:
    heap_caps_free(chr->value);
    chr->value = NULL;
    chr->value_len = 0;
    lua_ble_gatts_parse_rollback(s_gatts_char_count, initial_desc_count);
    return rc;
}

static int lua_ble_gatts_parse_service(lua_State *L, int svc_index)
{
    lua_ble_gatts_service_t *svc;
    uint8_t initial_char_count = s_gatts_char_count;
    uint8_t initial_desc_count = s_gatts_desc_count;

    if (s_gatts_service_count >= LUA_BLE_GATTS_MAX_SERVICES) {
        return luaL_error(L, "too many GATT services");
    }
    svc = &s_gatts_services[s_gatts_service_count];
    memset(svc, 0, sizeof(*svc));
    svc->first_chr = s_gatts_char_count;

    if (lua_ble_gatts_get_bool(L, svc_index, "secondary")) {
        return lua_ble_push_err_string(L, "ble_unsupported");
    }

    lua_getfield(L, svc_index, "uuid");
    const char *uuid_str = luaL_checkstring(L, -1);
    int rc = lua_ble_gatts_normalize_uuid(L, uuid_str, &svc->uuid, svc->uuid_str);
    lua_pop(L, 1);
    if (rc != 0) {
        return rc;
    }
    lua_ble_gatts_copy_id(L, svc_index, "id", svc->id, sizeof(svc->id));

    uint8_t service_model_index = s_gatts_service_count;
    lua_getfield(L, svc_index, "characteristics");
    luaL_checktype(L, -1, LUA_TTABLE);
    size_t chr_count = lua_rawlen(L, -1);
    if (chr_count == 0) {
        return luaL_error(L, "service must contain at least one characteristic");
    }
    if (chr_count > LUA_BLE_GATTS_MAX_CHARS) {
        return luaL_error(L, "too many characteristics for service");
    }
    for (size_t i = 1; i <= chr_count; i++) {
        lua_rawgeti(L, -1, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        rc = lua_ble_gatts_parse_characteristic(L, lua_gettop(L), service_model_index);
        if (rc != 0) {
            goto fail;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    s_gatts_svc_defs[service_model_index].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_gatts_svc_defs[service_model_index].uuid = &svc->uuid.u;
    s_gatts_svc_defs[service_model_index].characteristics = s_gatts_chr_defs[service_model_index];
    s_gatts_service_count++;
    return 0;

fail:
    lua_ble_gatts_parse_rollback(initial_char_count, initial_desc_count);
    s_gatts_services[service_model_index].chr_count = 0;
    return rc;
}

void lua_ble_gatts_reset(void)
{
    lua_ble_runtime_lock();
    lua_ble_gatts_free_values();
    lua_ble_gatts_free_runtime_arrays();
    s_gatts_service_count = 0;
    s_gatts_char_count = 0;
    s_gatts_desc_count = 0;
    s_gatts_active = false;
    lua_ble_runtime_unlock();
}

int lua_ble_gatts_define(lua_State *L)
{
    int rc;
    esp_err_t err;

    if (!s_stack_inited || !s_host_synced) {
        return lua_ble_push_err_string(L, "ble_init_not_initialized");
    }
    if (s_gatts_active) {
        return lua_ble_push_err_string(L, "ble_gatt_already_started");
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "services");
    luaL_checktype(L, -1, LUA_TTABLE);
    size_t svc_count = lua_rawlen(L, -1);
    if (svc_count == 0) {
        return luaL_error(L, "profile.services must contain at least one service");
    }
    lua_pop(L, 1);

    lua_ble_gatts_reset();
    err = lua_ble_gatts_alloc_runtime_arrays();
    if (err != ESP_OK) {
        return lua_ble_push_ok_or_err(L, err);
    }

    lua_getfield(L, 1, "services");
    for (size_t i = 1; i <= svc_count; i++) {
        lua_rawgeti(L, -1, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        rc = lua_ble_gatts_parse_service(L, lua_gettop(L));
        if (rc != 0) {
            lua_ble_gatts_reset();
            return rc;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    rc = ble_gatts_add_dynamic_svcs(s_gatts_svc_defs);
    if (rc != 0) {
        lua_ble_gatts_reset();
        return lua_ble_push_err_string(L, "ble_resource_busy");
    }
    s_gatts_active = true;
    ESP_LOGI(TAG, "[gatt]: defined services=%u characteristics=%u descriptors=%u",
             s_gatts_service_count, s_gatts_char_count, s_gatts_desc_count);
    lua_pushboolean(L, 1);
    return 1;
}

static bool lua_ble_gatts_service_matches(const lua_ble_gatts_service_t *svc, const char *id)
{
    return id == NULL || id[0] == '\0' ||
           (svc->id[0] && strcmp(svc->id, id) == 0) ||
           strcmp(svc->uuid_str, id) == 0;
}

static int lua_ble_gatts_find_char(lua_State *L, const char *service_id, const char *identifier,
                                   lua_ble_gatts_char_t **out)
{
    char uuid_norm[LUA_BLE_UUID_STR_MAX] = { 0 };
    ble_uuid_any_t ignored_uuid;
    uint8_t match_count = 0;
    lua_ble_gatts_char_t *match = NULL;

    if (!identifier || !identifier[0]) {
        return lua_ble_push_err_string(L, "ble_gatt_char_not_found");
    }
    size_t ident_len = strlen(identifier);
    if (ident_len == 4 || ident_len == 8 || ident_len == 32 || ident_len == 36) {
        bool could_be_uuid = true;
        for (const char *p = identifier; *p; p++) {
            if (*p != '-' && !isxdigit((unsigned char)*p)) {
                could_be_uuid = false;
                break;
            }
        }
        if (could_be_uuid) {
            (void)lua_ble_gatts_normalize_uuid(L, identifier, &ignored_uuid, uuid_norm);
        }
    }

    for (uint8_t i = 0; i < s_gatts_char_count; i++) {
        lua_ble_gatts_char_t *chr = &s_gatts_chars[i];
        const lua_ble_gatts_service_t *svc = &s_gatts_services[chr->service_index];
        bool id_match = chr->id[0] && strcmp(chr->id, identifier) == 0;
        bool uuid_match = uuid_norm[0] && strcmp(chr->uuid_str, uuid_norm) == 0;

        if (!lua_ble_gatts_service_matches(svc, service_id)) {
            continue;
        }
        if (id_match || uuid_match) {
            match = chr;
            match_count++;
        }
    }
    if (match_count == 0) {
        return lua_ble_push_err_string(L, "ble_gatt_char_not_found");
    }
    if (match_count > 1) {
        return lua_ble_push_err_string(L, "ble_gatt_char_ambiguous");
    }
    *out = match;
    return 0;
}

static int lua_ble_gatts_find_char_from_table(lua_State *L, int table_index, const char *field,
                                              lua_ble_gatts_char_t **out)
{
    char service_id_buf[LUA_BLE_UUID_STR_MAX] = { 0 };
    char identifier_buf[LUA_BLE_UUID_STR_MAX] = { 0 };
    const char *service_id = NULL;
    size_t len;

    table_index = lua_absindex(L, table_index);
    lua_getfield(L, table_index, "service_id");
    if (!lua_isnil(L, -1)) {
        const char *value = luaL_checklstring(L, -1, &len);
        if (len >= sizeof(service_id_buf)) {
            return luaL_error(L, "service_id length must be <= %d", LUA_BLE_UUID_STR_MAX - 1);
        }
        strlcpy(service_id_buf, value, sizeof(service_id_buf));
        service_id = service_id_buf;
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, field);
    if (lua_isnil(L, -1) && strcmp(field, "characteristic_id") != 0) {
        lua_pop(L, 1);
        lua_getfield(L, table_index, "characteristic_id");
    }
    const char *identifier = luaL_checklstring(L, -1, &len);
    if (len >= sizeof(identifier_buf)) {
        return luaL_error(L, "%s length must be <= %d", field, LUA_BLE_UUID_STR_MAX - 1);
    }
    strlcpy(identifier_buf, identifier, sizeof(identifier_buf));
    lua_pop(L, 1);
    return lua_ble_gatts_find_char(L, service_id, identifier_buf, out);
}

int lua_ble_gatts_set_value(lua_State *L)
{
    lua_ble_gatts_char_t *chr = NULL;
    size_t len;
    const char *data;
    int rc;

    if (!s_gatts_active) {
        return lua_ble_push_err_string(L, "ble_invalid_state");
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    rc = lua_ble_gatts_find_char_from_table(L, 1, "characteristic_id", &chr);
    if (rc != 0) {
        return rc;
    }
    lua_getfield(L, 1, "data");
    data = luaL_checklstring(L, -1, &len);
    if (len > chr->max_len) {
        lua_pop(L, 1);
        return lua_ble_push_err_string(L, "ble_gatt_data_too_long");
    }
    lua_ble_runtime_lock();
    memcpy(chr->value, data, len);
    chr->value_len = (uint16_t)len;
    lua_ble_runtime_unlock();
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

int lua_ble_gatts_stats_char(lua_State *L, int opts_index)
{
    lua_ble_gatts_char_t *chr = NULL;
    uint8_t *value = NULL;
    uint16_t value_len;
    int rc;

    if (!s_gatts_active) {
        return lua_ble_push_err_string(L, "ble_invalid_state");
    }
    lua_getfield(L, opts_index, "char");
    luaL_checktype(L, -1, LUA_TTABLE);
    rc = lua_ble_gatts_find_char_from_table(L, lua_gettop(L), "characteristic_id", &chr);
    lua_pop(L, 1);
    if (rc != 0) {
        return rc;
    }
    value = heap_caps_malloc_prefer(LUA_BLE_GATTS_VALUE_MAX,
                                    2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!value) {
        return lua_ble_push_err_string(L, "ble_no_mem");
    }
    lua_ble_runtime_lock();
    value_len = chr->value_len;
    if (value_len > 0) {
        memcpy(value, chr->value, value_len);
    }
    lua_ble_runtime_unlock();
    lua_newtable(L);
    lua_pushlstring(L, (const char *)value, value_len);
    heap_caps_free(value);
    lua_setfield(L, -2, "value");
    return 1;
}

static int lua_ble_gatts_resolve_conn_index(lua_State *L, int table_index, uint8_t *out)
{
    lua_getfield(L, table_index, "conn_index");
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

static int lua_ble_gatts_prepare_send(lua_State *L, bool indicate, uint8_t *conn_index,
                                      lua_ble_gatts_char_t **chr)
{
    int rc;
    uint16_t conn_handle;

    if (!s_stack_inited || !s_host_synced) {
        return lua_ble_push_err_string(L, "ble_init_not_initialized");
    }
    if (!s_gatts_active) {
        return lua_ble_push_err_string(L, "ble_invalid_state");
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    rc = lua_ble_gatts_resolve_conn_index(L, 1, conn_index);
    if (rc != 0) {
        return rc;
    }
    rc = lua_ble_gatts_find_char_from_table(L, 1, "characteristic", chr);
    if (rc != 0) {
        return rc;
    }
    if (indicate && !((*chr)->flags & BLE_GATT_CHR_F_INDICATE)) {
        return lua_ble_push_err_string(L, "ble_gatt_not_subscribed");
    }
    if (!indicate && !((*chr)->flags & BLE_GATT_CHR_F_NOTIFY)) {
        return lua_ble_push_err_string(L, "ble_gatt_not_subscribed");
    }
    if (indicate && !lua_module_ble_is_indicate_subscribed(*conn_index, (*chr)->val_handle)) {
        return lua_ble_push_err_string(L, "ble_gatt_not_subscribed");
    }
    if (!indicate && !lua_module_ble_is_notify_subscribed(*conn_index, (*chr)->val_handle)) {
        return lua_ble_push_err_string(L, "ble_gatt_not_subscribed");
    }
    lua_ble_runtime_lock();
    if (indicate && (*chr)->indicate_in_progress[*conn_index]) {
        lua_ble_runtime_unlock();
        return lua_ble_push_err_string(L, "ble_gatt_indicate_in_progress");
    }
    if (!s_conns[*conn_index].connected) {
        lua_ble_runtime_unlock();
        return lua_ble_push_err_string(L, "ble_conn_not_found");
    }
    conn_handle = s_conns[*conn_index].conn_handle;
    lua_ble_runtime_unlock();
    if (((*chr)->read_encrypted || (*chr)->write_encrypted ||
         (*chr)->read_authenticated || (*chr)->write_authenticated) &&
        lua_ble_gatts_check_security(conn_handle,
                                     (*chr)->read_encrypted || (*chr)->write_encrypted,
                                     (*chr)->read_authenticated || (*chr)->write_authenticated) != 0) {
        return lua_ble_push_err_string(L, "ble_smp_security_failed");
    }
    return 0;
}

static int lua_ble_gatts_send_payload(lua_State *L, bool indicate, uint8_t conn_index,
                                      lua_ble_gatts_char_t *chr)
{
    size_t len;
    const char *data;
    int rc;
    int last_rc = 0;
    uint16_t attr_handle;
    uint16_t conn_handle;
    uint16_t max_len;
    uint16_t mtu;

    lua_getfield(L, 1, "data");
    data = luaL_checklstring(L, -1, &len);
    lua_ble_runtime_lock();
    if (!s_conns[conn_index].connected) {
        lua_ble_runtime_unlock();
        lua_pop(L, 1);
        return lua_ble_push_err_string(L, "ble_conn_not_found");
    }
    conn_handle = s_conns[conn_index].conn_handle;
    mtu = s_conns[conn_index].mtu;
    attr_handle = chr->val_handle;
    max_len = chr->max_len;
    lua_ble_runtime_unlock();
    uint16_t max_payload = mtu > 3 ? mtu - 3 : 20;
    if (len > max_payload || len > max_len) {
        lua_pop(L, 1);
        return lua_ble_push_err_string(L, "ble_gatt_data_too_long");
    }
    for (int attempt = 0; attempt <= LUA_BLE_NOTIFY_SEND_RETRY_MAX; attempt++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (!om) {
            lua_pop(L, 1);
            ESP_LOGE(TAG, "[gatt_send]: mbuf alloc failed conn_index=%u attr_handle=%u len=%u indicate=%u",
                     conn_index, attr_handle, (unsigned)len, indicate);
            return lua_ble_push_err_string(L, "ble_mbuf_alloc_failed");
        }

        if (indicate) {
            rc = ble_gatts_indicate_custom(conn_handle, attr_handle, om);
        } else {
            rc = ble_gatts_notify_custom(conn_handle, attr_handle, om);
        }
        if (rc == 0) {
            if (indicate) {
                lua_ble_runtime_lock();
                chr->indicate_in_progress[conn_index] = true;
                lua_ble_runtime_unlock();
            }
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
            return 1;
        }

        last_rc = rc;
        os_mbuf_free_chain(om);
        if (attempt < LUA_BLE_NOTIFY_SEND_RETRY_MAX) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    lua_pop(L, 1);
    ESP_LOGE(TAG, "[gatt_send]: failed conn_index=%u conn_handle=%u attr_handle=%u len=%u indicate=%u rc=%d retries=%d",
             conn_index, conn_handle, attr_handle, (unsigned)len, indicate, last_rc,
             LUA_BLE_NOTIFY_SEND_RETRY_MAX);
    return lua_ble_push_err_string(L, "ble_gatt_send_failed");
}

static int lua_ble_gatts_send(lua_State *L, bool indicate)
{
    lua_ble_gatts_char_t *chr = NULL;
    uint8_t conn_index = 0;
    int rc;

    rc = lua_ble_gatts_prepare_send(L, indicate, &conn_index, &chr);
    if (rc != 0) {
        return rc;
    }
    return lua_ble_gatts_send_payload(L, indicate, conn_index, chr);
}

int lua_ble_notify(lua_State *L)
{
    return lua_ble_gatts_send(L, false);
}

int lua_ble_indicate(lua_State *L)
{
    return lua_ble_gatts_send(L, true);
}

static lua_ble_gatts_char_t *lua_ble_gatts_find_char_by_handle(uint16_t attr_handle)
{
    if (!s_gatts_chars) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_gatts_char_count; i++) {
        if (s_gatts_chars[i].val_handle == attr_handle) {
            return &s_gatts_chars[i];
        }
    }
    return NULL;
}

void lua_ble_gatts_on_notify_tx(uint16_t conn_handle, uint16_t attr_handle, bool indication, int status)
{
    int conn_index = lua_ble_find_conn_index_by_handle(conn_handle);
    lua_ble_gatts_char_t *chr = lua_ble_gatts_find_char_by_handle(attr_handle);

    if (conn_index < 0 || !chr) {
        return;
    }
    if (indication && status == 0) {
        return;
    }
    if (indication) {
        lua_ble_runtime_lock();
        chr->indicate_in_progress[conn_index] = false;
        lua_ble_runtime_unlock();
    }
    lua_ble_event_t *event = lua_ble_event_alloc();
    if (!event) {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d",
                 indication ? LUA_BLE_EVENT_INDICATE_COMPLETE : LUA_BLE_EVENT_NOTIFY_COMPLETE);
        return;
    }
    event->type = indication ? LUA_BLE_EVENT_INDICATE_COMPLETE : LUA_BLE_EVENT_NOTIFY_COMPLETE;
    event->conn_index = (uint8_t)conn_index;
    event->conn_handle = conn_handle;
    event->error_code = (status == 0 || status == BLE_HS_EDONE) ? 0 : status;
    strlcpy(event->status, event->error_code == 0 ? "ok" : "error", sizeof(event->status));
    lua_ble_gatts_fill_char_event(event, chr);
    lua_ble_event_enqueue(event);
}
