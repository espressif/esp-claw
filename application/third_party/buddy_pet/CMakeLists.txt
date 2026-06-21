cmake_minimum_required(VERSION 3.16)

set(CLAW_PROJECT_NAME "buddy_pet")
include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")
include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(buddy_pet)

set(BUDDY_PET_FATFS_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/fatfs_image")
set(BUDDY_PET_FATFS_BUILD_ROOT "${CMAKE_BINARY_DIR}/fatfs_image")
set(BUDDY_PET_FATFS_PREPARE_STAMP "${CMAKE_BINARY_DIR}/buddy_pet_prepare_fatfs_image.stamp")
set(BUDDY_PET_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")

file(GLOB_RECURSE BUDDY_PET_FATFS_SOURCE_FILES CONFIGURE_DEPENDS
    "${BUDDY_PET_FATFS_SOURCE_ROOT}/*"
)

set(BUDDY_PET_BOARD_FATFS_COPY_COMMANDS "")
set(BUDDY_PET_BOARD_FATFS_SOURCE_FILES "")

if(EXISTS "${BUDDY_PET_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${BUDDY_PET_BOARD_GENERATED_CMAKELISTS}" BUDDY_PET_BOARD_PATH_LINE
        REGEX "^message\\(STATUS \"Board Path: .+\"\\)$"
    )

    if(BUDDY_PET_BOARD_PATH_LINE)
        list(GET BUDDY_PET_BOARD_PATH_LINE 0 BUDDY_PET_BOARD_PATH_LINE)
        string(REGEX REPLACE
            "^message\\(STATUS \"Board Path: (.*)\"\\)$"
            "\\1"
            BUDDY_PET_BOARD_PATH
            "${BUDDY_PET_BOARD_PATH_LINE}"
        )

        set(BUDDY_PET_BOARD_FATFS_SOURCE_ROOT "${BUDDY_PET_BOARD_PATH}/fatfs_image")

        if(EXISTS "${BUDDY_PET_BOARD_FATFS_SOURCE_ROOT}")
            file(GLOB_RECURSE BUDDY_PET_BOARD_FATFS_SOURCE_FILES CONFIGURE_DEPENDS
                "${BUDDY_PET_BOARD_FATFS_SOURCE_ROOT}/*"
            )

            list(APPEND BUDDY_PET_BOARD_FATFS_COPY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${BUDDY_PET_BOARD_FATFS_SOURCE_ROOT}"
                        "${BUDDY_PET_FATFS_BUILD_ROOT}"
            )
        endif()
    endif()
endif()

add_custom_command(
    OUTPUT "${BUDDY_PET_FATFS_PREPARE_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${BUDDY_PET_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUDDY_PET_FATFS_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${BUDDY_PET_FATFS_SOURCE_ROOT}"
            "${BUDDY_PET_FATFS_BUILD_ROOT}"
    ${BUDDY_PET_BOARD_FATFS_COPY_COMMANDS}
    COMMAND ${CMAKE_COMMAND} -E touch "${BUDDY_PET_FATFS_PREPARE_STAMP}"
    DEPENDS ${BUDDY_PET_FATFS_SOURCE_FILES} ${BUDDY_PET_BOARD_FATFS_SOURCE_FILES}
    VERBATIM
)

add_custom_target(buddy_pet_prepare_fatfs_image
    DEPENDS "${BUDDY_PET_FATFS_PREPARE_STAMP}"
)

fatfs_create_spiflash_image(storage "${BUDDY_PET_FATFS_BUILD_ROOT}" FLASH_IN_PROJECT)

skill_builder_configure_skill_sync(
    TARGET fatfs_storage_bin
    SKILL_OUTPUT_DIR "${BUDDY_PET_FATFS_BUILD_ROOT}/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET fatfs_storage_bin
    BUILTIN_OUTPUT_DIR "${BUDDY_PET_FATFS_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR "${BUDDY_PET_FATFS_BUILD_ROOT}/scripts/docs"
)

add_dependencies(skill_builder_sync_skills buddy_pet_prepare_fatfs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts buddy_pet_prepare_fatfs_image)
add_dependencies(lua_module_builder_sync_docs buddy_pet_prepare_fatfs_image)
add_dependencies(fatfs_storage_bin buddy_pet_prepare_fatfs_image)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/skill_builder_sync.stamp"
    APPEND
    DEPENDS "${BUDDY_PET_FATFS_PREPARE_STAMP}"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_scripts_sync.stamp"
    APPEND
    DEPENDS "${BUDDY_PET_FATFS_PREPARE_STAMP}"
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/lua_module_builder_docs_sync.stamp"
    APPEND
    DEPENDS "${BUDDY_PET_FATFS_PREPARE_STAMP}"
)
