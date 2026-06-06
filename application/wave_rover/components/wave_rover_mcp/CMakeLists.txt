idf_component_register(
    SRCS "wave_rover_mcp.c" "wave_rover_mcp_tools.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        wave_rover_hal
        wave_rover_config
        espressif__mcp-c-sdk
        esp_http_server
        log
        esp_common
        esp_timer
        heap
        freertos
        espressif__cjson
)
