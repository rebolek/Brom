cmake_minimum_required(VERSION 3.21)

project(brom_probe_effect_vst3 VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

include("${POKUSTON_ROOT}/packages/recoil-brom/brom/plugin/vst3/brom-effect-vst3.cmake")
brom_effect_vst3_setup()

# Test identifiers: the probe is not shipped.
brom_add_effect_vst3(brom_probe_vst3
    PROCESSOR_UID "0x4252504F,0x42455646,0x58505230,0x43455353"
    CONTROLLER_UID "0x4252504F,0x42455646,0x58435452,0x4C4C4552"
)

# The generic VST3 contract runs as part of the build, against the bundle just
# linked, so a RUT title only has to build the project and read the output.
if(APPLE)
    add_executable(brom-effect-vst3-host "${POKUSTON_ROOT}/tests/fixtures/brom-effect-vst3-host.cpp")
    target_include_directories(brom-effect-vst3-host PRIVATE "${POKUSTON_ROOT}")
    target_link_libraries(brom-effect-vst3-host PRIVATE sdk sdk_hosting)
    target_link_libraries(brom-effect-vst3-host PRIVATE "-framework Cocoa" "-framework CoreFoundation")
    set_target_properties(brom-effect-vst3-host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/host"
    )
    add_custom_target(brom-effect-vst3-contract ALL
        COMMAND "$<TARGET_FILE:brom-effect-vst3-host>"
                "${CMAKE_BINARY_DIR}/VST3/$<CONFIG>/brom_probe_vst3.vst3"
        DEPENDS brom-effect-vst3-host brom_probe_vst3
        VERBATIM
    )
endif()
