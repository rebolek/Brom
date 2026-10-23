Rebol [
    Title: "Brom probe effect au build"
    Description: {
        What the probe IS lives in ../plugin.reb; how it is built lives in
        packages/recoil-brom/plugin-build.r3. This script only says which
        plug-in and which format.

        Usage:  r3 -s packages/recoil-brom/test/probe-effect/au/build.r3 [--no-install]
    }
]

do %../../../build-bootstrap.r3

run-pokuston-build-script %packages/recoil-brom/test/probe-effect/plugin.reb [au] pokuston-build-args
