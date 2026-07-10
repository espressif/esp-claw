set(mag_srcs
    "src/lua_module_magnetometer.c"
)

set(mag_include_dirs
    "src"
)

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350)
    list(APPEND mag_srcs
        "src/bmm350/bmm350.c"
        "src/bmm350/bmm350_backend.c"
    )
    list(APPEND mag_include_dirs
        "src/bmm350"
    )
endif()

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150)
    list(APPEND mag_srcs
        "src/bmm150/bmm150.c"
        "src/bmm150/bmm150_backend.c"
    )
    list(APPEND mag_include_dirs
        "src/bmm150"
    )
endif()

if(CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_QMC6309)
    list(APPEND mag_srcs
        "src/qmc6309/qmc6309_backend.c"
    )
    list(APPEND mag_include_dirs
        "src/qmc6309"
    )
endif()

idf_component_register(
    SRCS
        ${mag_srcs}
    INCLUDE_DIRS
        ${mag_include_dirs}
    REQUIRES
        cap_lua
        esp_board_manager
        nvs_flash
    PRIV_REQUIRES
        espressif__i2c_bus
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
