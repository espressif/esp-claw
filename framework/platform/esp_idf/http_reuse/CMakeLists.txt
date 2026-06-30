idf_component_register(
    SRCS
        "esp_http_client_reuse.c"
    INCLUDE_DIRS
    REQUIRES
        esp_http_client
        freertos
        http_parser
)

# Only redirect esp_http_client_init/cleanup/perform through the
# pool wrappers when the reuse feature is enabled in Kconfig.
# When disabled, callers go straight to the IDF implementation
# and esp_http_client_reuse.c does not define any __wrap_* symbols.
if(CONFIG_HTTP_REUSE_ENABLE)
    idf_build_set_property(LINK_OPTIONS
        "-Wl,--wrap=esp_http_client_init;-Wl,--wrap=esp_http_client_cleanup;-Wl,--wrap=esp_http_client_perform"
        APPEND)
endif()
