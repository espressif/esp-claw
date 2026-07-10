if(CONFIG_APP_CLAW_LUA_MODULE_BLE)
    if(NOT CONFIG_BT_ENABLED OR NOT CONFIG_BT_NIMBLE_ENABLED)
        message(FATAL_ERROR
            "APP_CLAW_LUA_MODULE_BLE requires ESP-IDF Bluetooth with NimBLE host. "
            "Enable CONFIG_BT_ENABLED and CONFIG_BT_NIMBLE_ENABLED."
        )
    endif()

    set(lua_module_ble_srcs
        "src/lua_module_ble.c"
        "src/lua_module_ble_events.c"
        "src/lua_module_ble_gap.c"
        "src/lua_module_ble_gatts.c"
        "src/lua_module_ble_smp.c"
        "src/lua_module_ble_stack.c"
        "src/lua_module_ble_utils.c"
    )
endif()

idf_component_register(
    SRCS
        ${lua_module_ble_srcs}
    INCLUDE_DIRS
        "src"
    REQUIRES
        bt
        cap_lua
        esp_timer
        nvs_flash
)
