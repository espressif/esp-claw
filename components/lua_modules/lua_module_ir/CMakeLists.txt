idf_component_register(
    SRCS
        "src/lua_module_ir.c"
    INCLUDE_DIRS
        "src"
    REQUIRES
        cap_lua
        esp_board_manager
        esp_driver_rmt
        esp_driver_gpio
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
