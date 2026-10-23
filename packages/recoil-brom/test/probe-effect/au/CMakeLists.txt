cmake_minimum_required(VERSION 3.21)

project(brom_probe_effect_au VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

include("${POKUSTON_ROOT}/packages/recoil-brom/brom/plugin/au/brom-effect-au.cmake")
brom_effect_au_setup()

# Test identifiers: the probe is not shipped. It takes MIDI, so it is a music
# effect (aumf), the type a host routes MIDI to.
brom_add_effect_au(brom_probe_au
    SUBTYPE "BrPr"
    MANUFACTURER "PkTs"
    MIDI_INPUT
    TAGS Effects
)

# The generic AU contract runs as part of the build, against the component
# just linked, loaded from the build directory and registered in-process with
# AudioComponentRegister, so nothing is installed. A RUT title only has to
# build the project and read the output.
add_executable(brom-effect-au-host "${POKUSTON_ROOT}/tests/fixtures/brom-effect-au-host.cpp")
target_include_directories(brom-effect-au-host PRIVATE "${POKUSTON_ROOT}")
target_compile_features(brom-effect-au-host PRIVATE cxx_std_23)
target_link_libraries(brom-effect-au-host PRIVATE
    "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation"
    "-framework Foundation")
set_target_properties(brom-effect-au-host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host")
add_custom_target(brom-effect-au-contract ALL
    COMMAND "$<TARGET_FILE:brom-effect-au-host>" "${CMAKE_BINARY_DIR}/brom_probe_au.component"
    DEPENDS brom-effect-au-host brom_probe_au
    VERBATIM
)
