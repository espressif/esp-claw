idf_component_register(
    SRCS "src/cap_app_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES app_registry claw_cap espressif__cjson
)
