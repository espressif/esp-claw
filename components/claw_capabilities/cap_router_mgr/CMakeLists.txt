set(srcs "src/cap_router_mgr.c")
if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND srcs "src/cmd_cap_router_mgr.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        claw_event_router
        console
)
