if(IDF_TARGET STREQUAL "esp32c5" OR IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32c2" OR IDF_TARGET STREQUAL "esp32c3" OR IDF_TARGET STREQUAL "esp32h2")
    # No capacitive touch sensor on these targets; register a header-only
    # component so apps that include lua_module_touch.h still resolve while
    # APP_CLAW_LUA_MODULE_TOUCH must be disabled to skip the registration.
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(
    SRCS
        "src/lua_driver_touch.c"
    INCLUDE_DIRS
        "src"
    REQUIRES
        cap_lua
        esp_driver_touch_sens
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
