idf_component_register(
    SRCS
        "src/lua_module_pet.c"
    INCLUDE_DIRS "src"
    REQUIRES
        pet_buddy
        cap_lua
        claw_event_router
        json
)
