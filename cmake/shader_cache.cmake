# Runs the Xenos shader recompiler over INPUT_DIR at build time into OUTPUT_CPP.
# Defines reblue_shader_cache_gen, and reblue_shader_hlsl_dump for inspecting
# the intermediate HLSL.
function(reblue_shader_cache)
    cmake_parse_arguments(ARG "" "RECOMP_TARGET;INPUT_DIR;INCLUDE_FILE;OUTPUT_CPP" "" ${ARGN})
    foreach(arg RECOMP_TARGET INPUT_DIR INCLUDE_FILE OUTPUT_CPP)
        if(NOT ARG_${arg})
            message(FATAL_ERROR "reblue_shader_cache: missing ${arg}")
        endif()
    endforeach()

    cmake_path(GET ARG_OUTPUT_CPP PARENT_PATH output_dir)
    file(MAKE_DIRECTORY "${output_dir}")

    file(GLOB_RECURSE shader_inputs CONFIGURE_DEPENDS
        "${ARG_INPUT_DIR}/*.vso" "${ARG_INPUT_DIR}/*.pso" "${ARG_INPUT_DIR}/*.xex")

    # REBLUE_XENOS_RECOMP names a host build of the recompiler for cross builds,
    # which cannot run the one they would compile themselves.
    if(REBLUE_XENOS_RECOMP)
        set(recomp_command "${REBLUE_XENOS_RECOMP}")
        set(recomp_depends "${REBLUE_XENOS_RECOMP}")
    else()
        set(recomp_command "$<TARGET_FILE:${ARG_RECOMP_TARGET}>")
        set(recomp_depends ${ARG_RECOMP_TARGET})
    endif()
    if(REBLUE_SHADERS_NO_INT64)
        set(recomp_command ${CMAKE_COMMAND} -E env XENOS_RECOMP_LOWER_INT64=1
            ${recomp_command})
    endif()

    add_custom_command(
        OUTPUT "${ARG_OUTPUT_CPP}"
        COMMAND ${recomp_command}
                "${ARG_INPUT_DIR}" "${ARG_OUTPUT_CPP}" "${ARG_INCLUDE_FILE}"
        DEPENDS ${recomp_depends} "${ARG_INCLUDE_FILE}" ${shader_inputs}
        COMMENT "Recompiling Xenos shaders from ${ARG_INPUT_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(reblue_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")

    set(REBLUE_HLSL_DUMP_DIR "${CMAKE_BINARY_DIR}/hlsl_dump" CACHE PATH
        "Directory reblue_shader_hlsl_dump writes recompiled HLSL into")
    add_custom_target(reblue_shader_hlsl_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REBLUE_HLSL_DUMP_DIR}"
        COMMAND ${recomp_command}
                "${ARG_INPUT_DIR}" "${output_dir}/shader_cache.hlsldump.cpp"
                "${ARG_INCLUDE_FILE}" "${REBLUE_HLSL_DUMP_DIR}"
        COMMENT "Dumping recompiled HLSL to ${REBLUE_HLSL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)
    if(NOT REBLUE_XENOS_RECOMP)
        add_dependencies(reblue_shader_hlsl_dump ${ARG_RECOMP_TARGET})
    endif()
endfunction()
