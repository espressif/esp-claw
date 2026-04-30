set(srcs
    "src/cap_voice.c"
    "src/cap_voice_stt.c"
    "src/cap_voice_tts.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        driver
        esp_http_client
        json
        esp-tls
        claw_core
        claw_cap
        app_config
        esp_board_manager
)
