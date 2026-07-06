
set(app_claw_srcs
    "app_claw.c"
    "app_capabilities.c"
    "app_lua_modules.c"
)

set(app_claw_requires json)

if(CONFIG_APP_CLAW_CAP_CORE)
    list(APPEND app_claw_requires runtime)
endif()

idf_component_register(
    SRCS
        ${app_claw_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${app_claw_requires}
)
