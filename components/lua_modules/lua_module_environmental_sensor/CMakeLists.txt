idf_component_register(
    SRCS "src/lua_module_environmental_sensor.c"
    INCLUDE_DIRS "src"
    REQUIRES cap_lua
    PRIV_REQUIRES lua_driver_i2c esp_driver_gpio esp_rom freertos
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
