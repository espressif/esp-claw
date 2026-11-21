set(mag_srcs "src/lua_module_magnetometer.c")
set(mag_include_dirs "src")

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350)
    list(APPEND mag_srcs "src/bmm350/bmm350_backend.c")
endif()

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150)
    list(APPEND mag_srcs "src/bmm150/bmm150_backend.c")
endif()

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_QMC6309)
    list(APPEND mag_srcs "src/qmc6309/qmc6309_backend.c")
endif()

idf_component_register(
    SRCS ${mag_srcs}
    INCLUDE_DIRS ${mag_include_dirs}
    REQUIRES cap_lua esp_board_manager esp_driver_i2c lua_driver_i2c
    PRIV_REQUIRES esp_timer
)

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350)
    idf_component_optional_requires(PRIVATE espressif__bmm350)
endif()

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150)
    idf_component_optional_requires(PRIVATE espressif__bmm150)
endif()

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
