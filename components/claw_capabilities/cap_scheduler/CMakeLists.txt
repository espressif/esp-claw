set(srcs
    "src/cap_scheduler.c"
    "src/cap_scheduler_parser.c"
    "src/cap_scheduler_store.c"
)
if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND srcs "src/cmd_cap_scheduler.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        claw_event_router
        console
        nvs_flash
)
