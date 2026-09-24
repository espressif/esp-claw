set(srcs "src/cap_skill_mgr.c")
if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND srcs "src/cmd_cap_skill.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_cap
        claw_skill
        console
)
