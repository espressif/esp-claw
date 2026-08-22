idf_component_register(
    SRCS
        "${CMAKE_SOURCE_DIR}/boards/m5sticks3/setup_device.cpp"
    INCLUDE_DIRS
        "${CMAKE_SOURCE_DIR}/boards/m5sticks3"
    REQUIRES
        driver esp_system esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd freertos
        m5stack__m5gfx
)
