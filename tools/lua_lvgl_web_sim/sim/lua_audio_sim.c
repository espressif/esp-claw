/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Browser audio keeps ownership and scheduling in the Web Audio service. */
#include <stdlib.h>
#include <emscripten.h>
#include "lua.h"
#include "lauxlib.h"

static int audio_call(lua_State *L)
{
    const char *method = luaL_checkstring(L, 1);
    const char *args = luaL_checkstring(L, 2);
    size_t size = 0;
    const char *pcm = luaL_optlstring(L, 3, NULL, &size);
    char *result = (char *)EM_ASM_PTR({
        let result;
        try {
            const data = $2 ? HEAPU8.slice($2, $2 + $3) : null;
            result = {value: Module.__simServices.audio.call(UTF8ToString($0), JSON.parse(UTF8ToString($1)), data)};
        } catch (error) { result = {error: error.message || String(error)}; }
        const text = JSON.stringify(result);
        const ptr = _malloc(lengthBytesUTF8(text) + 1);
        if (ptr) stringToUTF8(text, ptr, lengthBytesUTF8(text) + 1);
        return ptr;
    }, method, args, pcm, size);
    if (!result) return luaL_error(L, "audio: out of memory");
    lua_pushstring(L, result);
    free(result);
    return 1;
}

int luaopen_audio(lua_State *L)
{
    if (luaL_loadfile(L, "/sim/audio.lua") != LUA_OK) return lua_error(L);
    lua_pushcfunction(L, audio_call);
    lua_call(L, 1, 1);
    return 1;
}
