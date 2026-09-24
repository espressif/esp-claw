set(srcs
    "src/cap_lua.c"
    "src/cap_lua_runtime.c"
    "src/cap_lua_async.c"
)
if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND srcs "src/cmd_cap_lua.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        claw_cap
        claw_core
        console
        esp_timer
        freertos
        georgik__lua
)
