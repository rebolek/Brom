Recoil-Package [
    Name: pokuston-audio-unit-sdk
    Version: 1.4.0
    Module-Roots: [pokuston/audio-unit/sdk]
    Upstream-Source: [
        source: [tarball https://github.com/apple/AudioUnitSDK/archive/refs/tags/AudioUnitSDK-1.4.0.tar.gz]
        sha256: "a3ac2f94e8df5370df46f8bdf5e34cc07d1329d011bfa61503c8e503309c29e7"
        override-env: POKUSTON_AUDIO_UNIT_SDK_SOURCE
        local-cache-file: %AudioUnitSDK-1.4.0.tar.gz
    ]
]
