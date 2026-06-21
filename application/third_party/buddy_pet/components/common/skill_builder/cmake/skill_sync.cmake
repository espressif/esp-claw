include_guard(GLOBAL)

function(skill_builder_require_args function_name prefix)
    foreach(required_arg IN LISTS ARGN)
        if(NOT ${prefix}_${required_arg})
            message(FATAL_ERROR "${function_name} requires ${required_arg}")
        endif()
    endforeach()
endfunction()

function(skill_builder_add_target target_name)
    if(NOT target_name)
        message(FATAL_ERROR "skill_builder_add_target requires a target name")
    endif()
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "skill_builder_add_target requires an existing target: ${target_name}")
    endif()
    set_property(GLOBAL APPEND PROPERTY CLAW_SKILL_BUILD_DEPENDENCY_TARGETS "${target_name}")
    if(TARGET skill_builder_sync_skills)
        add_dependencies(skill_builder_sync_skills "${target_name}")
    endif()
endfunction()

function(skill_builder_get_dependency_targets out_var)
    get_property(property_set GLOBAL PROPERTY CLAW_SKILL_BUILD_DEPENDENCY_TARGETS SET)
    if(NOT property_set)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    get_property(dependency_targets GLOBAL PROPERTY CLAW_SKILL_BUILD_DEPENDENCY_TARGETS)
    list(REMOVE_DUPLICATES dependency_targets)
    set(${out_var} "${dependency_targets}" PARENT_SCOPE)
endfunction()

function(skill_builder_configure_skill_sync)
    set(one_value_args TARGET SKILL_OUTPUT_DIR)
    cmake_parse_arguments(arg "" "${one_value_args}" "" ${ARGN})

    skill_builder_require_args(skill_builder_configure_skill_sync arg TARGET SKILL_OUTPUT_DIR)

    idf_build_get_property(python PYTHON)
    set(sync_stamp "${CMAKE_BINARY_DIR}/skill_builder_sync.stamp")
    set(sync_depfile "${CMAKE_BINARY_DIR}/skill_builder_sync.d")
    set(script_args
        --build-dir "${CMAKE_BINARY_DIR}"
        --skill-output-dir "${arg_SKILL_OUTPUT_DIR}"
        --stamp-path "${sync_stamp}"
        --depfile "${sync_depfile}"
    )

    add_custom_command(
        OUTPUT "${sync_stamp}"
        COMMAND ${python} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/sync_component_skills.py" ${script_args}
        DEPENDS
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/sync_component_skills.py"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/skill_sync_common.py"
                "${CMAKE_BINARY_DIR}/project_description.json"
        DEPFILE "${sync_depfile}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )

    add_custom_target(skill_builder_sync_skills ALL DEPENDS "${sync_stamp}")

    skill_builder_get_dependency_targets(dependency_targets)
    if(dependency_targets)
        add_dependencies(skill_builder_sync_skills ${dependency_targets})
    endif()

    add_dependencies(${arg_TARGET} skill_builder_sync_skills)
endfunction()
