idf_component_register(
    SRCS
        "main.c"
        "rover_s3_settings.c"
        "rover_s3_cli.c"
        "rover_s3_wifi.c"
        "rover_s3_display.c"
        "rover_s3_httpd.c"
        "app_rover_s3.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event freertos
        fatfs wear_levelling console driver esp_timer
        esp_http_server cjson
        captive_dns
        board_support
        cap_rover cap_unitv cap_voice
        cap_im_platform cap_files cap_skill_mgr
        cap_session_mgr cap_system cap_time
        claw_core claw_cap claw_event_router claw_memory claw_skill
)
