file(GLOB_RECURSE FONT_SRCS font/*.c)

set(painter_reqs esp_mm)
if(CONFIG_SOC_PPA_SUPPORTED)
    list(APPEND painter_reqs esp_driver_ppa)
endif()

idf_component_register(
    SRCS
        ${FONT_SRCS}
        "esp_painter.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        ${painter_reqs}
)
