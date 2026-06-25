idf_component_register(
    SRCS
        "src/claw_event.c"
        "src/claw_event_router.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        claw_core
        claw_manager
        freertos
        console
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wframe-larger-than=512)
