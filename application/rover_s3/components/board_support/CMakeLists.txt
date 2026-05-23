idf_component_register(
    SRCS
        "${CMAKE_SOURCE_DIR}/boards/m5sticks3/setup_device.cpp"
    INCLUDE_DIRS
        "${CMAKE_SOURCE_DIR}/boards/m5sticks3"
    REQUIRES
        driver esp_system esp_driver_gpio esp_driver_i2c freertos
)
