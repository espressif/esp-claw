set(imu_srcs
    "src/lua_module_imu.c"
)

set(imu_include_dirs
    "src"
)

if(CONFIG_LUA_MODULE_IMU_CHIP_BMI270)
    list(APPEND imu_srcs
        "src/bmi270/bmi270_backend.c"
    )
endif()

if(CONFIG_LUA_MODULE_IMU_CHIP_ICM42670)
    list(APPEND imu_srcs
        "src/icm42670/icm42670_backend.c"
    )
endif()

if(CONFIG_LUA_MODULE_IMU_CHIP_MPU6050)
    list(APPEND imu_srcs
        "src/mpu6050/mpu6050.c"
        "src/mpu6050/mpu6050_backend.c"
    )
    list(APPEND imu_include_dirs
        "src/mpu6050"
    )
endif()

idf_component_register(
    SRCS
        ${imu_srcs}
    INCLUDE_DIRS
        ${imu_include_dirs}
    REQUIRES
        cap_lua
        esp_board_manager
    PRIV_REQUIRES
        espressif__i2c_bus
)

if(CONFIG_LUA_MODULE_IMU_CHIP_BMI270)
    idf_component_optional_requires(PRIVATE espressif__bmi270_sensor)
endif()

if(CONFIG_LUA_MODULE_IMU_CHIP_ICM42670)
    idf_component_optional_requires(PRIVATE espressif__icm42670)
endif()

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
