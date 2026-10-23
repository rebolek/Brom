Rebol [
    Title: "Loos clap build"
    Description: {
        What the plug-in IS lives in ../plugin.reb; how it is built lives in
        packages/recoil-brom/plugin-build.r3. This script only says which
        plug-in and which format.

        Usage:  r3 -s examples/pokuston/loos/clap/build.r3 [--no-install] [--dist]
    }
]

do %../../../../packages/recoil-brom/build-bootstrap.r3

run-pokuston-build-script %examples/pokuston/loos/plugin.reb [clap] pokuston-build-args
