idf_component_register(
    SRCS
        "settings_store.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash
)
