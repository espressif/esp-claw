idf_component_register(
    SRCS
        "src/lua_driver_touch.c"
    INCLUDE_DIRS
        "src"
    REQUIRES
        cap_lua
        esp_driver_touch_sens
)

# For touch hw version 3 chips, esp_driver_touch_sens propagates its include
# dirs automatically via REQUIRES.  For version 1/2 chips the new API is not
# supported; lua_driver_touch.c guards the implementation with
# #if SOC_TOUCH_SENSOR_VERSION == 3, so no extra include paths are needed.

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
