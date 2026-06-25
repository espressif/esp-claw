idf_component_register(
    SRCS
        "src/claw_agent_mgr.c"
        "src/claw_session_mgr.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        claw_core
        freertos
)

set_source_files_properties(
    "src/claw_session_mgr.c"
    PROPERTIES COMPILE_OPTIONS "-Wframe-larger-than=512"
)
