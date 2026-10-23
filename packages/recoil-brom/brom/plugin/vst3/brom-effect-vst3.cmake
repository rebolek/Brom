# The generic Brom effect VST3 target, shared by every effect's VST3 project.
#
# A product's vst3/CMakeLists.txt declares its project, includes this file,
# calls brom_effect_vst3_setup() and then brom_add_effect_vst3() with the two
# FUIDs that identify it. Everything else -- the shell source, the transpiled
# Recoil entry, the runtime objects and the bundle -- is the same for every
# effect, so it lives here once.
#
# The FUIDs are packaging data, not ABI: they must never change once a version
# ships, which is why they are written in the product's project and not
# derived from anything.

set(BROM_EFFECT_VST3_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The cache variables the Pokuston build driver passes, the SDK, and the
# release defaults. A macro, so the SDK's settings land in the caller's scope.
macro(brom_effect_vst3_setup)
    add_compile_definitions(RELEASE)

    set(RECOIL_PLUGIN_C "" CACHE FILEPATH "Transpiled Recoil entry: the engine and the effect shell interface")
    set(RECOIL_ROOT "" CACHE PATH "Selected Recoil checkout")
    set(POKUSTON_ROOT "" CACHE PATH "Pokuston product checkout")
    set(RECOIL_RUNTIME_OBJECT "" CACHE FILEPATH "PIC Recoil runtime object")
    set(RECOIL_INTERNAL_OBJECT "" CACHE FILEPATH "PIC Recoil internal object")
    set(RECOIL_TLS_BACKEND_STUB_OBJECT "" CACHE FILEPATH "PIC TLS backend stub")
    set(VST3_SDK_PATH "" CACHE PATH "Pinned VST3 SDK checkout")
    # Used only by an effect with an editor (brom_add_effect_vst3 EDITOR).
    set(RECOIL_EDITOR_C "" CACHE FILEPATH "Transpiled product editor")
    set(POKUSTON_EDITOR_ASSETS_C "" CACHE FILEPATH "Generated embedded editor PNG resources")
    set(RECOIL_GUI_LIBRARIES "" CACHE STRING "Resolved GUI package archives")
    set(RECOIL_GUI_INCLUDE_DIRS "" CACHE STRING "Resolved GUI include directories")
    set(RECOIL_GUI_SYSTEM_LIBRARIES "" CACHE STRING "Resolved GUI system libraries")
    set(RECOIL_GUI_FRAMEWORKS "" CACHE STRING "Resolved GUI frameworks")
    set(POKUSTON_PLUGIN_NAME "" CACHE STRING "Manifest plug-in name")
    set(POKUSTON_PLUGIN_ID "" CACHE STRING "Manifest plug-in identifier")
    set(POKUSTON_PLUGIN_VERSION "" CACHE STRING "Manifest plug-in version")
    set(POKUSTON_PLUGIN_VENDOR "" CACHE STRING "Manifest plug-in vendor")

    foreach(brom_required_path RECOIL_ROOT POKUSTON_ROOT RECOIL_PLUGIN_C RECOIL_RUNTIME_OBJECT
            RECOIL_INTERNAL_OBJECT RECOIL_TLS_BACKEND_STUB_OBJECT VST3_SDK_PATH)
        if(NOT EXISTS "${${brom_required_path}}")
            message(FATAL_ERROR "${brom_required_path} must name an existing file or directory")
        endif()
    endforeach()
    foreach(brom_required_identity POKUSTON_PLUGIN_NAME POKUSTON_PLUGIN_ID
            POKUSTON_PLUGIN_VERSION POKUSTON_PLUGIN_VENDOR)
        if("${${brom_required_identity}}" STREQUAL "")
            message(FATAL_ERROR "${brom_required_identity} must come from the plug-in manifest")
        endif()
    endforeach()

    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "Run the SDK validator after linking" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "Create a development install symlink" FORCE)
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(SMTG_BUILD_UNIVERSAL_BINARY OFF CACHE BOOL "Build one native architecture" FORCE)
    endif()

    add_subdirectory("${VST3_SDK_PATH}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    smtg_enable_vst3_sdk()
    set(SMTG_CODE_SIGN_IDENTITY_MAC "-" CACHE STRING "Local unsigned build" FORCE)
endmacro()

# brom_add_effect_vst3(<target>
#     PROCESSOR_UID <w0,w1,w2,w3> CONTROLLER_UID <w0,w1,w2,w3> [SOURCES <extra>...]
#     [EDITOR EDITOR_SOURCES <adapter>... [INCLUDE_DIRECTORIES <dir>...]])
#
# EDITOR builds the editor hook (brom/plugin/vst3/effect_editor.h): the
# transpiled editor (RECOIL_EDITOR_C), its embedded assets, the GUI package
# graph, and EDITOR_SOURCES, which define the brom_vst3_editor_* functions.
function(brom_add_effect_vst3 target)
    cmake_parse_arguments(BROM "EDITOR" "PROCESSOR_UID;CONTROLLER_UID"
        "SOURCES;EDITOR_SOURCES;INCLUDE_DIRECTORIES" ${ARGN})
    if(NOT BROM_PROCESSOR_UID OR NOT BROM_CONTROLLER_UID)
        message(FATAL_ERROR "brom_add_effect_vst3 needs PROCESSOR_UID and CONTROLLER_UID")
    endif()

    smtg_add_vst3plugin(${target}
        "${BROM_EFFECT_VST3_DIR}/effect_vst3.cpp"
        "${RECOIL_PLUGIN_C}"
        ${BROM_SOURCES}
    )
    set_source_files_properties("${RECOIL_PLUGIN_C}" PROPERTIES LANGUAGE C)
    target_include_directories(${target} PRIVATE
        "${RECOIL_ROOT}"
        "${RECOIL_ROOT}/lib"
        "${POKUSTON_ROOT}"
    )
    target_link_libraries(${target} PRIVATE
        sdk
        "${RECOIL_RUNTIME_OBJECT}"
        "${RECOIL_INTERNAL_OBJECT}"
        "${RECOIL_TLS_BACKEND_STUB_OBJECT}"
    )
    if(NOT WIN32)
        target_link_libraries(${target} PRIVATE m)
    endif()
    if(BROM_INCLUDE_DIRECTORIES)
        target_include_directories(${target} PRIVATE ${BROM_INCLUDE_DIRECTORIES})
    endif()
    if(BROM_EDITOR)
        if(NOT EXISTS "${RECOIL_EDITOR_C}" OR NOT BROM_EDITOR_SOURCES)
            message(FATAL_ERROR "an EDITOR effect needs RECOIL_EDITOR_C and EDITOR_SOURCES")
        endif()
        target_sources(${target} PRIVATE "${RECOIL_EDITOR_C}" ${BROM_EDITOR_SOURCES})
        set_source_files_properties("${RECOIL_EDITOR_C}" PROPERTIES LANGUAGE C)
        if(POKUSTON_EDITOR_ASSETS_C)
            target_sources(${target} PRIVATE "${POKUSTON_EDITOR_ASSETS_C}")
            get_filename_component(brom_assets_dir "${POKUSTON_EDITOR_ASSETS_C}" DIRECTORY)
            target_include_directories(${target} PRIVATE "${brom_assets_dir}")
        endif()
        target_include_directories(${target} PRIVATE ${RECOIL_GUI_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${RECOIL_GUI_LIBRARIES} ${RECOIL_GUI_SYSTEM_LIBRARIES})
        foreach(brom_framework IN LISTS RECOIL_GUI_FRAMEWORKS)
            target_link_libraries(${target} PRIVATE "-framework ${brom_framework}")
        endforeach()
        target_compile_definitions(${target} PRIVATE BROM_VST3_EDITOR=1)
    endif()
    target_compile_definitions(${target} PRIVATE
        "BROM_VST3_PROCESSOR_UID=${BROM_PROCESSOR_UID}"
        "BROM_VST3_CONTROLLER_UID=${BROM_CONTROLLER_UID}"
        POKUSTON_PLUGIN_NAME="${POKUSTON_PLUGIN_NAME}"
        POKUSTON_PLUGIN_ID="${POKUSTON_PLUGIN_ID}"
        POKUSTON_PLUGIN_VERSION="${POKUSTON_PLUGIN_VERSION}"
        POKUSTON_PLUGIN_VENDOR="${POKUSTON_PLUGIN_VENDOR}"
    )
    smtg_target_configure_version_file(${target})
    if(APPLE)
        smtg_target_set_bundle(${target}
            BUNDLE_IDENTIFIER "${POKUSTON_PLUGIN_ID}"
            COMPANY_NAME "${POKUSTON_PLUGIN_VENDOR}"
        )
        if(CMAKE_OSX_ARCHITECTURES)
            set_target_properties(${target} PROPERTIES
                OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}"
                XCODE_ATTRIBUTE_OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}"
                XCODE_ATTRIBUTE_ARCHS "${CMAKE_OSX_ARCHITECTURES}"
            )
        endif()
    endif()
endfunction()
