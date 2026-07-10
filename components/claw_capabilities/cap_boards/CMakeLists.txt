idf_component_register()

set(CAP_BOARDS_SKILLS_DIR "${COMPONENT_DIR}/skills")
set(CAP_BOARDS_GENERATOR "${COMPONENT_DIR}/tools/generate_board_skill.py")
set(CAP_BOARDS_METADATA_YAML "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/gen_board_metadata.yaml")
set(CAP_BOARDS_OUTPUT_MD "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info/SKILL.md")

idf_build_get_property(python PYTHON)

add_custom_command(
    OUTPUT "${CAP_BOARDS_OUTPUT_MD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info"
    COMMAND ${python} "${CAP_BOARDS_GENERATOR}"
            --metadata-yaml "${CAP_BOARDS_METADATA_YAML}"
            --output-md "${CAP_BOARDS_OUTPUT_MD}"
    DEPENDS
        "${CAP_BOARDS_GENERATOR}"
        "${CAP_BOARDS_METADATA_YAML}"
    COMMENT "CLAW Generating cap_boards skill from gen_board_metadata.yaml"
    VERBATIM
)

add_custom_target(cap_boards_generate_skill DEPENDS "${CAP_BOARDS_OUTPUT_MD}")
add_dependencies(${COMPONENT_LIB} cap_boards_generate_skill)
skill_builder_add_target(cap_boards_generate_skill)
