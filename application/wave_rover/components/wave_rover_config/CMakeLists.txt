idf_component_register(
    SRCS "wave_rover_config.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash log esp_common
)
