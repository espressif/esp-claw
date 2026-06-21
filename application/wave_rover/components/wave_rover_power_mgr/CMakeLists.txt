idf_component_register(
    SRCS "wave_rover_power_mgr.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        wave_rover_hal
        wave_rover_mcp
        esp_timer
        esp_wifi
        esp_pm
        log
        esp_common
        freertos
)
