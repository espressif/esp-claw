idf_component_register(SRCS "test_app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES claw_manager cap_agent_mgr claw_core claw_cap esp_timer unity fatfs wear_levelling
                       WHOLE_ARCHIVE)
