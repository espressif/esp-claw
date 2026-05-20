idf_component_register(
    SRCS
        "src/lua_driver_twai.c"
    INCLUDE_DIRS
        "src"
    REQUIRES
        cap_lua
        esp_driver_twai
        freertos
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
