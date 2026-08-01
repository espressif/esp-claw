idf_component_register(
    SRCS
        "src/display_service.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_board_manager
        esp_lcd
        esp_lcd_touch
        esp_lvgl_adapter
        lvgl
    PRIV_REQUIRES
        claw_hw_registry
)

# display_service uses app_claw_hw_lease_device from app_claw_hw_bridge.c but
# cannot REQUIRE app_claw because that would form a component-graph cycle
# (app_claw -> system_ui -> display_service). At link time app_claw's bridge
# is pulled in via app_claw_start(), so we only need its public header here.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../app_claw/include")
