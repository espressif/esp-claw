# Board information output
message(STATUS "Selected Board: nm_display_28inch")
message(STATUS "Board Path: D:/code/pony/IoT/esp-claw/application/edge_agent/boards/rockbase-iot/nm_display_28inch")

idf_component_register(
    SRC_DIRS "." "../../boards/rockbase-iot/nm_display_28inch"
    INCLUDE_DIRS "." "../../boards/rockbase-iot/nm_display_28inch"
    REQUIRES esp_board_manager
)

# This is equivalent to adding WHOLE_ARCHIVE option to the idf_component_register call above:
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
