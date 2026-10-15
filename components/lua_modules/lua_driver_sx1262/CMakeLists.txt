idf_component_register(
    SRCS "src/lua_driver_sx1262.c"
    INCLUDE_DIRS "src"
    REQUIRES cap_lua driver nicolaielectronics__sx126x
)
