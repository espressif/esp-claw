set(EDGE_AGENT_PROJECT_LOG_PREFIX "[edge_agent]")
option(EDGE_AGENT_STRICT_IDF_PATCH "Fail configure when an ESP-IDF patch cannot be verified or applied" ON)

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(FATAL_ERROR "${EDGE_AGENT_PROJECT_LOG_PREFIX} IDF_PATH environment variable is not set")
endif()

function(edge_agent_patch_file_replace FILE_PATH OLD_TEXT NEW_TEXT PATCH_NAME)
    if(NOT EXISTS "${FILE_PATH}")
        message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: file not found: ${FILE_PATH}")
        return()
    endif()

    file(READ "${FILE_PATH}" FILE_CONTENT)
    set(FIXED_TEXTS "${NEW_TEXT}" ${ARGN})
    foreach(FIXED_TEXT IN LISTS FIXED_TEXTS)
        string(FIND "${FILE_CONTENT}" "${FIXED_TEXT}" FIXED_TEXT_OFFSET)
        if(NOT FIXED_TEXT_OFFSET EQUAL -1)
            message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' already applied")
            return()
        endif()
    endforeach()

    string(FIND "${FILE_CONTENT}" "${OLD_TEXT}" OLD_TEXT_OFFSET)
    if(OLD_TEXT_OFFSET EQUAL -1)
        if(EDGE_AGENT_STRICT_IDF_PATCH)
            message(FATAL_ERROR "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' could not be verified or applied: source pattern not found in ${FILE_PATH}")
        endif()
        message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: source pattern not found in ${FILE_PATH}")
        return()
    endif()

    string(REPLACE "${OLD_TEXT}" "${NEW_TEXT}" FILE_CONTENT "${FILE_CONTENT}")
    file(WRITE "${FILE_PATH}" "${FILE_CONTENT}")
    message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} Applied ESP-IDF patch '${PATCH_NAME}'")
endfunction()

# Upstream moved this repair from sample_edge NEG to shift_edge NEG later.
# Treat both as fixed states so v5.5.4, release/v5.5 snapshots, and the
# upstream fix branch can all configure without relying on a brittle patch file.
edge_agent_patch_file_replace(
    "$ENV{IDF_PATH}/components/esp_lcd/parl/esp_lcd_panel_io_parl.c"
    ".sample_edge = PARLIO_SAMPLE_EDGE_POS"
    ".sample_edge = PARLIO_SAMPLE_EDGE_NEG"
    "parlio_tx_edge"
    ".shift_edge = PARLIO_SHIFT_EDGE_NEG"
)

edge_agent_patch_file_replace(
    "$ENV{IDF_PATH}/components/usb/include/usb/usb_types_ch9.h"
    "#define USB_IAD_DESC_SIZE    9"
    "#define USB_IAD_DESC_SIZE    8"
    "usb_iad_desc_size"
)
