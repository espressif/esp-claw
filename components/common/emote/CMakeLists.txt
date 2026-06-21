idf_component_register(
    SRCS
        "emote.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        espressif__esp_mmap_assets
        espressif2022__esp_emote_gfx
        display_session
        freertos
        lvgl
)

if(CONFIG_APP_CLAW_ENABLE_EMOTE)
    set(EMOTE_ANIM_ASSETS_DIR "${CMAKE_BINARY_DIR}/mmap_asset_src/emote_anim")
    file(MAKE_DIRECTORY "${EMOTE_ANIM_ASSETS_DIR}")
    file(COPY
        "${CMAKE_CURRENT_LIST_DIR}/assets_local/emoji_large/offline.eaf"
        "${CMAKE_CURRENT_LIST_DIR}/assets_local/emoji_large/swim.eaf"
        DESTINATION "${EMOTE_ANIM_ASSETS_DIR}"
    )

    spiffs_create_partition_assets(
        emote
        "${EMOTE_ANIM_ASSETS_DIR}"
        FLASH_IN_PROJECT
        MMAP_FILE_SUPPORT_FORMAT ".eaf"
        IMPORT_INC_PATH "${CMAKE_BINARY_DIR}/mmap_include/emote"
    )
endif()
