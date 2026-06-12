/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

char *audio_strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *out = malloc(len);
    if (out) {
        memcpy(out, s, len);
    }
    return out;
}

bool audio_format_equal(const audio_format_t *a, const audio_format_t *b)
{
    return a->sample_rate == b->sample_rate && a->channels == b->channels && a->bits == b->bits;
}

esp_err_t audio_format_complete(audio_format_t *fmt)
{
    if (fmt->sample_rate == 0 || fmt->channels == 0 || fmt->bits == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fmt->bits != 8 && fmt->bits != 16 && fmt->bits != 24 && fmt->bits != 32) {
        return ESP_ERR_INVALID_ARG;
    }
    fmt->bytes_per_frame = (uint8_t)(fmt->channels * (fmt->bits / 8));
    if (fmt->bytes_per_frame == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void audio_format_log(char *buf, size_t len, const audio_format_t *fmt)
{
    snprintf(buf, len, "%" PRIu32 "/%u/%u", fmt->sample_rate, fmt->channels, fmt->bits);
}

uint8_t lua_audio_get_u8_field(lua_State *L, int idx, const char *name, int raw_index, uint8_t def)
{
    lua_Integer value = 0;
    bool has_value = false;

    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        value = luaL_checkinteger(L, -1);
        has_value = true;
    }
    lua_pop(L, 1);

    if (!has_value && raw_index > 0) {
        lua_rawgeti(L, idx, raw_index);
        if (!lua_isnil(L, -1)) {
            value = luaL_checkinteger(L, -1);
            has_value = true;
        }
        lua_pop(L, 1);
    }

    if (!has_value) {
        return def;
    }
    if (value <= 0 || value > UINT8_MAX) {
        luaL_error(L, "audio %s must be a positive integer", name);
        return 0;
    }
    return (uint8_t)value;
}

uint32_t lua_audio_get_u32_field(lua_State *L, int idx, const char *name, int raw_index, uint32_t def)
{
    lua_Integer value = 0;
    bool has_value = false;

    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        value = luaL_checkinteger(L, -1);
        has_value = true;
    }
    lua_pop(L, 1);

    if (!has_value && raw_index > 0) {
        lua_rawgeti(L, idx, raw_index);
        if (!lua_isnil(L, -1)) {
            value = luaL_checkinteger(L, -1);
            has_value = true;
        }
        lua_pop(L, 1);
    }

    if (!has_value) {
        return def;
    }
    if (value <= 0 || value > UINT32_MAX) {
        luaL_error(L, "audio %s must be a positive integer", name);
        return 0;
    }
    return (uint32_t)value;
}

bool lua_audio_parse_format_table(lua_State *L, int idx, audio_format_t *fmt, bool allow_missing)
{
    memset(fmt, 0, sizeof(*fmt));
    if (lua_isnoneornil(L, idx)) {
        return allow_missing;
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    fmt->sample_rate = lua_audio_get_u32_field(L, idx, "sample_rate", 2, 0);
    fmt->channels = lua_audio_get_u8_field(L, idx, "channels", 3, 0);
    fmt->bits = lua_audio_get_u8_field(L, idx, "bits", 4, 0);
    if (audio_format_complete(fmt) != ESP_OK) {
        luaL_error(L, "audio format must include valid sample_rate, channels, and bits");
        return false;
    }
    return true;
}

void *lua_audio_get_codec_field(lua_State *L, int idx)
{
    void *codec = NULL;

    lua_getfield(L, idx, "codec");
    if (!lua_isnil(L, -1)) {
        codec = lua_touserdata(L, -1);
    }
    lua_pop(L, 1);
    if (!codec) {
        lua_rawgeti(L, idx, 1);
        codec = lua_touserdata(L, -1);
        lua_pop(L, 1);
    }
    return codec;
}

int lua_audio_get_int_field(lua_State *L, int idx, const char *name, int def)
{
    int value = def;
    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        value = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

void lua_audio_push_format(lua_State *L, const audio_format_t *fmt)
{
    lua_newtable(L);
    lua_pushinteger(L, fmt->sample_rate);
    lua_setfield(L, -2, "sample_rate");
    lua_pushinteger(L, fmt->channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, fmt->bits);
    lua_setfield(L, -2, "bits");
    lua_pushinteger(L, fmt->bytes_per_frame);
    lua_setfield(L, -2, "bytes_per_frame");
}

int lua_audio_push_error(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

int lua_audio_push_errorf(lua_State *L, const char *fmt, ...)
{
    va_list ap;
    lua_pushnil(L);
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    return 2;
}

audio_device_t *lua_audio_check_device(lua_State *L, int idx, audio_device_kind_t kind, const char *what)
{
    audio_device_t *dev = NULL;

    if (kind == AUDIO_DEVICE_INPUT) {
        dev = (audio_device_t *)luaL_checkudata(L, idx, AUDIO_DEVICE_INPUT_META);
    } else {
        dev = (audio_device_t *)luaL_checkudata(L, idx, AUDIO_DEVICE_OUTPUT_META);
    }
    if (!dev || dev->closed || dev->codec_dev == NULL) {
        luaL_error(L, "audio %s: invalid or closed device", what);
        return NULL;
    }
    return dev;
}

audio_player_t *lua_audio_check_player(lua_State *L, int idx, const char *what)
{
    audio_player_t *player = (audio_player_t *)luaL_checkudata(L, idx, AUDIO_PLAYER_META);
    if (!player || player->closed) {
        luaL_error(L, "audio %s: invalid or closed player", what);
        return NULL;
    }
    return player;
}

audio_recorder_t *lua_audio_check_recorder(lua_State *L, int idx, const char *what)
{
    audio_recorder_t *rec = (audio_recorder_t *)luaL_checkudata(L, idx, AUDIO_RECORDER_META);
    if (!rec || rec->closed) {
        luaL_error(L, "audio %s: invalid or closed recorder", what);
        return NULL;
    }
    return rec;
}

audio_analyzer_t *lua_audio_check_analyzer(lua_State *L, int idx, const char *what)
{
    audio_analyzer_t *analyzer = (audio_analyzer_t *)luaL_checkudata(L, idx, AUDIO_ANALYZER_META);
    if (!analyzer || analyzer->closed) {
        luaL_error(L, "audio %s: invalid or closed analyzer", what);
        return NULL;
    }
    return analyzer;
}
