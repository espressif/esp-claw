# Register the Unity test component and link the Rust `claw-sys-selftest`
# staticlib, mirroring the cargo->staticlib->imported-library wiring in
# framework/runtime/CMakeLists.txt.
#
# The tiny `claw_rs_log.c` shim (the per-level ESP_LOGx bridge the Rust log sink
# calls) is compiled here. WHOLE_ARCHIVE keeps the Unity `TEST_CASE`s and the
# shim regardless of static-archive scan order.
idf_component_register(
    SRCS
        "test_app_main.c"
        "test_claw_sys.c"
        "test_wifi.c"
        "../../../csrc/claw_rs_log.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        unity
        esp_http_client
        esp-tls
        mbedtls
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
    WHOLE_ARCHIVE
)

# The Rust workspace root is four levels up: main -> esp_idf -> tests -> claw-sys -> rust.
set(claw_sys_rust_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(claw_sys_rust_target "xtensa-esp32s3-espidf")
set(claw_sys_rust_profile "release")
set(claw_sys_rust_lib
    "${claw_sys_rust_dir}/target/${claw_sys_rust_target}/${claw_sys_rust_profile}/libclaw_sys_selftest.a")

# Always invoke cargo; it does its own incremental rebuild and only rewrites the
# archive when the Rust sources change. `std` is not shipped precompiled for
# *-espidf, so it is built from source (the pinned `esp` toolchain accepts -Z).
add_custom_target(claw_sys_selftest_build
    BYPRODUCTS "${claw_sys_rust_lib}"
    COMMAND cargo build --${claw_sys_rust_profile} -p claw-sys-selftest
            --target ${claw_sys_rust_target} -Z build-std=std,panic_abort
    WORKING_DIRECTORY "${claw_sys_rust_dir}"
    COMMENT "Building claw-sys-selftest Rust staticlib (${claw_sys_rust_target})"
    VERBATIM
    USES_TERMINAL
)

add_library(claw_sys_selftest_rust STATIC IMPORTED GLOBAL)
set_target_properties(claw_sys_selftest_rust PROPERTIES
    IMPORTED_LOCATION "${claw_sys_rust_lib}"
    # The Rust archive references esp_http_client_* etc (from claw-sys's FFI
    # bindings). CMake places imported libraries after the IDF component link
    # groups, so by default those symbols are unresolved. Setting
    # INTERFACE_LINK_LIBRARIES makes CMake emit the IDF libs directly after the
    # Rust archive, giving the linker a second chance to resolve them.
    INTERFACE_LINK_LIBRARIES "idf::esp_http_client;idf::esp-tls;idf::mbedtls;idf::newlib;idf::freertos;idf::pthread"
)
add_dependencies(claw_sys_selftest_rust claw_sys_selftest_build)

target_link_libraries(${COMPONENT_LIB} PUBLIC claw_sys_selftest_rust)
add_dependencies(${COMPONENT_LIB} claw_sys_selftest_build)
