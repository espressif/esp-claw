cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")
include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.3.0")
project(edge_agent)

set(EDGE_AGENT_FATFS_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_BUILD_ROOT "${CMAKE_BINARY_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_PREPARE_STAMP "${CMAKE_BINARY_DIR}/edge_agent_prepare_fatfs_image.stamp")
set(EDGE_AGENT_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")

file(GLOB_RECURSE EDGE_AGENT_FATFS_SOURCE_FILES CONFIGURE_DEPENDS
    "${EDGE_AGENT_FATFS_SOURCE_ROOT}/*"
)

set(EDGE_AGENT_BOARD_FATFS_COPY_COMMANDS "")
set(EDGE_AGENT_BOARD_FATFS_SOURCE_FILES "")

if(EXISTS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}" EDGE_AGENT_BOARD_PATH_LINE
        REGEX "^message\\(STATUS \"Board Path: .+\"\\)$"
    )

    if(EDGE_AGENT_BOARD_PATH_LINE)
        list(GET EDGE_AGENT_BOARD_PATH_LINE 0 EDGE_AGENT_BOARD_PATH_LINE)
        string(REGEX REPLACE
            "^message\\(STATUS \"Board Path: (.*)\"\\)$"
            "\\1"
            EDGE_AGENT_BOARD_PATH
            "${EDGE_AGENT_BOARD_PATH_LINE}"
        )

        set(EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT "${EDGE_AGENT_BOARD_PATH}/fatfs_image")

        if(EXISTS "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}")
            file(GLOB_RECURSE EDGE_AGENT_BOARD_FATFS_SOURCE_FILES CONFIGURE_DEPENDS
                "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}/*"
            )

            list(APPEND EDGE_AGENT_BOARD_FATFS_COPY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}"
                        "${EDGE_AGENT_FATFS_BUILD_ROOT}"
            )
        endif()
    endif()
endif()

add_custom_command(
    OUTPUT "${EDGE_AGENT_FATFS_PREPARE_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    ${EDGE_AGENT_BOARD_FATFS_COPY_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E touch "${EDGE_AGENT_FATFS_PREPARE_STAMP}"
    DEPENDS ${EDGE_AGENT_FATFS_SOURCE_FILES} ${EDGE_AGENT_BOARD_FATFS_SOURCE_FILES}
    VERBATIM
)

add_custom_target(edge_agent_prepare_fatfs_image
    DEPENDS "${EDGE_AGENT_FATFS_PREPARE_STAMP}"
)

fatfs_create_spiflash_image(storage "${EDGE_AGENT_FATFS_BUILD_ROOT}" FLASH_IN_PROJECT)

skill_builder_configure_skill_sync(
    TARGET fatfs_storage_bin
    SKILL_OUTPUT_DIR "${EDGE_AGENT_FATFS_BUILD_ROOT}/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET fatfs_storage_bin
    BUILTIN_OUTPUT_DIR "${EDGE_AGENT_FATFS_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR "${EDGE_AGENT_FATFS_BUILD_ROOT}/scripts/docs"
)

add_dependencies(skill_builder_sync_skills edge_agent_prepare_fatfs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts edge_agent_prepare_fatfs_image)
add_dependencies(lua_module_builder_sync_docs edge_agent_prepare_fatfs_image)
add_dependencies(fatfs_storage_bin edge_agent_prepare_fatfs_image)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/skill_builder_sync.stamp"
    APPEND
    DEPENDS "${EDGE_AGENT_FATFS_PREPARE_STAMP}"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_scripts_sync.stamp"
    APPEND
    DEPENDS "${EDGE_AGENT_FATFS_PREPARE_STAMP}"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/lua_module_builder_docs_sync.stamp"
    APPEND
    DEPENDS "${EDGE_AGENT_FATFS_PREPARE_STAMP}"
)
