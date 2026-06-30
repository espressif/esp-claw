idf_component_register(
    SRCS
        "src/lua_module_led_strip.c"
    INCLUDE_DIRS
        "src"
    REQUIRES
        lua_runtime
)
