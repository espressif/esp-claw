idf_component_register(
    SRCS
        "src/cap_voice.c"
        "src/cap_voice_audio.c"
        "src/cap_voice_pipeline.c"
        "src/cap_voice_whisper.c"
        "src/cap_voice_tts.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        claw_cap claw_core driver esp_timer esp_http_client
        esp_driver_i2s esp_driver_i2c
        cjson nvs_flash console mbedtls
    PRIV_REQUIRES
        claw_event_router esp_codec_dev esp-sr
)
