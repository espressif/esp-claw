idf_component_register(
    SRCS
        "app_ota.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_ota_service
)
