idf_component_register(
    SRCS "app_main.c" "wr_wifi.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        wave_rover_config
        wave_rover_hal
        wave_rover_mcp
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_log
        esp_check
)
