idf_component_register(
    SRCS
        "src/lua_module_uart.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cap_lua
        driver
)

