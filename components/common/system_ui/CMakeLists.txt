

idf_component_register(
    SRC_DIRS
        "src"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        claw_cap
        claw_core
        claw_skill
        display_service
        esp_board_manager
        esp_lcd
        esp_timer
        lvgl
    PRIV_REQUIRES
        esp_driver_gpio
)
