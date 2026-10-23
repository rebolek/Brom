Recoil-Package [
    Name: pokuston-vst3-sdk
    Version: 3.8.1
    Module-Roots: [pokuston/vst3/sdk]
    Upstream-Source: [
        ; This release asset includes the submodule sources. GitHub's tag archive does not.
        ; After composition the CMake project is in the package's vst3sdk/ directory.
        source: [zip https://github.com/pongasoft/vst3sdk/releases/download/v3.8.1_build_84/vst-sdk_3.8.1_build-84_2026-08-11.zip]
        sha256: "64965f1b74e08a6d4087a35af7a716f4dcff5852c66ad7ee13f1c47e79c1ab77"
        override-env: POKUSTON_VST3_SDK_SOURCE
        local-cache-file: %vst-sdk_3.8.1_build-84_2026-08-11.zip
    ]
]
