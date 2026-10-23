# The program-rule flavor of a Brom binary (see brom/plugin/preset/program_map.h).
#
#   brom_program_map      the VST3 rule: every VST3 and CLAP build (the default)
#   brom_program_map_au   the AU rule: only the VST3 target an AUv2 component embeds
#
# Both are INTERFACE targets that carry the include root and -fno-finite-math-only.
# The VST3 SDK compiles every Xcode build with -ffast-math, which implies
# -ffinite-math-only: the compiler then folds every NaN test, and program_map.h
# refuses to compile that way. The flag must come after -ffast-math, and a target's
# own options do.
#
# A binary uses exactly one flavor: brom_record_program_flavor fails configure when a
# target is attached to both, and brom_assert_au_flavor fails it when the target an AU
# wraps is not the AU flavor.

include_guard(GLOBAL)

get_filename_component(_BROM_POKUSTON_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

foreach(_brom_flavor vst3 au)
    if(_brom_flavor STREQUAL "au")
        set(_brom_target brom_program_map_au)
    else()
        set(_brom_target brom_program_map)
    endif()
    if(NOT TARGET "${_brom_target}")
        add_library("${_brom_target}" INTERFACE)
        target_include_directories("${_brom_target}" INTERFACE "${_BROM_POKUSTON_ROOT}")
        if(NOT MSVC)
            target_compile_options("${_brom_target}" INTERFACE -fno-finite-math-only)
        endif()
        if(_brom_flavor STREQUAL "au")
            target_compile_definitions("${_brom_target}" INTERFACE BROM_PRESET_PROGRAM_AU_WRAPPER)
        endif()
        set_property(TARGET "${_brom_target}" PROPERTY BROM_PROGRAM_FLAVOR "${_brom_flavor}")
    endif()
endforeach()

# Records that `consumer` is built with `flavor` ("vst3" or "au").
function(brom_record_program_flavor consumer flavor)
    if(NOT TARGET "${consumer}")
        message(FATAL_ERROR "brom_record_program_flavor: '${consumer}' is not a target")
    endif()
    if(NOT flavor STREQUAL "vst3" AND NOT flavor STREQUAL "au")
        message(FATAL_ERROR "brom_record_program_flavor: flavor must be vst3 or au, not '${flavor}'")
    endif()
    set(flavors "")
    get_target_property(existing "${consumer}" BROM_PROGRAM_FLAVORS)
    if(existing)
        set(flavors ${existing})
    endif()
    if(NOT flavor IN_LIST flavors)
        list(APPEND flavors "${flavor}")
    endif()
    set_property(TARGET "${consumer}" PROPERTY BROM_PROGRAM_FLAVORS "${flavors}")
    list(LENGTH flavors count)
    if(count GREATER 1)
        message(FATAL_ERROR
            "Brom: target '${consumer}' links both the VST3 and the AU program flavor; "
            "a binary uses exactly one (see packages/recoil-brom/cmake/BromProgramMap.cmake)")
    endif()
endfunction()

# Call before smtg_target_add_auv2: the VST3 target an AU component wraps must carry
# the AU flavor, or the wrapper's i/N program values select the wrong preset.
function(brom_assert_au_flavor)
    cmake_parse_arguments(BROM "" "TARGET" "" ${ARGN})
    if(NOT BROM_TARGET OR NOT TARGET "${BROM_TARGET}")
        message(FATAL_ERROR "brom_assert_au_flavor: TARGET must name an existing target")
    endif()
    get_target_property(flavors "${BROM_TARGET}" BROM_PROGRAM_FLAVORS)
    if(NOT flavors STREQUAL "au")
        if(NOT flavors)
            set(flavors "no flavor (not attached to a Brom support target)")
        endif()
        message(FATAL_ERROR
            "Brom: '${BROM_TARGET}' is wrapped as an AUv2 component but carries '${flavors}'; "
            "attach it to a support target created with AU_WRAPPER")
    endif()
endfunction()

# For a plug-in that includes program_map.h from its own sources instead of linking a
# Brom support library: brom_vst3_program_rules(TARGET t SOURCES s... [AU_WRAPPER]).
# Only the named sources get -fno-finite-math-only and the flavor define (the define
# only for a target recorded as au); the plug-in's other translation units, its
# Recoil-generated DSP included, keep their float flags. The flavor is recorded on the
# target like a support library's.
function(brom_vst3_program_rules)
    cmake_parse_arguments(BROM "AU_WRAPPER" "TARGET" "SOURCES" ${ARGN})
    if(NOT BROM_TARGET OR NOT TARGET "${BROM_TARGET}" OR NOT BROM_SOURCES)
        message(FATAL_ERROR "brom_vst3_program_rules: TARGET (an existing target) and SOURCES are required")
    endif()
    if(BROM_AU_WRAPPER)
        set(flavor au)
    else()
        set(flavor vst3)
    endif()
    target_include_directories("${BROM_TARGET}" PRIVATE "${_BROM_POKUSTON_ROOT}")
    foreach(source IN LISTS BROM_SOURCES)
        if(NOT MSVC)
            set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -fno-finite-math-only)
        endif()
        # Source properties belong to the directory, not to a target, and a product
        # compiles the same source into its plain and its AU-embedded VST3 target. The
        # define is therefore a generator expression, evaluated for the target that is
        # compiling the file.
        set(au_define "$<$<IN_LIST:au,$<TARGET_PROPERTY:BROM_PROGRAM_FLAVORS>>:BROM_PRESET_PROGRAM_AU_WRAPPER>")
        get_source_file_property(existing "${source}" COMPILE_DEFINITIONS)
        if(NOT existing OR NOT au_define IN_LIST existing)
            set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_DEFINITIONS "${au_define}")
        endif()
    endforeach()
    brom_record_program_flavor("${BROM_TARGET}" "${flavor}")
endfunction()
