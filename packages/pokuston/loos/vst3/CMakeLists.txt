cmake_minimum_required(VERSION 3.21)

project(pokuston_loos
    VERSION 0.2.0
    DESCRIPTION "Pokuston Loos VST3 and AUv2 targets"
    LANGUAGES C CXX
)

if(APPLE)
    enable_language(OBJCXX)
endif()

# Steinberg's base library requires one explicit build-configuration macro
# outside the Xcode generator as well.
add_compile_definitions(RELEASE)

# The Rebol build driver produces this C file and the two matching PIC objects.
set(RECOIL_PLUGIN_C "" CACHE FILEPATH "Transpiled Recoil synth core")
set(RECOIL_ROOT "" CACHE PATH "Selected Recoil checkout")
set(POKUSTON_ROOT "" CACHE PATH "Pokuston product checkout")
set(RECOIL_RUNTIME_OBJECT "" CACHE FILEPATH "PIC Recoil runtime object")
set(RECOIL_INTERNAL_OBJECT "" CACHE FILEPATH "PIC Recoil internal object")
set(RECOIL_TLS_BACKEND_STUB_OBJECT "" CACHE FILEPATH "PIC Recoil TLS backend stub object")
set(RECOIL_GUI_LIBRARIES "" CACHE STRING "Ordered gui/pugl package archives")
set(RECOIL_GUI_INCLUDE_DIRS "" CACHE STRING "gui/pugl package include directories")
set(RECOIL_GUI_SYSTEM_LIBRARIES "" CACHE STRING "gui/pugl package system libraries")
set(RECOIL_GUI_FRAMEWORKS "" CACHE STRING "gui/pugl package Apple frameworks")
set(VST3_SDK_PATH "" CACHE PATH "Checkout of steinbergmedia/vst3sdk")
set(AUDIOUNIT_SDK_PATH "" CACHE PATH "Checkout of apple/AudioUnitSDK (macOS only)")

# The Recoil runtime and package artifacts are cached for the native Rebol ABI,
# not as universal binaries. Do not let the SDK re-enable its universal target
# policy when an explicit matching macOS architecture was requested.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(SMTG_BUILD_UNIVERSAL_BINARY OFF CACHE BOOL "Build VST SDK targets as universal binaries" FORCE)
endif()

foreach(required_path RECOIL_ROOT POKUSTON_ROOT RECOIL_PLUGIN_C RECOIL_RUNTIME_OBJECT RECOIL_INTERNAL_OBJECT RECOIL_TLS_BACKEND_STUB_OBJECT VST3_SDK_PATH)
    if(NOT EXISTS "${${required_path}}")
        message(FATAL_ERROR "${required_path} must name an existing file or directory")
    endif()
endforeach()
foreach(required_library IN LISTS RECOIL_GUI_LIBRARIES)
    if(NOT EXISTS "${required_library}")
        message(FATAL_ERROR "Resolved gui/pugl archive does not exist: ${required_library}")
    endif()
endforeach()

# `smtg_enable_vst3_sdk()` discovers the AU SDK while it initializes the
# VST3 build. Set this before loading the SDK rather than after the fact.
if(APPLE AND AUDIOUNIT_SDK_PATH AND EXISTS "${AUDIOUNIT_SDK_PATH}")
    set(SMTG_AUDIOUNIT_SDK_PATH "${AUDIOUNIT_SDK_PATH}" CACHE PATH "Apple AudioUnitSDK checkout" FORCE)
    set(SMTG_ENABLE_AUV2_BUILDS ON CACHE BOOL "Build AUv2 target" FORCE)
endif()

# Hosts and CI can run their preferred validator explicitly. The SDK's optional
# post-link validator is not part of producing a distributable plug-in.
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "Run the VST3 SDK validator after linking" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "Create a development VST3 install symlink" FORCE)

add_subdirectory("${VST3_SDK_PATH}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
smtg_enable_vst3_sdk()

# A local development build must not require an Apple Development certificate.
# Pass -DLOOS_CODE_SIGN_IDENTITY='Apple Development: …' for a distributable
# signed component.
set(LOOS_CODE_SIGN_IDENTITY "-" CACHE STRING "macOS signing identity for Loos bundles")
set(SMTG_CODE_SIGN_IDENTITY_MAC "${LOOS_CODE_SIGN_IDENTITY}" CACHE STRING
    "macOS Code Sign Identity" FORCE)

# The editor is Recoil (../editor.rcl), transpiled by build.r3 before CMake
# runs and handed over as RECOIL_EDITOR_C. The hand-written ../editor.c it was
# ported from has been removed; it had drifted away from the port.
include("${POKUSTON_ROOT}/packages/recoil-brom/cmake/BromProgramMap.cmake")

# The product's VST3 target, defined once and instantiated twice: the plain VST3 (the
# default program rule) and, on macOS, the module the AUv2 component embeds (the AU
# rule, under its own package name so the driver never installs it as a VST3).
function(pokuston_loos_add_vst3 target flavor package_name bundle_id)
    set(package_args "")
    if(package_name)
        set(package_args PACKAGE_NAME "${package_name}")
    endif()
    set(flavor_args "")
    if(flavor STREQUAL "au")
        set(flavor_args AU_WRAPPER)
    endif()
    smtg_add_vst3plugin(${target} ${package_args}
        SOURCES_LIST
        source/loos_vst3.cpp
        "${RECOIL_EDITOR_C}"
        "${RECOIL_PLUGIN_C}"
    )

    # The program rule (BromProgramMap.cmake): the VST3 rule, with the float flag its header needs.
    brom_vst3_program_rules(TARGET ${target} SOURCES source/loos_vst3.cpp ${flavor_args})

    set_source_files_properties("${RECOIL_PLUGIN_C}" PROPERTIES LANGUAGE C)
    set_source_files_properties("${RECOIL_EDITOR_C}" PROPERTIES LANGUAGE C)

    target_include_directories(${target} PRIVATE
        "${RECOIL_ROOT}"
        "${RECOIL_ROOT}/lib"
        "${POKUSTON_ROOT}"
        # editor.rcl reaches loos.h through `foreign <loos.h>`, which emits an
        # angle-bracket include, so the example root has to be on the search path.
        "${CMAKE_CURRENT_LIST_DIR}/.."
        ${RECOIL_GUI_INCLUDE_DIRS}
    )

    target_link_libraries(${target} PRIVATE
        sdk
        "${RECOIL_RUNTIME_OBJECT}"
        "${RECOIL_INTERNAL_OBJECT}"
        "${RECOIL_TLS_BACKEND_STUB_OBJECT}"
        ${RECOIL_GUI_LIBRARIES}
        ${RECOIL_GUI_SYSTEM_LIBRARIES}
    )

    foreach(recoil_framework IN LISTS RECOIL_GUI_FRAMEWORKS)
        target_link_libraries(${target} PRIVATE "-framework ${recoil_framework}")
    endforeach()

    if(NOT WIN32)
        target_link_libraries(${target} PRIVATE m)
    endif()

    smtg_target_configure_version_file(${target})

    # The VST3 SDK defaults its plug-in targets to a universal macOS build. Recoil
    # native objects are deliberately cached for one host ABI, so keep the target
    # on CMAKE_OSX_ARCHITECTURES selected by build.r3.
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set_target_properties(${target} PROPERTIES
            OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}"
            XCODE_ATTRIBUTE_OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}"
            XCODE_ATTRIBUTE_ARCHS "${CMAKE_OSX_ARCHITECTURES}"
        )
    endif()
    if(SMTG_MAC)
        smtg_target_set_bundle(${target}
            BUNDLE_IDENTIFIER "${bundle_id}"
            COMPANY_NAME "Pokuston"
        )
    endif()
endfunction()

pokuston_loos_add_vst3(pokuston_loos_vst3 vst3 "" com.pokuston.loos)

if(SMTG_MAC)
    # The VST3 SDK's AUv2 wrapper provides the component-manager ABI. Keeping
    # it around the VST processor means VST3 and AU share one event/DSP bridge.
    list(APPEND CMAKE_MODULE_PATH "${VST3_SDK_PATH}/cmake/modules")
    include(SMTG_AddVST3AuV2)
    if(SMTG_ENABLE_AUV2_BUILDS AND COMMAND smtg_target_add_auv2)
        pokuston_loos_add_vst3(pokuston_loos_vst3_au au Loos-au-embedded com.pokuston.loos.au-embedded)
        brom_assert_au_flavor(TARGET pokuston_loos_vst3_au)
        smtg_target_add_auv2(pokuston_loos_au
            BUNDLE_NAME Loos
            BUNDLE_IDENTIFIER com.pokuston.loos.au
            INFO_PLIST_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/resource/au-info.plist"
            VST3_PLUGIN_TARGET pokuston_loos_vst3_au
        )
        if(CMAKE_OSX_ARCHITECTURES)
            set_target_properties(pokuston_loos_au PROPERTIES
                OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}"
                XCODE_ATTRIBUTE_OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}"
                XCODE_ATTRIBUTE_ARCHS "${CMAKE_OSX_ARCHITECTURES}"
            )
        endif()
        # Current AudioUnitSDK headers use C++23 std::expected (as well as
        # C++20 concepts/span); the VST3 SDK wrapper still declares C++17.
        set_target_properties(pokuston_loos_au PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED YES
        )
    else()
        message(STATUS "AUv2 target disabled: it requires the Xcode generator and AUDIOUNIT_SDK_PATH")
    endif()
endif()
