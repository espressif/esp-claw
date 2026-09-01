set(lua_module_vision_srcs)

set(lua_module_vision_requires)

if(CONFIG_APP_CLAW_LUA_MODULE_VISION AND CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    set(lua_module_vision_srcs
        "src/lua_module_vision.c"
    )

    set(lua_module_vision_requires
        cap_lua
        lua_module_image
    )

    if(CONFIG_LUA_MODULE_VISION_COLOR_DETECT OR CONFIG_LUA_MODULE_VISION_QRCODE_DETECT)
        list(APPEND lua_module_vision_srcs
            "src/vision_core_runtime.c"
        )
        list(APPEND lua_module_vision_requires
            esp_vision_core
        )
    endif()

    if(CONFIG_LUA_MODULE_VISION_MOTION_DETECT)
        list(APPEND lua_module_vision_srcs
            "src/motion_detect.cpp"
        )
    endif()

    if(CONFIG_LUA_MODULE_VISION_COLOR_DETECT)
        list(APPEND lua_module_vision_srcs
            "src/color_detect.c"
        )
    endif()

    if(CONFIG_LUA_MODULE_VISION_QRCODE_DETECT)
        list(APPEND lua_module_vision_srcs
            "src/qrcode_detect.c"
        )
    endif()

    if(CONFIG_LUA_MODULE_VISION_ESPDET)
        list(APPEND lua_module_vision_srcs
            "src/espdet.cpp"
        )
        list(APPEND lua_module_vision_requires
            esp-dl
            freertos
        )
    endif()
endif()

idf_component_register(
    SRCS
        ${lua_module_vision_srcs}
    INCLUDE_DIRS
        "src"
    REQUIRES
        ${lua_module_vision_requires}
)

if(CONFIG_LUA_MODULE_VISION_COLOR_DETECT OR CONFIG_LUA_MODULE_VISION_QRCODE_DETECT)
    idf_component_get_property(lua_module_vision_core_lib tda-2030__esp_vision_core COMPONENT_LIB)
    if(lua_module_vision_core_lib)
        # Keep optional vision algorithms within the application partition.
        target_compile_options(${lua_module_vision_core_lib} PRIVATE -Os)
    endif()
endif()

if(CONFIG_LUA_MODULE_VISION_ESPDET)
    idf_component_get_property(lua_module_vision_esp_dl_lib espressif__esp-dl COMPONENT_LIB)
    if(lua_module_vision_esp_dl_lib)
        target_compile_options(${lua_module_vision_esp_dl_lib} PRIVATE -Wno-non-c-typedef-for-linkage)
        target_compile_options(${lua_module_vision_esp_dl_lib} PRIVATE -Os)
    endif()
endif()
