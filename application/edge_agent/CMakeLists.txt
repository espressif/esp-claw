cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")
include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(edge_agent VERSION 0.1.0)

set(EDGE_AGENT_FATFS_IMAGE_ROOT "${CMAKE_SOURCE_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/storage")
set(EDGE_AGENT_FATFS_BUILD_ROOT "${CMAKE_BINARY_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/system")
set(EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT "${CMAKE_BINARY_DIR}/system_fs_image")
set(EDGE_AGENT_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")
set(EDGE_AGENT_FATFS_STORAGE_COPY_COMMANDS "")

set(EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS "")

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
            list(APPEND EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}"
                        "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
            )
        endif()
    endif()
endif()

add_custom_target(edge_agent_prepare_fatfs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_BUILD_ROOT}"
    ${EDGE_AGENT_FATFS_STORAGE_COPY_COMMANDS}
    VERBATIM
)

# Prepare the read-only system partition build tree: mirror the source
# system_fs_image/ into a build-side staging dir that skill/lua syncs
# can also write into before fatfsgen packs it.
add_custom_target(edge_agent_prepare_system_fs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    ${EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS}
    VERBATIM
)

fatfs_create_rawflash_image(system "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}" FLASH_IN_PROJECT)
fatfs_create_spiflash_image(storage "${EDGE_AGENT_FATFS_BUILD_ROOT}" FLASH_IN_PROJECT)

# Skill / Lua resources are now baked into the read-only system partition
# so storage can be reformatted at runtime without losing them.
skill_builder_configure_skill_sync(
    TARGET fatfs_system_bin
    SKILL_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET fatfs_system_bin
    BUILTIN_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/docs"
)

add_dependencies(skill_builder_sync_skills edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_docs edge_agent_prepare_system_fs_image)
add_dependencies(fatfs_system_bin edge_agent_prepare_system_fs_image)
add_dependencies(fatfs_storage_bin edge_agent_prepare_fatfs_image)
