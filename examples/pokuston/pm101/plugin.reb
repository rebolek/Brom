Rebol [
    Title: "PM-101 plug-in manifest"
    Description: {
        What PM-101 is. packages/recoil-brom/plugin-build.r3 owns how it is
        built; clap/build.r3 loads this and asks for its format.

        CLAP only for now. VST3 and AUv2 need an instrument C++ bridge of their
        own, as Loos has in vst3/source/loos_vst3.cpp; the generic Brom VST3
        and AU shells are effect shells.
    }
]

[
    name       "PM-101"
    slug       "pm101"
    id         "com.pokuston.pm101"
    bundle-id  "com.pokuston.pm101.clap"
    version    "0.1.0"
    vendor     "Pokuston"
    root       %examples/pokuston/pm101/
    project    %examples/pokuston/pm101/clap/
    projects   [
        clap  %examples/pokuston/pm101/clap/
    ]
    descriptor %plugin.rcl
    core       %engine-export.rcl
    ; CLAP imports the contract into its shell translation unit.
    entry-carries-core true
    entry      %clap_entry.rcl
    ; No editor yet: the descriptor says has-editor false, and the shared CLAP
    ; support still links the adapter entry points, so a stub supplies them.
    support    %pm101_editor_stub.c
    formats    [clap]
    manifest-file "examples/pokuston/pm101/plugin.reb"
]
