set(audio_hub_srcs
    "src/audio_hub.c"
    "src/audio_memory.c"
)

if(CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT)
    list(APPEND audio_hub_srcs
        "src/audio_mixer.c"
        "src/audio_capture.c"
    )
endif()

idf_component_register(
    SRCS ${audio_hub_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
)
