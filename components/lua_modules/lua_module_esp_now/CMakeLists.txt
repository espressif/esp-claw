if(CONFIG_APP_CLAW_LUA_MODULE_ESP_NOW)
    set(lua_module_esp_now_srcs
        "src/lua_module_esp_now.c"
        "src/lua_module_esp_now_events.c"
    )
endif()

idf_component_register(
    SRCS
        ${lua_module_esp_now_srcs}
    INCLUDE_DIRS
        "src"
    REQUIRES
        cap_lua
        esp_wifi
        freertos
        heap
)
