Rebol [Title: "Brom probe effect manifest"]

; A test effect, not a product: it exists so the shared shells can be driven
; through every effect ABI path (f64, sidechain, latency, tail) by a fixture.
[
    name       "BromProbe"
    id         "com.pokuston.brom-probe"
    version    "0.1.0"
    vendor     "Pokuston"
    root       %packages/recoil-brom/test/probe-effect/
    projects   [
        clap %packages/recoil-brom/test/probe-effect/clap/
        vst3 %packages/recoil-brom/test/probe-effect/vst3/
        au   %packages/recoil-brom/test/probe-effect/au/
    ]
    descriptor %plugin.rcl
    core       %plugin.rcl
    entry-carries-core true
    support    %probe_editor_stub.c
    vst3-entry %vst3_entry.rcl
    au-entry   %au_entry.rcl
    formats    [clap vst3 au]
    manifest-file "packages/recoil-brom/test/probe-effect/plugin.reb"
]
