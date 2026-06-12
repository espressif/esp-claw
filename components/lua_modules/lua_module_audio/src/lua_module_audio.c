/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_audio.h"
#include "audio_private.h"
#include "cap_lua.h"

static void lua_audio_create_meta(lua_State *L, const char *name, const luaL_Reg *methods, lua_CFunction gc)
{
    if (luaL_newmetatable(L, name)) {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        luaL_setfuncs(L, methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

int luaopen_audio(lua_State *L)
{
    static const luaL_Reg output_methods[] = {
        {"close",      lua_audio_device_close},
        {"info",       lua_audio_device_info},
        {"set_volume", lua_audio_output_set_volume},
        {"get_volume", lua_audio_output_get_volume},
        {"set_mute",   lua_audio_output_set_mute},
        {"write",      lua_audio_output_write},
        {"play_tone",  lua_audio_output_play_tone},
        {NULL, NULL},
    };
    static const luaL_Reg input_methods[] = {
        {"close",      lua_audio_device_close},
        {"info",       lua_audio_device_info},
        {"set_volume", lua_audio_input_set_volume},
        {"get_volume", lua_audio_input_get_volume},
        {"read",       lua_audio_input_read},
        {NULL, NULL},
    };
    static const luaL_Reg player_methods[] = {
        {"close",  lua_audio_player_close},
        {"play",   lua_audio_player_play},
        {"stop",   lua_audio_player_stop},
        {"pause",  lua_audio_player_pause},
        {"resume", lua_audio_player_resume},
        {"poll",   lua_audio_player_poll},
        {NULL, NULL},
    };
    static const luaL_Reg recorder_methods[] = {
        {"close",  lua_audio_recorder_close},
        {"record", lua_audio_recorder_record},
        {NULL, NULL},
    };
    static const luaL_Reg analyzer_methods[] = {
        {"close",         lua_audio_analyzer_close},
        {"read_level",    lua_audio_analyzer_read_level},
        {"read_spectrum", lua_audio_analyzer_read_spectrum},
        {NULL, NULL},
    };
    static const luaL_Reg funcs[] = {
        {"new_output", lua_audio_new_output},
        {"new_input",  lua_audio_new_input},
        {"player",     lua_audio_player_new},
        {"recorder",   lua_audio_recorder_new},
        {"analyzer",   lua_audio_analyzer_new},
        {NULL, NULL},
    };

    lua_audio_create_meta(L, AUDIO_DEVICE_OUTPUT_META, output_methods, lua_audio_device_gc);
    lua_audio_create_meta(L, AUDIO_DEVICE_INPUT_META, input_methods, lua_audio_device_gc);
    lua_audio_create_meta(L, AUDIO_PLAYER_META, player_methods, lua_audio_player_gc);
    lua_audio_create_meta(L, AUDIO_RECORDER_META, recorder_methods, lua_audio_recorder_gc);
    lua_audio_create_meta(L, AUDIO_ANALYZER_META, analyzer_methods, lua_audio_analyzer_gc);

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

esp_err_t lua_module_audio_register(void)
{
    return cap_lua_register_module("audio", luaopen_audio);
}
