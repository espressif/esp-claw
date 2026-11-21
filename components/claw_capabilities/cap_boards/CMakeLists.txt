idf_component_register()

set(CAP_BOARDS_SKILLS_DIR "${COMPONENT_DIR}/skills")
set(CAP_BOARDS_GENERATOR "${COMPONENT_DIR}/tools/generate_board_skill.py")
set(CAP_BOARDS_METADATA_YAML "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/gen_board_metadata.yaml")
set(CAP_BOARDS_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")
set(CAP_BOARDS_OUTPUT_MD "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info/SKILL.md")
set(CAP_BOARDS_OUTPUT_REFERENCE_MD "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info/references/board.md")
set(CAP_BOARDS_GENERATOR_ARGS
    --metadata-yaml "${CAP_BOARDS_METADATA_YAML}"
    --output-md "${CAP_BOARDS_OUTPUT_MD}"
    --output-reference-md "${CAP_BOARDS_OUTPUT_REFERENCE_MD}"
)
set(CAP_BOARDS_GENERATOR_DEPENDS
    "${CAP_BOARDS_GENERATOR}"
    "${CAP_BOARDS_METADATA_YAML}"
    "${CAP_BOARDS_GENERATED_CMAKELISTS}"
)

if(EXISTS "${CAP_BOARDS_GENERATED_CMAKELISTS}")
    file(STRINGS "${CAP_BOARDS_GENERATED_CMAKELISTS}" CAP_BOARDS_BOARD_PATH_LINE REGEX "^message\\(STATUS \"Board Path: .+\"\\)$")
    if(CAP_BOARDS_BOARD_PATH_LINE)
        list(GET CAP_BOARDS_BOARD_PATH_LINE 0 CAP_BOARDS_BOARD_PATH_LINE)
        string(REGEX REPLACE "^message\\(STATUS \"Board Path: (.*)\"\\)$" "\\1" CAP_BOARDS_BOARD_PATH "${CAP_BOARDS_BOARD_PATH_LINE}")
        set(CAP_BOARDS_BOARD_README "${CAP_BOARDS_BOARD_PATH}/README.md")
        if(EXISTS "${CAP_BOARDS_BOARD_README}")
            list(APPEND CAP_BOARDS_GENERATOR_ARGS --board-readme "${CAP_BOARDS_BOARD_README}")
            list(APPEND CAP_BOARDS_GENERATOR_DEPENDS "${CAP_BOARDS_BOARD_README}")
        endif()
    endif()
endif()

idf_build_get_property(python PYTHON)

add_custom_command(
    OUTPUT "${CAP_BOARDS_OUTPUT_MD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CAP_BOARDS_SKILLS_DIR}/board_hardware_info"
    COMMAND ${python} "${CAP_BOARDS_GENERATOR}" ${CAP_BOARDS_GENERATOR_ARGS}
    DEPENDS ${CAP_BOARDS_GENERATOR_DEPENDS}
    COMMENT "CLAW Generating cap_boards skill from gen_board_metadata.yaml"
    VERBATIM
)

add_custom_target(cap_boards_generate_skill DEPENDS "${CAP_BOARDS_OUTPUT_MD}")
add_dependencies(${COMPONENT_LIB} cap_boards_generate_skill)
skill_builder_add_target(cap_boards_generate_skill)
