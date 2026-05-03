cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")
include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

# Board Manager bypasses idf.py and calls cmake directly, so SDKCONFIG_DEFAULTS
# (sdkconfig.defaults + board_manager.defaults) is never passed to cmake.
# Must set the target chip explicitly before project().
# set(ENV{IDF_TARGET} esp32s3)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(edge_agent)
include("${CMAKE_SOURCE_DIR}/tools/cmake/board_manager_patch.cmake")

include("${CMAKE_SOURCE_DIR}/tools/cmake/skills_sync.cmake")

fatfs_create_spiflash_image(storage fatfs_image FLASH_IN_PROJECT)
edge_agent_enable_component_skills_sync(
    TARGET fatfs_storage_bin
    DEMO_SKILLS_DIR "${CMAKE_SOURCE_DIR}/fatfs_image/skills"
    MANIFEST_PATH "${CMAKE_BINARY_DIR}/component_skills_manifest.json"
)
edge_agent_enable_component_builtin_lua_sync(
    TARGET fatfs_storage_bin
    OUTPUT_DIR "${CMAKE_SOURCE_DIR}/fatfs_image/scripts/builtin"
    MANIFEST_PATH "${CMAKE_BINARY_DIR}/component_builtin_lua_manifest.json"
)
