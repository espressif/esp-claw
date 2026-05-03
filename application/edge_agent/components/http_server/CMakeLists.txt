set(FRONTEND_SOURCE_DIR        "${CMAKE_CURRENT_LIST_DIR}/frontend_source")
set(FRONTEND_DIST_HTML_GZ      "${FRONTEND_SOURCE_DIR}/dist/index.html.gz")
set(FRONTEND_PUBLIC_FAVICON_ICO  "${FRONTEND_SOURCE_DIR}/public/favicon.ico")

if(NOT EXISTS "${FRONTEND_DIST_HTML_GZ}")
    message(FATAL_ERROR
        "\n"
        "Frontend bundle not found: ${FRONTEND_DIST_HTML_GZ}\n"
        "Build the SolidJS UI before running idf.py build, e.g.:\n"
        "    cd ${FRONTEND_SOURCE_DIR}\n"
        "    pnpm install --frozen-lockfile\n"
        "    pnpm run build\n")
endif()

idf_component_register(
    SRCS
        "http_server_core.c"
        "http_server_assets.c"
        "http_server_capabilities_api.c"
        "http_server_config_api.c"
        "http_server_lua_modules_api.c"
        "http_server_status_api.c"
        "http_server_files_api.c"
        "http_server_wechat_api.c"
        "http_server_webim_api.c"
        "http_server_json.c"
        "http_server_utils.c"
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        "${FRONTEND_DIST_HTML_GZ}"
        "${FRONTEND_PUBLIC_FAVICON_ICO}"
    REQUIRES
        app_config
        app_claw
        cap_im_local
        claw_cap
        esp_http_server
        heap
        json
)

# Rebuild the component whenever the bundled UI asset changes on disk.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FRONTEND_DIST_HTML_GZ}")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
