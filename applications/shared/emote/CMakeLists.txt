idf_component_register(
    SRCS
        "emote.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        espressif__esp_board_manager
        espressif2022__esp_emote_expression
        espressif2022__esp_emote_gfx
        freertos
)

if(CONFIG_APP_CLAW_ENABLE_EMOTE)
    set(EMOTE_ASSETS_EXTERNAL_PATH "${CMAKE_CURRENT_LIST_DIR}/assets_local")
    set(EMOTE_ASSETS_FILE "${CMAKE_BINARY_DIR}/emote_assets.bin")
    build_speaker_assets_bin("emote" "284_240" ${EMOTE_ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH} ${EMOTE_ASSETS_EXTERNAL_PATH})
    message(STATUS "Generated emote assets: ${EMOTE_ASSETS_FILE} -> emote partition")
    esptool_py_flash_to_partition(flash "emote" "${EMOTE_ASSETS_FILE}")
endif()
