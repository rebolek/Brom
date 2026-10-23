Rebol [
    Title: "Shared Pokuston native package build resolution"
    Description: "Resolves the host-selected Pugl GUI package graph for plug-in builds."
]

resolve-pokuston-package-libraries: function [
    "Return the declared archives for a package, materializing missing recipe outputs when requested."
    metadata [object!] "Loaded package metadata whose exported archives are resolved."
    /materialize "Build missing archive outputs from their package recipes."
    /local libraries library-file recipe
][
    libraries: copy []
    foreach library-file metadata/vendor-export/libs [
        either exists? library-file [
            append libraries library-file
        ][
            recipe: find-package-vendor-recipe-by-output metadata/vendor-build library-file
            unless object? recipe [
                cli-error/context 'R6025 "Package vendor library is unavailable" reduce [
                    'package metadata/name
                    'path library-file
                ]
            ]
            append libraries either materialize [
                build-package-vendor-recipe recipe
            ][
                library-file
            ]
        ]
    ]
    libraries
]

resolve-pokuston-pugl-build: function [
    "Resolve gui/pugl for a host and return its complete ordered native build surface."
    host-platform [word!] "Host platform selector, normally system/platform."
    /materialize "Build missing package archives before returning the surface."
    /local catalog resolution result package-spec metadata surface library-file include-dir lib-dir lib-name framework define-entry link-arg
][
    catalog: discover-std-capability-catalog
    resolution: resolve-std-capability-install/variant catalog 'gui 'native host-platform 'pugl
    result: make object! [
        packages: copy []
        includes: copy []
        archives: copy []
        compile-args: copy []
        link-args: copy []
        system-libs: copy []
        frameworks: copy []
    ]

    foreach package-spec resolution/packages [
        append result/packages package-spec/name
        metadata: load-package-metadata package-spec/root
        surface: metadata/vendor-export
        foreach include-dir surface/includes [
            append-unique-value result/includes include-dir
            append-unique-value result/compile-args ajoin ["-I" include-dir]
        ]
        foreach define-entry surface/defines [
            append-unique-value result/compile-args package-vendor-encode-define-entry define-entry
        ]
        foreach library-file either materialize [
            resolve-pokuston-package-libraries/materialize metadata
        ][
            resolve-pokuston-package-libraries metadata
        ][
            append-unique-value result/archives library-file
            append-unique-value result/link-args library-file
        ]
        foreach lib-dir surface/lib-dirs [
            append-unique-value result/link-args ajoin ["-L" lib-dir]
        ]
        foreach lib-name surface/link-libs [
            append-unique-value result/link-args ajoin ["-l" lib-name]
        ]
        foreach lib-name surface/system-libs [
            append-unique-value result/system-libs lib-name
            append-unique-value result/link-args ajoin ["-l" lib-name]
        ]
        foreach framework surface/frameworks [
            append-unique-value result/frameworks framework
            append result/link-args "-framework"
            append result/link-args form framework
        ]
        foreach link-arg surface/link-args [
            append-unique-value result/link-args link-arg
        ]
    ]
    result
]

cmake-list-from-values: function [
    "Join build-surface values into one CMake list argument."
    values [block!] "Ordered file, word, or string values to encode."
][
    combine/with map-each value values [form value] ";"
]

link-pokuston-clap: function [
    "Link a CLAP binary, using Zig's old-glibc surface for portable Linux hosts when available."
    output [file!] "Destination CLAP shared library."
    objects [block!] "Ordered object files to link."
    package-link-args [block!] "Ordered package archives and platform link arguments."
    /portable-linux "Require the portable Linux linker used for Flatpak-hosted DAWs."
    /local command use-zig? zig-target library-dir env-spec
][
    zig-target: switch system/build/arch [
        x64 ["x86_64-linux-gnu.2.17"]
        arm64 ["aarch64-linux-gnu.2.17"]
    ]
    use-zig?: to logic! all [
        system/platform = 'Linux
        string? zig-target
        compiler-available? "zig"
    ]
    if all [portable-linux system/platform = 'Linux not use-zig?] [
        do make error! "Linux CLAP installation requires Zig to target a Flatpak-compatible glibc ABI"
    ]

    command: copy either use-zig? [
        reduce [zig-effective-argv0 "cc" "-target" zig-target]
    ][
        ["gcc"]
    ]
    repend command ["-shared" "-o" output]
    append command objects
    append command package-link-args
    if use-zig? [
        foreach library-dir [
            %/usr/lib64/
            %/usr/lib/x86_64-linux-gnu/
            %/usr/lib/aarch64-linux-gnu/
        ][
            if exists? library-dir [repend command ["-L" library-dir]]
        ]
    ]
    append command "-lm"

    either use-zig? [
        env-spec: reduce [
            "ZIG_LOCAL_CACHE_DIR" ensure-cache-dir join pokuston-build-cache-root %zig-clap-local/
            "ZIG_GLOBAL_CACHE_DIR" ensure-cache-dir join pokuston-build-cache-root %zig-clap-global/
        ]
        run-shell-command-with-env env-spec command
    ][
        run-shell-command command
    ]
]

; The plug-in build driver. Kept in its own file for readability, loaded here
; so every target that already does this module gets it without another line.
do join pokuston-root %packages/recoil-brom/plugin-build.r3
