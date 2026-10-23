# The generic Brom effect AUv2 target, shared by every effect's AU project.
#
# A product's au/CMakeLists.txt declares its project (languages C, CXX and
# OBJCXX), includes this file, calls brom_effect_au_setup() and then
# brom_add_effect_au() with the component subtype and manufacturer codes that
# identify it. Everything else -- the shell source, the AudioUnitSDK, the
# transpiled Recoil entry, the runtime objects, the Info.plist and the bundle --
# is the same for every effect, so it lives here once.
#
# The codes are packaging data, not ABI: they must never change once a version
# ships, which is why they are written in the product's project and not
# derived from anything. The component type is aufx (kAudioUnitType_Effect),
# or aumf (kAudioUnitType_MusicEffect) for an effect that declares MIDI_INPUT:
# hosts route MIDI only to music effects, so an effect that must receive MIDI
# declares it. The type is part of the identity too.

set(BROM_EFFECT_AU_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The cache variables the Pokuston build driver passes, the AudioUnitSDK, and
# the release defaults. A macro, so the settings land in the caller's scope.
macro(brom_effect_au_setup)
    if(NOT APPLE)
        message(FATAL_ERROR "The Brom AU effect shell builds only on macOS")
    endif()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    set(RECOIL_PLUGIN_C "" CACHE FILEPATH "Transpiled Recoil entry: the engine and the effect shell interface")
    set(RECOIL_ROOT "" CACHE PATH "Selected Recoil checkout")
    set(POKUSTON_ROOT "" CACHE PATH "Pokuston product checkout")
    set(RECOIL_RUNTIME_OBJECT "" CACHE FILEPATH "PIC Recoil runtime object")
    set(RECOIL_INTERNAL_OBJECT "" CACHE FILEPATH "PIC Recoil internal object")
    set(RECOIL_TLS_BACKEND_STUB_OBJECT "" CACHE FILEPATH "PIC TLS backend stub")
    set(AUDIOUNIT_SDK_PATH "" CACHE PATH "Pinned Apple AudioUnitSDK checkout")
    # Used only by an effect with an editor (brom_add_effect_au EDITOR).
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
            RECOIL_INTERNAL_OBJECT RECOIL_TLS_BACKEND_STUB_OBJECT AUDIOUNIT_SDK_PATH)
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
    if(NOT EXISTS "${AUDIOUNIT_SDK_PATH}/include/AudioUnitSDK/AUBase.h")
        message(FATAL_ERROR "AUDIOUNIT_SDK_PATH lacks include/AudioUnitSDK/AUBase.h")
    endif()

    # The SDK's sources, compiled once into the bundle: AUBase and its
    # elements, buffers and plug-in dispatch. AUEffectBase and the MIDI/music
    # bases are not used; the shell derives from AUBase directly.
    if(NOT TARGET brom_audiounit_sdk)
        set(brom_au_sdk_src "${AUDIOUNIT_SDK_PATH}/src/AudioUnitSDK")
        add_library(brom_audiounit_sdk STATIC
            "${brom_au_sdk_src}/AUBase.cpp"
            "${brom_au_sdk_src}/AUBuffer.cpp"
            "${brom_au_sdk_src}/AUBufferAllocator.cpp"
            "${brom_au_sdk_src}/AUInputElement.cpp"
            "${brom_au_sdk_src}/AUOutputElement.cpp"
            "${brom_au_sdk_src}/AUPlugInDispatch.cpp"
            "${brom_au_sdk_src}/AUScopeElement.cpp"
            "${brom_au_sdk_src}/ComponentBase.cpp"
        )
        target_include_directories(brom_audiounit_sdk PUBLIC "${AUDIOUNIT_SDK_PATH}/include")
        target_compile_features(brom_audiounit_sdk PUBLIC cxx_std_23)
        set_target_properties(brom_audiounit_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_compile_options(brom_audiounit_sdk PRIVATE -w)
    endif()
endmacro()

# brom_add_effect_au(<target>
#     SUBTYPE <4cc> MANUFACTURER <4cc> [MIDI_INPUT] [TAGS <tag>...] [SOURCES <extra>...]
#     [EDITOR EDITOR_SOURCES <adapter>... [INCLUDE_DIRECTORIES <dir>...]])
#
# Produces ${CMAKE_BINARY_DIR}/<target>.component. MIDI_INPUT publishes the
# component as aumf, a music effect, so hosts send it MIDI 1.0 through
# MusicDeviceMIDIEvent; without it the component is aufx. TAGS are the component's
# AudioComponents tags (default: Effects); they are build-time data because
# the Info.plist is read without loading code. EDITOR builds the editor hook
# (brom/plugin/au/effect_editor.h): the transpiled editor (RECOIL_EDITOR_C),
# its embedded assets, the GUI package graph, and EDITOR_SOURCES, which define
# the brom_au_editor_* functions.
function(brom_add_effect_au target)
    cmake_parse_arguments(BROM "EDITOR;MIDI_INPUT" "SUBTYPE;MANUFACTURER"
        "TAGS;SOURCES;EDITOR_SOURCES;INCLUDE_DIRECTORIES" ${ARGN})
    foreach(brom_code SUBTYPE MANUFACTURER)
        string(LENGTH "${BROM_${brom_code}}" brom_code_length)
        if(NOT brom_code_length EQUAL 4)
            message(FATAL_ERROR "brom_add_effect_au needs a four-character ${brom_code}")
        endif()
    endforeach()
    if(NOT BROM_TAGS)
        set(BROM_TAGS Effects)
    endif()

    add_library(${target} MODULE
        "${BROM_EFFECT_AU_DIR}/effect_au.mm"
        "${RECOIL_PLUGIN_C}"
        ${BROM_SOURCES}
    )
    set_source_files_properties("${RECOIL_PLUGIN_C}" PROPERTIES LANGUAGE C)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_include_directories(${target} PRIVATE
        "${RECOIL_ROOT}"
        "${RECOIL_ROOT}/lib"
        "${POKUSTON_ROOT}"
    )
    target_link_libraries(${target} PRIVATE
        brom_audiounit_sdk
        "${RECOIL_RUNTIME_OBJECT}"
        "${RECOIL_INTERNAL_OBJECT}"
        "${RECOIL_TLS_BACKEND_STUB_OBJECT}"
        m
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework CoreMIDI"
        "-framework Foundation"
    )
    if(BROM_INCLUDE_DIRECTORIES)
        target_include_directories(${target} PRIVATE ${BROM_INCLUDE_DIRECTORIES})
    endif()

    # Objective-C class names share one namespace across every plug-in a host
    # loads, so the view classes carry the product's codes.
    string(MAKE_C_IDENTIFIER "${BROM_MANUFACTURER}${BROM_SUBTYPE}" brom_au_class_tag)
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
        target_link_libraries(${target} PRIVATE ${RECOIL_GUI_LIBRARIES} ${RECOIL_GUI_SYSTEM_LIBRARIES}
            "-framework Cocoa")
        foreach(brom_framework IN LISTS RECOIL_GUI_FRAMEWORKS)
            target_link_libraries(${target} PRIVATE "-framework ${brom_framework}")
        endforeach()
        target_compile_definitions(${target} PRIVATE
            BROM_AU_EDITOR=1
            "BROM_AU_VIEW_FACTORY_CLASS=BromAUViewFactory_${brom_au_class_tag}"
            "BROM_AU_VIEW_CLASS=BromAUView_${brom_au_class_tag}"
        )
    endif()
    target_compile_definitions(${target} PRIVATE
        POKUSTON_PLUGIN_NAME="${POKUSTON_PLUGIN_NAME}"
        POKUSTON_PLUGIN_ID="${POKUSTON_PLUGIN_ID}"
        POKUSTON_PLUGIN_VERSION="${POKUSTON_PLUGIN_VERSION}"
        POKUSTON_PLUGIN_VENDOR="${POKUSTON_PLUGIN_VENDOR}"
    )

    # Info.plist: identity from the manifest and this call, version as AU's
    # 0xMMMMmmbb integer.
    string(REPLACE "." ";" brom_version_parts "${POKUSTON_PLUGIN_VERSION}")
    list(LENGTH brom_version_parts brom_version_count)
    if(NOT brom_version_count EQUAL 3)
        message(FATAL_ERROR "POKUSTON_PLUGIN_VERSION must be major.minor.patch")
    endif()
    list(GET brom_version_parts 0 brom_version_major)
    list(GET brom_version_parts 1 brom_version_minor)
    list(GET brom_version_parts 2 brom_version_patch)
    math(EXPR BROM_AU_VERSION
        "(${brom_version_major} << 16) | (${brom_version_minor} << 8) | ${brom_version_patch}")
    set(BROM_AU_EXECUTABLE "${target}")
    set(BROM_AU_BUNDLE_ID "${POKUSTON_PLUGIN_ID}.au")
    if(BROM_MIDI_INPUT)
        set(BROM_AU_TYPE aumf)
    else()
        set(BROM_AU_TYPE aufx)
    endif()
    set(BROM_AU_SUBTYPE "${BROM_SUBTYPE}")
    set(BROM_AU_MANUFACTURER "${BROM_MANUFACTURER}")
    set(BROM_AU_TAGS_XML "")
    foreach(brom_tag IN LISTS BROM_TAGS)
        string(APPEND BROM_AU_TAGS_XML "                <string>${brom_tag}</string>\n")
    endforeach()
    string(STRIP "${BROM_AU_TAGS_XML}" BROM_AU_TAGS_XML)
    set(brom_plist "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
    configure_file("${BROM_EFFECT_AU_DIR}/Info.plist.in" "${brom_plist}" @ONLY)

    set_target_properties(${target} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION component
        OUTPUT_NAME "${target}"
        MACOSX_BUNDLE_INFO_PLIST "${brom_plist}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        CXX_VISIBILITY_PRESET hidden
        OBJCXX_VISIBILITY_PRESET hidden
    )
    if(CMAKE_OSX_ARCHITECTURES)
        set_target_properties(${target} PROPERTIES OSX_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    # Ad-hoc signature over the whole bundle, for local loading.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND codesign --force --sign - "${CMAKE_BINARY_DIR}/${target}.component"
        VERBATIM
    )
endfunction()
