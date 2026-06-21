idf_component_register(
    SRCS "app_main.c" "wr_wifi.c" "wr_syslog.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        app_update
        wave_rover_config
        wave_rover_hal
        wave_rover_mcp
        wave_rover_power_mgr
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        lwip
        mdns
        log
        esp_common
        freertos
)
