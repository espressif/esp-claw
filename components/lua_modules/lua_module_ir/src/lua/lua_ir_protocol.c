/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_ir_protocol.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_paths.h"
#include "esp_log.h"
#include "ir_builtin.h"
#include "ir_protocol_json.h"
#include "lauxlib.h"
#include "lua_ir_common.h"
#include "lua_ir_frame.h"

#define LUA_IR_PROTOCOL_METATABLE "ir.protocol"
#define LUA_IR_PROTOCOL_DATA_DIR "ir/protocols"
#define LUA_IR_PROTOCOL_SYSTEM_BUNDLE "ir/protocols.jsonl"
/* Source path; ir_protocol_open() appends "/<id>.json" to a directory within its own 256-byte path. */
#define LUA_IR_PATH_MAX 192
#define LUA_IR_SOURCE_COUNT 2
/* A protocol description nests about 8 tables deep. */
#define LUA_IR_DESC_DEPTH_MAX 16

static const char *TAG = "lua_ir";
/* Its address is ir.protocol.none, the JSON null of a description, e.g. an unsupported raw value. */
static const char s_none;
/* Its address keys the LUA_REGISTRYINDEX table of this Lua state's registered protocols: id -> object, plus 1..n in registration order. */
static const char s_registered;

typedef struct {
    const ir_protocol_t *protocol;
    ir_protocol_t *owned;               /* parsed protocol released on GC, NULL for a builtin */
} lua_ir_protocol_t;

/* DATA <id>.json files first, so a user file overrides the firmware bundle. */
typedef struct {
    char path[LUA_IR_SOURCE_COUNT][LUA_IR_PATH_MAX];
    const char *sources[LUA_IR_SOURCE_COUNT];
    size_t count;
} protocol_sources_t;

typedef struct {
    lua_State *L;
    int list;
    int filter;
    int hide;                           /* stack index of the registered table hiding JSON ids, 0 for none */
    bool failed;
} protocol_list_ctx_t;

/* Lua table to cJSON conversion state; error is a static message, type the offending Lua type if any. */
typedef struct {
    lua_State *L;
    const char *error;
    const char *type;
} desc_ctx_t;

static protocol_sources_t *sources_create(void)
{
    static const claw_path_root_t roots[LUA_IR_SOURCE_COUNT] = {CLAW_PATH_DATA, CLAW_PATH_SYSTEM};
    static const char *const subpaths[LUA_IR_SOURCE_COUNT] = {LUA_IR_PROTOCOL_DATA_DIR, LUA_IR_PROTOCOL_SYSTEM_BUNDLE};
    protocol_sources_t *s = calloc(1, sizeof(*s));
    for (size_t i = 0; s && i < LUA_IR_SOURCE_COUNT; ++i) {
        esp_err_t err = claw_paths_join(roots[i], subpaths[i], s->path[i], LUA_IR_PATH_MAX);
        if (err == ESP_OK) {
            s->sources[s->count++] = s->path[i];
        } else {
            ESP_LOGW(TAG, "protocol source %s skipped: %s", subpaths[i], esp_err_to_name(err));
        }
    }
    return s;
}

static void registered_push(lua_State *L)
{
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &s_registered) == LUA_TTABLE) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &s_registered);
}

static lua_ir_protocol_t *protocol_new(lua_State *L)
{
    lua_ir_protocol_t *p = lua_newuserdata(L, sizeof(*p));
    p->protocol = NULL;
    p->owned = NULL;
    luaL_setmetatable(L, LUA_IR_PROTOCOL_METATABLE);
    return p;
}

/* Pushes a protocol object: a builtin id first, then DATA <id>.json, then the SYSTEM bundle. */
static const ir_protocol_t *protocol_push(lua_State *L, const char *id, const char *context)
{
    lua_ir_protocol_t *p = protocol_new(L);
    p->protocol = ir_builtin_find(id);
    if (p->protocol) return p->protocol;
    if (!ir_protocol_id_valid(id)) luaL_error(L, "%s: unknown protocol '%s'", context, id);
    protocol_sources_t *sources = sources_create();
    if (!sources) luaL_error(L, "%s: out of memory", context);
    esp_err_t err = ir_protocol_open(sources->sources, sources->count, id, &p->owned);
    free(sources);
    if (err == ESP_ERR_NOT_FOUND) luaL_error(L, "%s: unknown protocol '%s'", context, id);
    if (err != ESP_OK) luaL_error(L, "%s: protocol '%s': %s", context, id, lua_ir_error_text(err));
    p->protocol = p->owned;
    return p->protocol;
}

static const ir_protocol_t *protocol_check(lua_State *L, int index)
{
    const ir_protocol_t *protocol = ((lua_ir_protocol_t *)luaL_checkudata(L, index, LUA_IR_PROTOCOL_METATABLE))->protocol;
    if (!protocol) luaL_error(L, "ir.protocol: protocol is closed");
    return protocol;
}

static int protocol_gc(lua_State *L)
{
    lua_ir_protocol_t *p = luaL_checkudata(L, 1, LUA_IR_PROTOCOL_METATABLE);
    ir_protocol_free(p->owned);
    p->owned = NULL;
    p->protocol = NULL;
    return 0;
}

static void set_string(lua_State *L, const char *key, const char *value)
{
    if (!value) return;
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
}

static void push_field_names(lua_State *L, uint32_t fields)
{
    lua_newtable(L);
    lua_Integer n = 0;
    for (unsigned i = 0; i < IR_FIELD_COUNT; ++i) {
        if (!(fields & IR_FIELD_BIT(i))) continue;
        lua_pushstring(L, ir_field_info((ir_field_id_t)i)->name);
        lua_rawseti(L, -2, ++n);
    }
}

static void push_info(lua_State *L, const ir_protocol_t *p)
{
    bool decode = p->flags & IR_PROTOCOL_DECODE;
    lua_createtable(L, 0, 10);
    set_string(L, "id", p->id);
    lua_pushinteger(L, p->carrier_hz);
    lua_setfield(L, -2, "carrier_hz");
    lua_pushboolean(L, decode);
    lua_setfield(L, -2, "decode");
    lua_pushboolean(L, decode && !(p->flags & IR_PROTOCOL_UNCHECKED));
    lua_setfield(L, -2, "checked");
    push_field_names(L, ir_protocol_fields(p));
    lua_setfield(L, -2, "fields");
    if (!p->meta) return;
    set_string(L, "category", p->meta->category);
    set_string(L, "brand", p->meta->brand);
    set_string(L, "model", p->meta->model);
    set_string(L, "source", p->meta->source);
    set_string(L, "maturity", p->meta->maturity);
}

static bool filter_matches(lua_State *L, int filter, const char *key, const char *value)
{
    lua_getfield(L, filter, key);
    bool match = lua_isnil(L, -1) || (value && lua_type(L, -1) == LUA_TSTRING && !strcmp(lua_tostring(L, -1), value));
    lua_pop(L, 1);
    return match;
}

/* Runs under lua_pcall: (list, filter, protocol, registered table hiding its ids or nil). */
static int append_protocol(lua_State *L)
{
    const ir_protocol_t *p = lua_touserdata(L, 3);
    if (lua_istable(L, 4) && lua_getfield(L, 4, p->id) != LUA_TNIL) return 0;
    const ir_protocol_meta_t *meta = p->meta;
    if (!lua_isnil(L, 2) && (!filter_matches(L, 2, "category", meta ? meta->category : NULL) || !filter_matches(L, 2, "brand", meta ? meta->brand : NULL))) return 0;
    push_info(L, p);
    lua_rawseti(L, 1, (lua_Integer)lua_rawlen(L, 1) + 1);
    return 0;
}

static bool append_protected(protocol_list_ctx_t *ctx, const ir_protocol_t *protocol)
{
    lua_State *L = ctx->L;
    bool ok = lua_checkstack(L, 5);
    if (ok) {
        lua_pushcfunction(L, append_protocol);
        lua_pushvalue(L, ctx->list);
        lua_pushvalue(L, ctx->filter);
        lua_pushlightuserdata(L, (void *)protocol);
        if (ctx->hide) {
            lua_pushvalue(L, ctx->hide);
        } else {
            lua_pushnil(L);
        }
        ok = lua_pcall(L, 4, 0, 0) == LUA_OK;
        if (!ok) lua_pop(L, 1);
    }
    if (!ok) ctx->failed = true;
    return ok;
}

/* ir_protocol_list() only reports protocols that ir.protocol.get(id) can open; builtin and registered ids hide JSON ones. */
static bool list_json(const ir_protocol_t *protocol, void *user_ctx)
{
    return ir_builtin_find(protocol->id) ? true : append_protected(user_ctx, protocol);
}

static int lua_ir_protocol_list(lua_State *L)
{
    static const char *const options[] = {"category", "brand"};
    if (!lua_isnoneornil(L, 1)) lua_ir_check_keys(L, 1, "ir.protocol.list", options, 2);
    lua_settop(L, 1);
    lua_newtable(L);
    registered_push(L);
    protocol_list_ctx_t ctx = {.L = L, .list = 2, .filter = 1};
    size_t count;
    const ir_protocol_t *const *builtins = ir_builtin_protocols(&count);
    for (size_t i = 0; i < count && !ctx.failed; ++i) append_protected(&ctx, builtins[i]);
    lua_Integer registered = (lua_Integer)lua_rawlen(L, 3);
    for (lua_Integer i = 1; i <= registered && !ctx.failed; ++i) {
        lua_rawgeti(L, 3, i);
        const ir_protocol_t *protocol = ((lua_ir_protocol_t *)lua_touserdata(L, -1))->protocol;
        lua_pop(L, 1);
        if (protocol) append_protected(&ctx, protocol);
    }
    ctx.hide = 3;
    protocol_sources_t *sources = ctx.failed ? NULL : sources_create();
    esp_err_t err = sources ? ir_protocol_list(sources->sources, sources->count, list_json, &ctx) : ESP_ERR_NO_MEM;
    free(sources);
    if (ctx.failed || err != ESP_OK) return luaL_error(L, "ir.protocol.list: %s", lua_ir_error_text(ctx.failed ? ESP_ERR_NO_MEM : err));
    lua_settop(L, ctx.list);
    return 1;
}

static uint32_t read_hold_ms(lua_State *L, int index, const char *context)
{
    static const char *const options[] = {"hold_ms"};
    if (lua_isnoneornil(L, index)) return 0;
    lua_ir_check_keys(L, index, context, options, 1);
    lua_getfield(L, index, "hold_ms");
    int64_t hold_ms = lua_isnil(L, -1) ? 0 : lua_isinteger(L, -1) ? (int64_t)lua_tointeger(L, -1) : -1;
    if (hold_ms < 0 || hold_ms > UINT32_MAX) luaL_error(L, "%s: hold_ms must be a non-negative integer", context);
    lua_pop(L, 1);
    return (uint32_t)hold_ms;
}

/* Registered protocols never use builtin ids, so looking them up first still keeps builtin tables first. */
static int lua_ir_protocol_get(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    registered_push(L);
    if (lua_getfield(L, -1, id) == LUA_TUSERDATA) return 1;
    lua_pop(L, 2);
    protocol_push(L, id, "ir.protocol.get");
    return 1;
}

static cJSON *desc_fail(desc_ctx_t *ctx, const char *error)
{
    ctx->error = error;
    return NULL;
}

static bool desc_is_empty(lua_State *L, int index)
{
    index = lua_absindex(L, index);
    if (!lua_istable(L, index)) return false;
    lua_pushnil(L);
    if (!lua_next(L, index)) return true;
    lua_pop(L, 2);
    return false;
}

/* Length of a 1..n sequence, or 0 when the table has any other key. */
static lua_Integer desc_sequence_length(lua_State *L, int index)
{
    lua_Integer count = 0, max = 0;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        lua_pop(L, 1);
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1) {
            lua_pop(L, 1);
            return 0;
        }
        lua_Integer key = lua_tointeger(L, -1);
        max = key > max ? key : max;
        count++;
    }
    return count == max ? count : 0;
}

static cJSON *desc_value(desc_ctx_t *ctx, int index, int depth);

static cJSON *desc_array(desc_ctx_t *ctx, int index, lua_Integer length, int depth)
{
    lua_State *L = ctx->L;
    cJSON *array = cJSON_CreateArray();
    if (!array) return desc_fail(ctx, "out of memory");
    for (lua_Integer i = 1; i <= length; ++i) {
        lua_rawgeti(L, index, i);
        cJSON *item = desc_value(ctx, -1, depth + 1);
        lua_pop(L, 1);
        if (!item || !cJSON_AddItemToArray(array, item)) {
            if (item) ctx->error = "out of memory";
            cJSON_Delete(item);
            cJSON_Delete(array);
            return NULL;
        }
    }
    return array;
}

/* String keys only; an empty table member means the key is absent. */
static cJSON *desc_object(desc_ctx_t *ctx, int index, int depth)
{
    lua_State *L = ctx->L;
    cJSON *object = cJSON_CreateObject();
    if (!object) return desc_fail(ctx, "out of memory");
    lua_pushnil(L);
    while (lua_next(L, index)) {
        cJSON *item = NULL;
        bool ok = lua_type(L, -2) == LUA_TSTRING;
        if (!ok) {
            ctx->error = "table keys must be strings, or 1..n for a list";
        } else if (!desc_is_empty(L, -1)) {
            item = desc_value(ctx, -1, depth + 1);
            ok = item && cJSON_AddItemToObject(object, lua_tostring(L, -2), item);
            if (item && !ok) ctx->error = "out of memory";
        }
        if (!ok) {
            cJSON_Delete(item);
            lua_pop(L, 2);
            cJSON_Delete(object);
            return NULL;
        }
        lua_pop(L, 1);
    }
    return object;
}

/* Builds the cJSON tree without raising, so a failure frees the partial tree before the Lua error. */
static cJSON *desc_value(desc_ctx_t *ctx, int index, int depth)
{
    lua_State *L = ctx->L;
    index = lua_absindex(L, index);
    int type = lua_type(L, index);
    cJSON *item = NULL;
    switch (type) {
    case LUA_TBOOLEAN:
        item = cJSON_CreateBool(lua_toboolean(L, index));
        break;
    case LUA_TNUMBER:
        item = cJSON_CreateNumber(lua_isinteger(L, index) ? (double)lua_tointeger(L, index) : (double)lua_tonumber(L, index));
        break;
    case LUA_TSTRING:
        item = cJSON_CreateString(lua_tostring(L, index));
        break;
    case LUA_TTABLE: {
        /* desc_object() holds a key/value pair plus desc_is_empty()'s pair on each level. */
        if (depth >= LUA_IR_DESC_DEPTH_MAX || !lua_checkstack(L, 4)) return desc_fail(ctx, "tables nested too deep");
        lua_Integer length = desc_sequence_length(L, index);
        return length ? desc_array(ctx, index, length, depth) : desc_object(ctx, index, depth);
    }
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(L, index) == &s_none) {
            item = cJSON_CreateNull();
            break;
        }
        /* fall through */
    default:
        ctx->type = lua_typename(L, type);
        return desc_fail(ctx, "unsupported value");
    }
    return item ? item : desc_fail(ctx, "out of memory");
}

/* Builds an encode only protocol from a table in the JSON protocol format and registers it in this Lua state only. */
static int lua_ir_protocol_register_desc(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_ir_protocol_t *p = protocol_new(L);
    desc_ctx_t ctx = {.L = L};
    cJSON *root = desc_value(&ctx, 1, 0);
    bool converted = root != NULL;
    esp_err_t err = converted ? ir_protocol_parse_object(root, &p->owned) : ESP_FAIL;
    cJSON_Delete(root);
    if (!converted && ctx.type) return luaL_error(L, "ir.protocol.register: unsupported %s value", ctx.type);
    if (!converted) return luaL_error(L, "ir.protocol.register: %s", ctx.error);
    if (err != ESP_OK) return luaL_error(L, "ir.protocol.register: %s", lua_ir_error_text(err));
    p->protocol = p->owned;
    const char *id = p->protocol->id;
    if (ir_builtin_find(id)) return luaL_error(L, "ir.protocol.register: '%s' is a builtin protocol id", id);
    registered_push(L);
    if (lua_getfield(L, -1, id) != LUA_TNIL) return luaL_error(L, "ir.protocol.register: '%s' is already registered", id);
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    lua_setfield(L, 3, id);
    lua_pushvalue(L, 2);
    lua_rawseti(L, 3, (lua_Integer)lua_rawlen(L, 3) + 1);
    lua_settop(L, 2);
    return 1;
}

static int protocol_info(lua_State *L)
{
    push_info(L, protocol_check(L, 1));
    return 1;
}

static int protocol_encode(lua_State *L)
{
    const ir_protocol_t *protocol = protocol_check(L, 1);
    ir_values_t values;
    lua_ir_values_read(L, 2, "protocol:encode", &values);
    uint32_t hold_ms = read_hold_ms(L, 3, "protocol:encode");
    if ((ir_protocol_fields(protocol) & IR_FIELD_BIT(IR_FIELD_TOGGLE)) && !ir_values_has(&values, IR_FIELD_TOGGLE)) ir_values_set(&values, IR_FIELD_TOGGLE, 0);
    uint32_t holds = ir_protocol_hold_count(protocol, &values, hold_ms);
    ir_frame_t *frame = lua_ir_frame_create(L, ir_protocol_max_pulses(protocol, holds));
    esp_err_t err = ir_protocol_encode(protocol, &values, holds, frame);
    if (err != ESP_OK) return luaL_error(L, "protocol:encode: %s", lua_ir_error_text(err));
    return 1;
}

/* True when every value is within the codeset limits of the values' mode. */
static int protocol_check_values(lua_State *L)
{
    const ir_protocol_t *protocol = protocol_check(L, 1);
    ir_values_t values;
    lua_ir_values_read(L, 2, "protocol:check", &values);
    lua_pushboolean(L, ir_protocol_check(protocol, &values) == ESP_OK);
    return 1;
}

static lua_Integer read_offset(lua_State *L, int index)
{
    static const char *const options[] = {"offset"};
    if (lua_isnoneornil(L, index)) return 1;
    lua_ir_check_keys(L, index, "protocol:decode", options, 1);
    lua_getfield(L, index, "offset");
    lua_Integer offset = luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);
    return offset;
}

/* Returns values, info, or nil, reason. A codeset only reports values within its own limits. */
static int protocol_decode(lua_State *L)
{
    const ir_protocol_t *protocol = protocol_check(L, 1);
    const ir_frame_t *frame = lua_ir_frame_check(L, 2);
    lua_Integer offset = read_offset(L, 3);
    if (offset < 1 || (size_t)offset > frame->count) return luaL_error(L, "protocol:decode: offset must be 1-%d", (int)frame->count);
    if (!(protocol->flags & IR_PROTOCOL_DECODE)) {
        lua_pushnil(L);
        lua_pushliteral(L, "not_decodable");
        return 2;
    }
    ir_decode_result_t result;
    esp_err_t err = ir_protocol_decode(protocol, frame, (size_t)offset - 1, &result);
    if (err == ESP_OK) err = ir_protocol_check(protocol, &result.values);
    if (err != ESP_OK) {
        const char *reason = lua_ir_reason(err);
        if (!reason) return luaL_error(L, "protocol:decode: %s", lua_ir_error_text(err));
        lua_pushnil(L);
        lua_pushstring(L, reason);
        return 2;
    }
    lua_createtable(L, 0, IR_FIELD_COUNT);
    lua_ir_values_write(L, &result.values);
    lua_createtable(L, 0, 5);
    lua_pushstring(L, result.message->name);
    lua_setfield(L, -2, "message");
    lua_pushinteger(L, result.copies);
    lua_setfield(L, -2, "copies");
    lua_pushinteger(L, result.holds);
    lua_setfield(L, -2, "holds");
    lua_pushboolean(L, !(protocol->flags & IR_PROTOCOL_UNCHECKED));
    lua_setfield(L, -2, "checked");
    lua_pushinteger(L, (lua_Integer)result.next + 1);
    lua_setfield(L, -2, "next");
    return 2;
}

static const char *type_name(ir_field_type_t type)
{
    switch (type) {
    case IR_FIELD_TYPE_BOOL: return "boolean";
    case IR_FIELD_TYPE_ENUM: return "enum";
    case IR_FIELD_TYPE_DECI: return "number";
    default: return "integer";
    }
}

static void push_value_list(lua_State *L, ir_field_id_t id, uint32_t mask)
{
    lua_newtable(L);
    lua_Integer n = 0;
    for (int32_t v = 0; v < IR_FIELD_ENUM_MAX; ++v) {
        if (!((mask >> v) & 1U)) continue;
        lua_ir_value_push(L, id, v);
        lua_rawseti(L, -2, ++n);
    }
}

static void set_value(lua_State *L, const char *key, ir_field_id_t id, int32_t value)
{
    lua_ir_value_push(L, id, value);
    lua_setfield(L, -2, key);
}

/* {type, values | min, max, step, default}; range keys are absent when the codeset does not limit the field. */
static void push_field_caps(lua_State *L, const ir_protocol_t *protocol, ir_field_id_t id, int32_t mode)
{
    ir_field_type_t type = ir_field_info(id)->type;
    ir_limit_t limit;
    lua_createtable(L, 0, 5);
    lua_pushstring(L, type_name(type));
    lua_setfield(L, -2, "type");
    if (ir_protocol_get_limit(protocol, id, mode, &limit)) {
        if (type == IR_FIELD_TYPE_BOOL || type == IR_FIELD_TYPE_ENUM) {
            push_value_list(L, id, limit.mask);
            lua_setfield(L, -2, "values");
        } else {
            set_value(L, "min", id, limit.min);
            set_value(L, "max", id, limit.max);
            set_value(L, "step", id, limit.step);
        }
    }
    if (protocol->meta && ir_values_has(&protocol->meta->defaults, id)) set_value(L, "default", id, protocol->meta->defaults.value[id]);
}

/* Function keys usable in mode: the codeset key limit, or every key name when the field is unlimited. */
static void push_function_keys(lua_State *L, const ir_protocol_t *protocol, int32_t mode)
{
    ir_limit_t limit;
    uint32_t mask = 0;
    if (ir_protocol_fields(protocol) & IR_FIELD_BIT(IR_FIELD_KEY)) {
        mask = ir_protocol_get_limit(protocol, IR_FIELD_KEY, mode, &limit) ? limit.mask : (1UL << ir_field_info(IR_FIELD_KEY)->enum_count) - 1U;
    }
    push_value_list(L, IR_FIELD_KEY, mask);
}

static int protocol_capabilities(lua_State *L)
{
    static const char *const options[] = {"mode"};
    const ir_protocol_t *protocol = protocol_check(L, 1);
    int32_t mode = -1;
    if (!lua_isnoneornil(L, 2)) {
        ir_values_t context;
        lua_ir_check_keys(L, 2, "protocol:capabilities", options, 1);
        lua_ir_values_read(L, 2, "protocol:capabilities", &context);
        if (ir_values_has(&context, IR_FIELD_MODE)) mode = context.value[IR_FIELD_MODE];
    }
    uint32_t state_fields = ir_protocol_fields(protocol) & IR_FIELD_STATE_MASK;
    lua_createtable(L, 0, 2);
    lua_newtable(L);
    for (unsigned i = 0; i < IR_FIELD_COUNT; ++i) {
        ir_limit_t limit;
        if (!(state_fields & IR_FIELD_BIT(i))) continue;
        if (ir_protocol_get_limit(protocol, (ir_field_id_t)i, mode, &limit) && ir_limit_unused(&limit)) continue;
        push_field_caps(L, protocol, (ir_field_id_t)i, mode);
        lua_setfield(L, -2, ir_field_info((ir_field_id_t)i)->name);
    }
    lua_setfield(L, -2, "state");
    push_function_keys(L, protocol, mode);
    lua_setfield(L, -2, "keys");
    return 1;
}

void lua_ir_protocol_register(lua_State *L)
{
    luaL_checktype(L, -1, LUA_TTABLE);
    static const luaL_Reg methods[] = {
        {"info", protocol_info},
        {"capabilities", protocol_capabilities},
        {"check", protocol_check_values},
        {"encode", protocol_encode},
        {"decode", protocol_decode},
        {NULL, NULL},
    };
    if (luaL_newmetatable(L, LUA_IR_PROTOCOL_METATABLE)) {
        lua_pushcfunction(L, protocol_gc);
        lua_setfield(L, -2, "__gc");
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        /* Hides the metatable so scripts cannot call __gc on a live, possibly cached protocol. */
        lua_pushboolean(L, false);
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);
    static const luaL_Reg functions[] = {
        {"list", lua_ir_protocol_list},
        {"get", lua_ir_protocol_get},
        {"register", lua_ir_protocol_register_desc},
        {NULL, NULL},
    };
    luaL_newlib(L, functions);
    lua_pushlightuserdata(L, (void *)&s_none);
    lua_setfield(L, -2, "none");
    lua_setfield(L, -2, "protocol");
}
