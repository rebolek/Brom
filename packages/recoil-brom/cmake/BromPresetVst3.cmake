# Brom VST3 preset support, packaged for any plug-in's CMake graph.
#
#   include(<pokuston>/packages/recoil-brom/cmake/BromPresetVst3.cmake)
#   brom_add_preset_vst3_support(TARGET brom_preset_support
#       POKUSTON_ROOT <pokuston checkout> VST3_SDK_TARGET sdk)
#   brom_attach_preset_vst3_support(my_plugin_vst3 brom_preset_support)
#
# A plug-in links exactly one support target. The VST3 target that an AUv2 component
# embeds links one created with AU_WRAPPER (the AU program rule); every other VST3
# target uses the default. BromProgramMap.cmake checks both at configure time.
#
# The support target is one static library: every Brom source is compiled into it
# exactly once, however many plug-ins attach to it. A consumer supplies its own
# schema, preparation, parameter conversion and DSP callbacks; nothing here knows an
# instrument. No SDK source is changed.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/BromProgramMap.cmake")

# Creates the PIC static support target. Calling it again with the same TARGET and the
# same AU_WRAPPER setting is a no-op, so a graph with several consumers cannot compile
# the sources twice; asking for a different flavor under an existing name is an error.
function(brom_add_preset_vst3_support)
    cmake_parse_arguments(BROM "AU_WRAPPER" "TARGET;POKUSTON_ROOT;VST3_SDK_TARGET" "" ${ARGN})
    if(BROM_AU_WRAPPER)
        set(program_flavor au)
        set(program_map_target brom_program_map_au)
    else()
        set(program_flavor vst3)
        set(program_map_target brom_program_map)
    endif()
    foreach(required TARGET POKUSTON_ROOT VST3_SDK_TARGET)
        if(NOT BROM_${required})
            message(FATAL_ERROR "brom_add_preset_vst3_support: ${required} is required")
        endif()
    endforeach()
    if(TARGET "${BROM_TARGET}")
        get_target_property(existing_flavor "${BROM_TARGET}" BROM_SUPPORT_FLAVOR)
        if(NOT existing_flavor STREQUAL program_flavor)
            message(FATAL_ERROR
                "brom_add_preset_vst3_support: '${BROM_TARGET}' already exists with the "
                "'${existing_flavor}' program flavor; this call asks for '${program_flavor}'")
        endif()
        return()
    endif()
    if(NOT TARGET "${BROM_VST3_SDK_TARGET}")
        message(FATAL_ERROR
            "brom_add_preset_vst3_support: VST3_SDK_TARGET '${BROM_VST3_SDK_TARGET}' is not a target; "
            "call it after smtg_enable_vst3_sdk()")
    endif()
    set(plugin "${BROM_POKUSTON_ROOT}/packages/recoil-brom/brom/plugin")
    set(c_sources
        "${plugin}/preset/transaction.c"
        "${plugin}/preset/catalog.c"
        "${plugin}/preset/catalog_service.c"
        "${plugin}/preset/save.c"
        "${plugin}/preset/session.c"
    )
    set(cxx_sources
        "${plugin}/vst3/preset_protocol.cpp"
        "${plugin}/vst3/preset_bridge.cpp"
        "${plugin}/vst3/meter_protocol.cpp"
    )
    foreach(source IN LISTS c_sources cxx_sources)
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "brom_add_preset_vst3_support: missing ${source}")
        endif()
    endforeach()
    set_source_files_properties(${c_sources} PROPERTIES LANGUAGE C)

    add_library("${BROM_TARGET}" STATIC ${c_sources} ${cxx_sources})
    target_compile_features("${BROM_TARGET}" PUBLIC cxx_std_17)
    target_compile_features("${BROM_TARGET}" PRIVATE c_std_11)
    # The objects end up inside plug-in shared libraries: position independent, and
    # hidden, so a plug-in exports only what the SDK's own export list names.
    set_target_properties("${BROM_TARGET}" PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    # Headers are included as "packages/recoil-brom/brom/plugin/...", rooted at the
    # checkout. The architecture and optimisation come from the consumer's graph.
    target_include_directories("${BROM_TARGET}" PUBLIC "${BROM_POKUSTON_ROOT}")
    find_package(Threads REQUIRED)
    target_link_libraries("${BROM_TARGET}" PUBLIC "${BROM_VST3_SDK_TARGET}" Threads::Threads
        "${program_map_target}")
    set_property(TARGET "${BROM_TARGET}" PROPERTY BROM_SUPPORT_FLAVOR "${program_flavor}")
    if(NOT WIN32)
        target_link_libraries("${BROM_TARGET}" PUBLIC m)
    endif()
endfunction()

# Links a plug-in (or any consumer) to the support target. PRIVATE, so the support
# library never leaks into a consumer's own interface.
function(brom_attach_preset_vst3_support consumer support)
    if(NOT TARGET "${consumer}")
        message(FATAL_ERROR "brom_attach_preset_vst3_support: '${consumer}' is not a target")
    endif()
    if(NOT TARGET "${support}")
        message(FATAL_ERROR "brom_attach_preset_vst3_support: '${support}' is not a target")
    endif()
    get_target_property(support_flavor "${support}" BROM_SUPPORT_FLAVOR)
    if(NOT support_flavor)
        message(FATAL_ERROR "brom_attach_preset_vst3_support: '${support}' was not created by brom_add_preset_vst3_support")
    endif()
    brom_record_program_flavor("${consumer}" "${support_flavor}")
    target_link_libraries("${consumer}" PRIVATE "${support}")
endfunction()
