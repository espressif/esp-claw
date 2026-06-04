idf_component_register(
    SRCS
        "main.c"
        "rover_demo_settings.c"
        "rover_demo_cli.c"
        "rover_demo_wifi.c"
        "rover_buttons.c"
        "rover_display.cpp"
        "app_mcp.c"
        "rover_mcp_tools.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        fatfs
        wear_levelling
        console
        driver
        esp_timer
        m5stack__m5unified
        cap_rover
        cap_unitv
        cap_mcp_server
        cap_session_mgr
        claw_cap
        claw_event_router
        espressif__mcp-c-sdk
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/boards/m5stickc_plus/setup_device.cpp"
)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_SOURCE_DIR}/boards/m5stickc_plus"
)
