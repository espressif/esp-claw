set(env_sensor_srcs
    "src/lua_module_environmental_sensor.c"
)

set(env_sensor_requires
    cap_lua
)

set(env_sensor_priv_requires
    espressif__esp_board_manager
)

if(CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690)
    list(APPEND env_sensor_priv_requires
        espressif__bme690
        espressif__i2c_bus
    )
endif()

idf_component_register(
    SRCS
        ${env_sensor_srcs}
    INCLUDE_DIRS
        "src"
    REQUIRES
        ${env_sensor_requires}
    PRIV_REQUIRES
        ${env_sensor_priv_requires}
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
