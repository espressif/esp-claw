idf_component_register(
    SRC_DIRS
        "."
    INCLUDE_DIRS
        "."
    EMBED_TXTFILES
        "web/index.html"
        "web/styles.css"
        "web/app.js"
        "web/lean-qr.min.mjs"
)

if(CONFIG_BASIC_DEMO_ENABLE_EMOTE)
    set(EMOTE_ASSETS_EXTERNAL_PATH "${CMAKE_SOURCE_DIR}/assets_local")
    set(EMOTE_ASSETS_FILE "${CMAKE_BINARY_DIR}/emote_assets.bin")
    build_speaker_assets_bin("emote" "284_240" ${EMOTE_ASSETS_FILE} ${CONFIG_MMAP_FILE_NAME_LENGTH} ${EMOTE_ASSETS_EXTERNAL_PATH})
    message(STATUS "Generated emote assets: ${EMOTE_ASSETS_FILE} -> emote partition")
    esptool_py_flash_to_partition(flash "emote" "${EMOTE_ASSETS_FILE}")
endif()
