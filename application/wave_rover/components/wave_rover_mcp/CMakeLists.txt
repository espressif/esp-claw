idf_component_register(
    SRCS "wave_rover_mcp.c" "wave_rover_mcp_tools.c" "wave_rover_mcp_web.c" "wave_rover_mcp_state.c" "wave_rover_mcp_metrics.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        wave_rover_hal
        wave_rover_config
        wave_rover_power_mgr
        espressif__mcp-c-sdk
        esp_http_server
        esp_wifi
        esp_netif
        log
        esp_common
        esp_timer
        heap
        freertos
        app_update
        espressif__cjson
)
