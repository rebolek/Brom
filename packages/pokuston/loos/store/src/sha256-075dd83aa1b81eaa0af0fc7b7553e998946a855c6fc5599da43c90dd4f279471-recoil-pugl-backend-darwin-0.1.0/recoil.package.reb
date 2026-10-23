Recoil-Package [
    Name: recoil-pugl-backend-darwin
    Version: 0.1.0
    Module-Roots: [recoil/pugl/backend/darwin]
    Install: [
        Provides: [pugl-backend]
        Selectors: [[Target: native Host-Platform: macOS]]
    ]
    Upstream-Source: [
        source: [tarball https://github.com/lv2/pugl/archive/b7637149ebe53124e5be90559e02a0185bbcbd73.tar.gz]
        sha256: "5220dcda06112f77476f68654c5b9072f2ec07b996b866ebd44cfa1a81482ebf"
        override-env: RECOIL_PUGL_SOURCE
        local-cache-file: %pugl-b7637149ebe53124e5be90559e02a0185bbcbd73.tar.gz
    ]
    Vendor: [
        Build: [
            [
                name: pugl
                kind: static
                output: %native/out/libpugl.a
                sources: [
                    %src/common.c
                    %src/internal.c
                    %src/mac.m
                    %src/mac_gl.m
                ]
                includes: [%src/ %include/]
                defines: [PUGL_INTERNAL]
                compile-flags: ["-fPIC" "-xobjective-c"]
            ]
        ]
        Export: [
            Libs: [%native/out/libpugl.a]
            Includes: [%include/]
            Frameworks: [Cocoa OpenGL CoreVideo]
        ]
    ]
]
