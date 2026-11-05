# Generate ownership from the same board selected by board manager.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(board_resources "${CMAKE_CURRENT_BINARY_DIR}/generated_board_resources.c")
    set(board_generator "${CMAKE_CURRENT_LIST_DIR}/tools/generate_board_resources.py")
    set(board_selection "${PROJECT_DIR}/components/gen_bmgr_codes/CMakeLists.txt")
    set(board_args --empty)
    set(board_inputs "${board_generator}")
    if(EXISTS "${board_selection}")
        file(STRINGS "${board_selection}" board_path_line REGEX "^message\\(STATUS \"Board Path: .+\"\\)$")
        if(NOT board_path_line)
            message(FATAL_ERROR "Board manager did not record its selected board path")
        endif()
        list(GET board_path_line 0 board_path_line)
        string(REGEX REPLACE "^message\\(STATUS \"Board Path: (.*)\"\\)$" "\\1" board_dir "${board_path_line}")
        set(board_args --board-dir "${board_dir}")
        list(APPEND board_inputs "${board_selection}" "${board_dir}/board_peripherals.yaml" "${board_dir}/board_devices.yaml")
    elseif(CONFIG_ESP_BOARD_NAME)
        message(FATAL_ERROR "Generate board manager configuration before building board resources")
    endif()
    idf_component_get_property(board_manager_dir espressif__esp_board_manager COMPONENT_DIR)
    list(APPEND board_inputs "${board_manager_dir}/src/esp_board_periph.c" "${board_manager_dir}/src/esp_board_device.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${board_inputs})
    execute_process(COMMAND "${PYTHON}" "${board_generator}" ${board_args} --output "${board_resources}" --manager-dir "${board_manager_dir}"
        RESULT_VARIABLE board_result ERROR_VARIABLE board_error)
    if(NOT board_result EQUAL 0)
        message(FATAL_ERROR "Board resource generation failed: ${board_error}")
    endif()
    if(board_error)
        string(STRIP "${board_error}" board_error)
        message(STATUS "${board_error}")
    endif()
endif()

set(app_claw_srcs
    "app_claw.c"
    "app_claw_hw_bridge.c"
    ${board_resources}
    "app_capabilities.c"
    "app_lua_modules.c"
)

if(CONFIG_APP_CLAW_ENABLE_CLI)
    list(APPEND app_claw_srcs "app_claw_cli.c")
endif()

idf_component_register(
    SRCS
        ${app_claw_srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        claw_hw_registry
    PRIV_REQUIRES
        console
)

# Use build-local Board Manager copies with hooks around its driver lifecycle.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(board_manager_lib espressif__esp_board_manager COMPONENT_LIB)
    get_target_property(board_manager_sources ${board_manager_lib} SOURCES)
    foreach(kind periph device)
        list(FILTER board_manager_sources EXCLUDE REGEX "(^|/)esp_board_${kind}\\.c$")
        list(APPEND board_manager_sources "${CMAKE_CURRENT_BINARY_DIR}/esp_board_${kind}.c")
    endforeach()
    set_property(TARGET ${board_manager_lib} PROPERTY SOURCES "${board_manager_sources}")
endif()
