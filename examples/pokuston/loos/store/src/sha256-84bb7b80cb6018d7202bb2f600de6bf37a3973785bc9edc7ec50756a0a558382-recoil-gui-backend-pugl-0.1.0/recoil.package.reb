Recoil-Package [
    Name: recoil-gui-backend-pugl
    Version: 0.1.0
    Module-Roots: [recoil/gui/backend/pugl]
    Install: [
        Provides: [gui-backend]
        Requires: [pugl-backend]
        Selectors: [
            [Target: native Host-Platform: macOS]
            [Target: native Host-Platform: Linux]
        ]
    ]
    Vendor: [
        Build: [
            [
                name: recoil-gui-backend-pugl
                kind: static
                output: %native/out/librecoil-gui-backend-pugl.a
                sources: [
                    %nanovg/nanovg.c
                    %backend_pugl.c
                ]
                includes: [%nanovg/ %include/]
                compile-flags: ["-fPIC" "-DGL_SILENCE_DEPRECATION" "-Wno-deprecated-declarations"]
            ]
        ]
        Export: [
            Libs: [%native/out/librecoil-gui-backend-pugl.a]
            System-Libs: [m]
        ]
    ]
]
