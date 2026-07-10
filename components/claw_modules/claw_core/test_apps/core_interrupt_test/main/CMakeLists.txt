idf_component_register(SRCS "test_app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES claw_core esp_event esp_netif lwip unity
                       WHOLE_ARCHIVE)
