Rebol [
    Title: "Loos plug-in manifest"
    Description: {
        What Loos is. packages/recoil-brom/plugin-build.r3 owns how it is
        built; clap/build.r3 and vst3/build.r3 each load this and ask for their
        own format, so the two cannot disagree about the plug-in's identity.
    }
]

[
    name       "Loos"
    id         "com.pokuston.loos"
    version    "0.2.0"
    vendor     "Pokuston"
    root       %examples/pokuston/loos/
    project    %examples/pokuston/loos/clap/
    projects   [
        clap  %examples/pokuston/loos/clap/
        vst3  %examples/pokuston/loos/vst3/
        au    %examples/pokuston/loos/au/
    ]
    descriptor %plugin.rcl
    core       %engine-export.rcl
    ; CLAP imports the contract into its shell translation unit. VST3/AUv2
    ; still transpile `core` separately because their C++ bridge links it once.
    entry-carries-core true
    entry      %clap_entry.rcl
    editor     %editor.rcl
    adapter    %loos_clap_editor_adapter.c
    formats    [clap vst3 au]
    manifest-file "examples/pokuston/loos/plugin.reb"
]
