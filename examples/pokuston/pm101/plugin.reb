Rebol [
    Title: "PM-101 plug-in manifest"
    Description: {
        What PM-101 is. packages/recoil-brom/plugin-build.r3 owns how it is
        built; clap/build.r3 loads this and asks for its own format, so the
        formats cannot disagree about the plug-in's identity.

        CLAP only for now. The VST3 and AUv2 projects need the same glue Loos
        carries under vst3/ and au/ (a C++ bridge, a CMake project and an
        Info.plist); they are not written yet, which is why `formats` lists one.
    }
]

[
    name       "PM-101"
    id         "com.pokuston.pm101"
    version    "0.5.0"
    vendor     "Pokuston"
    root       %examples/pokuston/pm101/
    project    %examples/pokuston/pm101/clap/
    projects   [
        clap  %examples/pokuston/pm101/clap/
    ]
    descriptor %plugin.rcl
    core       %engine-export.rcl
    entry-carries-core true
    entry      %clap_entry.rcl
    editor     %editor.rcl
    adapter    %pm101_clap_editor_adapter.c
    formats    [clap]
    manifest-file "examples/pokuston/pm101/plugin.reb"
]
