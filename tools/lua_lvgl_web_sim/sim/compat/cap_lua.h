#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lua.h"

esp_err_t cap_lua_register_module(const char *name, lua_CFunction openf);
esp_err_t cap_lua_register_exit_cleanup(void (*cleanup)(lua_State *L));
bool cap_lua_runtime_stop_requested(lua_State *L);
