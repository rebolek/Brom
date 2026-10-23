Rebol [
    Title: "Pokuston plug-in build driver"
    Description: {
        One build sequence for every Pokuston plug-in and format.

        Each target used to carry its own build.r3 -- six scripts, about 1240
        lines, and after normalising the plug-in name Kapralova's and Loos' CLAP
        scripts differed by ten lines, every one of them a comment. They were
        one script copied twice, and the copies had drifted in the ways copies
        do: install happened only under a flag, only for two of the three CLAP
        targets, and for none of the VST3 ones; the bundle identity was written
        a second time into Info.plist and had already gone stale against the
        descriptor; the step numbering printed "Step 6" three times.

        A target now declares WHAT it is -- see `pokuston-plugin-manifest` --
        and this driver owns HOW it is built, installed and packaged.
    }
]

;; ── Optimisation ───────────────────────────────────────────────────────

; A plug-in always builds optimised. `config/optimize` is what `--release`
; sets, and it reaches the runtime and internal PIC objects that
; ensure-runtime-pic-o and ensure-internal-pic-o compile through recoil.r3;
; pokuston-compile-c hardcodes the matching -O2 for the plug-in's own
; translation units. Setting it here also keeps those objects in their own
; target-artifact tree, so an optimised plug-in build and a default -O0 build
; of anything else never reuse each other's objects.
;
; This is not a tuning knob. See pokuston-compile-c for the measurement: the
; default -O0 build cannot render in realtime.
config/optimize: 2

;; ── Identity ───────────────────────────────────────────────────────────

pokuston-descriptor-field: function [
    {Read one `field: (as c-string! "value")` out of a plug-in descriptor.}
    source [string!] "Descriptor source text."
    field [word!] "Field to read, e.g. 'id."
    /local marker value
] [
    marker: ajoin [form field {: (as c-string! "}]
    value: none
    either parse source [thru marker copy value to {"} to end] [
        return value
    ] [
        return none
    ]
]

check-pokuston-plugin-identity: function [
    {Require a manifest's identity to match the plug-in's own descriptor.

     The bundle's Info.plist used to restate the id, name and version that the
     descriptor already declares, with nothing checking the two agreed --
     Kapralova shipped `com.pokuston.kapralova.clap` at version "0.1" against a
     descriptor saying `com.pokuston.kapralova` at "0.2.0". A stale plist is not
     a diagnostic in any host; it is a plug-in that reports the wrong version
     forever. Read the descriptor and refuse to build on a mismatch.}
    manifest [object!] "Plug-in manifest."
    /local descriptor-file source declared expected mismatches
] [
    descriptor-file: join manifest/root manifest/descriptor
    unless exists? descriptor-file [
        print rejoin ["ERROR: plug-in descriptor not found: " descriptor-file]
        quit/return 1
    ]
    source: to-string read descriptor-file
    mismatches: copy []
    foreach field [id name version vendor] [
        declared: pokuston-descriptor-field source field
        expected: get in manifest field
        case [
            none? declared [
                repend mismatches [field "not found in descriptor" expected]
            ]
            declared <> expected [
                repend mismatches [field declared expected]
            ]
        ]
    ]
    unless empty? mismatches [
        print ""
        print "ERROR: plug-in identity disagrees with its descriptor."
        print rejoin ["  descriptor: " descriptor-file]
        print rejoin ["  manifest:   " manifest/manifest-file]
        foreach [field declared expected] mismatches [
            print rejoin ["  " form field ": descriptor says " mold declared ", manifest says " mold expected]
        ]
        print ""
        quit/return 1
    ]
]

;; ── Bundle staging ─────────────────────────────────────────────────────

pokuston-bundle-package-type: function [
    "Four-character CFBundlePackageType for a plug-in format."
    format [word!] "clap, vst3 or au."
] [
    return either format = 'au ["BNDL"] ["BNDL"]
]

write-pokuston-info-plist: function [
    {Write a bundle Info.plist from the manifest, so identity has one source.}
    target [file!] "Info.plist destination."
    manifest [object!] "Plug-in manifest."
    format [word!] "Format the bundle is for."
    /local plist bundle-id
] [
    bundle-id: any [manifest/bundle-id manifest/id]
    plist: copy ""
    repend plist [
        {<?xml version="1.0" encoding="UTF-8"?>} newline
        {<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" }
        {"http://www.apple.com/DTDs/PropertyList-1.0.dtd">} newline
        {<plist version="1.0">} newline
        {<dict>} newline
        {    <key>CFBundleDevelopmentRegion</key><string>en</string>} newline
        {    <key>CFBundleExecutable</key><string>} manifest/name {</string>} newline
        {    <key>CFBundleIdentifier</key><string>} bundle-id {</string>} newline
        {    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>} newline
        {    <key>CFBundleName</key><string>} manifest/name {</string>} newline
        {    <key>CFBundlePackageType</key><string>} pokuston-bundle-package-type format {</string>} newline
        {    <key>CFBundleShortVersionString</key><string>} manifest/version {</string>} newline
        {    <key>CFBundleVersion</key><string>} manifest/version {</string>} newline
        {</dict>} newline
        {</plist>} newline
    ]
    write target plist
]

stage-pokuston-bundle: function [
    {Build a macOS plug-in bundle around a linked binary.}
    bundle-root [file!] "Bundle directory to (re)create."
    binary-source [file!] "Linked shared library to install as the executable."
    manifest [object!] "Plug-in manifest."
    format [word!] "Format the bundle is for."
    /local bundle-macos bundle-binary info-plist legacy-text legacy-root
] [
    bundle-macos: join bundle-root %Contents/MacOS/
    bundle-binary: join bundle-macos to-file manifest/name
    info-plist: join bundle-root %Contents/Info.plist
    ; `rm -rf` both the trailing-slash and bare spellings: a previous build may
    ; have left either, and deleting only one leaves a half-bundle behind.
    legacy-text: copy form bundle-root
    if all [not empty? legacy-text #"/" = last legacy-text] [
        remove back tail legacy-text
    ]
    legacy-root: to-file legacy-text
    run-shell-command compose/deep [rm -rf (bundle-root) (legacy-root)]
    make-dir/deep bundle-macos
    write-pokuston-info-plist info-plist manifest format
    write/binary bundle-binary read/binary binary-source
]

;; ── Install ────────────────────────────────────────────────────────────

pokuston-install-dir: function [
    "User plug-in directory for a format on this platform, or none when unsupported."
    format [word!] "clap, vst3 or au."
] [
    return either system/platform = 'macOS [
        switch format [
            clap [join user-home-dir %Library/Audio/Plug-Ins/CLAP/]
            vst3 [join user-home-dir %Library/Audio/Plug-Ins/VST3/]
            au   [join user-home-dir %Library/Audio/Plug-Ins/Components/]
        ]
    ] [
        switch format [
            clap [join user-home-dir %.clap/]
            vst3 [join user-home-dir %.vst3/]
            au   [none]
        ]
    ]
]

pokuston-bundle-extension: function [
    "Bundle suffix for a format."
    format [word!] "clap, vst3 or au."
] [
    return switch format [
        clap [".clap"]
        vst3 [".vst3"]
        au   [".component"]
    ]
]

install-pokuston-bundle: function [
    {Install a built bundle atomically, replacing whatever is there.

     Staged into a dot-prefixed sibling and renamed into place, so a host that
     scans the directory mid-install never sees a half-written bundle. `retire`
     names an older bundle to delete -- a plug-in renamed across formats would
     otherwise be offered twice, once under each name.}
    source [file!] "Built bundle (a directory on macOS, a file elsewhere)."
    manifest [object!] "Plug-in manifest."
    format [word!] "Format being installed."
    /retire old-name [string!] "Previous bundle name to delete, without extension."
    /local install-dir target stage old-target
] [
    install-dir: pokuston-install-dir format
    unless install-dir [
        print rejoin ["  skipped install: no " form format " location on this platform"]
        return false
    ]
    make-dir/deep install-dir
    target: join install-dir ajoin [manifest/name pokuston-bundle-extension format]
    stage: join install-dir ajoin ["." manifest/name pokuston-bundle-extension format ".stage"]

    run-shell-command compose/deep [rm -rf (stage)]
    run-shell-command compose/deep [cp -R (source) (stage)]
    run-shell-command compose/deep [rm -rf (target)]
    run-shell-command compose/deep [mv (stage) (target)]

    if retire [
        old-target: join install-dir ajoin [old-name pokuston-bundle-extension format]
        if exists? old-target [
            run-shell-command compose/deep [rm -rf (old-target)]
            print rejoin ["  retired " old-target]
        ]
    ]
    print rejoin ["Installed: " target]
    return true
]

;; ── Manifest ───────────────────────────────────────────────────────────

pokuston-plugin-manifest: function [
    {Complete a declared plug-in manifest with defaults.

     A target declares only what distinguishes it. Everything mechanical --
     where its generated C lands, what its bundle is called -- follows from the
     name and root, which is what stopped the six hand-written scripts from
     agreeing on any of it.}
    spec [block!] "Declared fields: name, id, version, vendor, root, entry, ..."
    /local manifest field defaults lower
] [
    defaults: make object! [
        name:       none        ; display + bundle name, e.g. "Mach I"
        slug:       none        ; optional ASCII stem for build artifacts
        id:         none        ; reverse-DNS plug-in id
        bundle-id:  none        ; optional format bundle id when it differs
        version:    none
        vendor:     "Pokuston"
        root:       none        ; example root, e.g. %examples/pokuston/mach/
        project:    none        ; dir holding the format's recoil.project
        projects:   none        ; map of format -> recoil.project directory
        descriptor: none        ; file under root declaring id/name/version
        core:       none        ; %core.rcl, or none when the entry carries it
        entry-carries-core: false ; true when CLAP imports `core` into its TU
        entry:      %clap_entry.rcl
        editor:     none        ; %editor.rcl
        adapter:    none        ; extra C compiled with the GUI package args
        support:    none        ; extra C compiled without them
        formats:    [clap]
        retire:     none        ; previous bundle name, when one was renamed
        manifest-file: "(unknown)"
    ]
    manifest: make defaults []
    foreach [field value] spec [
        unless in manifest field [
            print rejoin ["ERROR: unknown plug-in manifest field: " form field]
            quit/return 1
        ]
        set in manifest field value
    ]
    foreach field [name id version root descriptor] [
        unless get in manifest field [
            print rejoin ["ERROR: plug-in manifest is missing a required field: " form field]
            quit/return 1
        ]
    ]
    ; Absolute the roots once, so no step has to remember to.
    manifest/root: dirize clean-path either absolute-file? manifest/root [
        manifest/root
    ] [join pokuston-root manifest/root]
    manifest/project: either manifest/project [
        dirize clean-path either absolute-file? manifest/project [
            manifest/project
        ] [join pokuston-root manifest/project]
    ] [
        manifest/root
    ]
    manifest/projects: make map! collect [
        foreach [format project] any [manifest/projects reduce ['clap manifest/project]] [
            unless all [find [clap vst3 au] format file? project] [
                do make error! ajoin ["Invalid plug-in project mapping: " mold reduce [format project]]
            ]
            keep format
            keep dirize clean-path either absolute-file? project [project] [join pokuston-root project]
        ]
    ]
    ; Existing CLAP helpers use this alias for source files within that graph.
    manifest/project: any [select manifest/projects 'clap manifest/project]
    return manifest
]

pokuston-selected-formats: function [
    "Validate requested formats in order, excluding default AU on non-macOS hosts."
    requested [block! none!] "Explicit format block, or none for host defaults."
    manifest-formats [block!] "Formats supported by this plug-in or shortcut."
    host-platform [word!] "Host platform, such as macOS, Linux or Windows."
] [
    selected: unique copy any [requested manifest-formats]
    foreach format selected [
        unless all [find [clap vst3 au] format find manifest-formats format] [
            do make error! ajoin ["Unsupported Pokuston plug-in format: " form format]
        ]
        if all [format = 'au host-platform <> 'macOS requested] [
            do make error! "Pokuston AU builds require macOS."
        ]
    ]
    unless requested [
        remove-each format selected [all [format = 'au host-platform <> 'macOS]]
    ]
    if empty? selected [do make error! "No Pokuston plug-in formats selected for this host."]
    return selected
]

pokuston-selected-projects: function [
    "Return the shared project followed by each selected format graph, once each."
    manifest [object!] "Normalized plug-in manifest with absolute project paths."
    formats [block!] "Validated format selection in build order."
] [
    return unique collect [
        keep manifest/root
        foreach format formats [
            project: select manifest/projects format
            unless project [
                do make error! ajoin ["Missing " form format " project for " manifest/name]
            ]
            keep project
        ]
    ]
]

pokuston-resolved-package-root: function [
    "Read an exact package's existing directory from its project's generated lockfile."
    project-root [file!] "Directory containing recoil.lock.reb."
    package-name [word!] "Exact package name to find in Packages."
    /upstream "Materialize the locked package's checksum-pinned upstream source overlay."
] [
    problem: ajoin ["Cannot resolve package " form package-name " in project " form project-root]
    lock-file: join project-root %recoil.lock.reb
    unless exists? lock-file [do make error! ajoin [problem ": missing recoil.lock.reb"]]
    lock-data: try [load lock-file]
    if error? lock-data [do make error! ajoin [problem ": unreadable recoil.lock.reb"]]
    lock-body: all [block? lock-data select lock-data 'Recoil-Lock]
    packages: all [block? lock-body select lock-body 'Packages]
    unless block? packages [do make error! ajoin [problem ": missing Packages entries"]]
    foreach entry packages [
        if all [block? entry (select entry 'package) = package-name] [
            store-path: select entry 'store-path
            unless file? store-path [do make error! ajoin [problem ": missing store-path"]]
            store-path: clean-path either absolute-file? store-path [store-path] [join project-root store-path]
            unless (exists? store-path) = 'dir [
                do make error! ajoin [problem ": missing store directory " store-path]
            ]
            if upstream [
                ; The lock identifies the Recoil overlay, while upstream SDK
                ; files live in Recoil's separately composed source cache.
                package-file: join store-path %recoil.package.reb
                package-body: load-package-data-file package-file 'Recoil-Package
                    'R6025 'R6025 "Package root is missing recoil.package.reb" "Invalid recoil.package.reb file"
                upstream-source: parse-package-upstream-source
                    select package-body 'Upstream-Source store-path package-file package-name
                if object? upstream-source [
                    return dirize prepare-package-upstream-root store-path package-name upstream-source
                ]
            ]
            return dirize store-path
        ]
    ]
    do make error! ajoin [problem ": package absent from lockfile"]
]

pokuston-build-options: function [
    "Parse shared build flags and an optional single format selector."
    args [block!] "Arguments after bootstrap removes --recoil-root."
    shortcut-formats [block! none!] "Shortcut's format set, or none for all manifest formats."
    /local format-name
] [
    requested: none
    no-install?: false
    dist?: false
    unless parse args [any [
        "--install"
        | "--no-install" (no-install?: true)
        | "--dist" (dist?: true)
        | "--format" set format-name string! (
            if requested [do make error! "--format may be supplied only once."]
            requested: reduce [to-word format-name]
        )
    ]] [
        do make error! ajoin ["Invalid Pokuston build arguments: " mold args
            ". Expected [--format clap|vst3|au] [--no-install] [--dist]."]
    ]
    if requested [
        requested: pokuston-selected-formats requested any [shortcut-formats [clap vst3 au]] 'macOS
    ]
    return make object! reduce/no-set [
        quote formats: any [requested shortcut-formats]
        quote explicit-format?: not none? requested
        quote no-install?: no-install?
        quote dist?: dist?
    ]
]

pokuston-cache-file: function [
    "Cache path for one of a plug-in's build artifacts."
    manifest [object!] "Plug-in manifest."
    role [string!] {Artifact role, e.g. "core.o".}
    /local lower
] [
    ; Display names are allowed to carry accents, but Rebol's file port cannot
    ; write some Unicode paths on macOS. Keep generated build artifacts on an
    ; explicit ASCII stem while preserving the display name for the bundle.
    lower: lowercase copy any [manifest/slug manifest/name]
    replace/all lower " " "-"
    return join pokuston-build-cache-root ajoin [lower "-" role]
]

pokuston-build-cache-root: ensure-cache-dir join pokuston-root to-file ajoin [
    "cache/plugin-build/"
    lowercase copy/part binary-to-hex checksum to binary! form recoil-root 'sha256 16
    "/"
]

;; ── Compile helpers ────────────────────────────────────────────────────

pokuston-transpile: function [
    {Transpile one .rcl through package-mode `compile`, so bare module imports
     resolve from the owning project's lockfile.}
    project [file!] "Directory holding recoil.project."
    source [file!] "Source file, relative to the project."
    target [file!] "Generated C destination."
    label [string!] "What is being transpiled, for diagnostics."
    /local output exit-code
] [
    output: copy ""
    exit-code: run-recoil-subprocess/env project reduce [
        "compile" "--lib" "--transpile" source "-o" target
    ] output reduce ["RECOIL_ROOT" recoil-root]
    if exit-code <> 0 [
        print rejoin ["ERROR: " label " transpile (package mode) failed."]
        print output
        quit/return 1
    ]
    print rejoin ["  Wrote " target]
]

pokuston-compile-c: function [
    {Compile one C file to an optimised -fPIC object, or exit with a named
     diagnostic.

     -O2 is not optional and is not a per-file choice. A plug-in renders on a
     hard realtime deadline, and the default -O0 build misses it: the Mach
     engine benchmark sustains 15.5x realtime per voice at -O2 and 2.14x at
     -O0, so one -O0 voice costs 47% of a core and two saturate it. The
     audible result is crackling, dropouts and a stuttering transport, worse
     the longer a patch's release holds voices overlapping.

     This flag used to be a /optimize refinement passed only when compiling
     core.rcl. A plug-in whose engine reaches the binary through its entry
     translation unit rather than a core.rcl of its own -- Mach -- therefore
     shipped its whole DSP at -O0.}
    source [file!] "C source."
    target [file!] "Object destination."
    includes [block!] "Extra -I arguments, already ordered."
    label [string!] "What is being compiled, for diagnostics."
    /local exit-code command
] [
    command: compose/deep [gcc -g -O2]
    repend command [toolchain-command-c-std-flag '-c '-fPIC]
    append command includes
    repend command [source '-o target]
    exit-code: run-shell-command command
    if exit-code/exit-code != 0 [
        print rejoin ["ERROR: " label " compilation failed."]
        quit/return 1
    ]
]

;; ── The CLAP build ─────────────────────────────────────────────────────

refresh-pokuston-stores: function [
    {Re-vendor every package store this plug-in builds through.

     The stores are content-addressed copies. Editing a source they cover
     without re-vendoring does not fail loudly -- it quietly builds the STALE
     copy, which has cost this project whole sessions twice: a brom state-buffer
     fix looked unfixed through two builds and two validations, and a gui/vid
     fix rebuilt byte-identical while the bug was already repaired. Only Mach's
     script did this; now every plug-in does.}
    manifest [object!] "Plug-in manifest."
    formats [block!] "Validated formats whose package graphs must be refreshed."
    /local output exit-code projects
] [
    projects: pokuston-selected-projects manifest formats
    foreach project projects [
        output: copy ""
        exit-code: run-recoil-subprocess/env project ["update"] output reduce ["RECOIL_ROOT" recoil-root]
        if exit-code <> 0 [
            print rejoin ["ERROR: package update failed in " project]
            print output
            quit/return 1
        ]
    ]
]

build-pokuston-clap: function [
    {Build, bundle and install one plug-in's CLAP target.}
    manifest [object!] "Completed plug-in manifest."
    gui-build [object!] "Resolved GUI package graph."
    /no-install "Build without installing into the user's plug-in directory."
    /local clap-headers clap-header link-inputs exit-code
           core-c core-o entry-c entry-o editor-c editor-o
           adapter-o support-o helpers-o helpers-c linked bundle
           lib-include base-includes
] [
    clap-headers: join pokuston-resolved-package-root/upstream
        select manifest/projects 'clap 'pokuston-clap-sdk %include/
    clap-header: join clap-headers %clap/clap.h
    unless exists? clap-header [
        do make error! ajoin ["Resolved pokuston-clap-sdk in project "
            select manifest/projects 'clap " lacks include/clap/clap.h: " clap-header]
    ]

    lib-include: join recoil-root %lib/
    helpers-c: join pokuston-root %packages/recoil-brom/brom/plugin/clap/clap_support.c
    linked: pokuston-cache-file manifest "clap-binary"
    link-inputs: copy []

    print "Step 2: Checking plug-in identity against its descriptor..."
    check-pokuston-plugin-identity manifest

    print "Step 3: Compiling runtime and internal with -fPIC..."
    ensure-runtime-pic-o
    ensure-internal-pic-o
    ensure-tls-backend-stub-pic-o

    ; A plug-in whose DSP lives in its own core.rcl compiles it as its own
    ; translation unit. One whose engine is imported by the entry (Mach)
    ; declares no core and gets it through the entry's package graph instead.
    ; Both routes are optimised: pokuston-compile-c applies -O2 to every unit.
    either all [manifest/core not manifest/entry-carries-core] [
        print "Step 4: Transpiling and compiling the core..."
        core-c: pokuston-cache-file manifest "core.c"
        core-o: pokuston-cache-file manifest "core.o"
        pokuston-transpile manifest/root manifest/core core-c "core"
        pokuston-compile-c core-c core-o reduce [
            '-I recoil-root '-I lib-include '-I pokuston-root
        ] "core"
    ] [
        print "Step 4: No separate CLAP core -- the entry carries the engine."
    ]

    print "Step 5: Transpiling and compiling the CLAP entry..."
    entry-c: pokuston-cache-file manifest "clap-entry.c"
    entry-o: pokuston-cache-file manifest "clap-entry.o"
    pokuston-transpile manifest/project manifest/entry entry-c "CLAP entry"
    pokuston-compile-c entry-c entry-o compose [
        -I (clap-headers) -I (recoil-root) -I (lib-include) -I (pokuston-root)
        (gui-build/compile-args)
    ] "CLAP entry"

    if manifest/editor [
        print "Step 6: Transpiling and compiling the editor..."
        editor-c: pokuston-cache-file manifest "editor.c"
        editor-o: pokuston-cache-file manifest "editor.o"
        pokuston-transpile manifest/root manifest/editor editor-c "editor"
        ; The editor's `foreign <name.h>` emits an angle-bracket include, so
        ; the example root has to be on the search path.
        pokuston-compile-c editor-c editor-o compose [
            -I (recoil-root) -I (lib-include) -I (pokuston-root) -I (manifest/root)
            (gui-build/compile-args)
        ] "editor"
    ]

    print "Step 7: Compiling shared CLAP support..."
    helpers-o: pokuston-cache-file manifest "clap-support.o"
    pokuston-compile-c helpers-c helpers-o compose [
        -I (clap-headers) -I (recoil-root) -I (lib-include) -I (pokuston-root)
    ] "shared CLAP support"

    if manifest/adapter [
        adapter-o: pokuston-cache-file manifest "clap-editor-adapter.o"
        pokuston-compile-c (join manifest/project manifest/adapter) adapter-o compose [
            -I (recoil-root) -I (pokuston-root) (gui-build/compile-args)
        ] "editor adapter"
    ]
    if manifest/support [
        support-o: pokuston-cache-file manifest "support.o"
        pokuston-compile-c (join manifest/project manifest/support) support-o reduce [
            '-I recoil-root '-I pokuston-root
        ] "plug-in support"
    ]

    ; Link order is layout, so it is fixed here rather than left to fall out of
    ; the order things happen to be compiled in. This sequence reproduces both
    ; hand-written orders exactly -- Kapralova's core/entry/helpers/editor/adapter
    ; and Mach's entry/helpers/support/editor -- because each plug-in simply
    ; omits the slots it has no source for.
    foreach object reduce [core-o entry-o helpers-o support-o editor-o adapter-o] [
        if object [append link-inputs object]
    ]
    ; Named through target-artifact-file rather than assumed to sit in cache/:
    ; these are target-keyed (cache/<host>/targets/<target>/). The old scripts
    ; each declared `join cache-dir %runtime-pic.o` and got away with it only
    ; because ensure-runtime-pic-o is a `func` whose global `runtime-o`
    ; assignment leaked out and overwrote the declaration.
    append link-inputs reduce [
        target-artifact-file %runtime-pic.o
        target-artifact-file %internal-pic.o
        target-artifact-file %tls-backend-stubs-pic.o
    ]

    print rejoin ["Step 8: Linking " manifest/name ".clap..."]
    exit-code: either all [not no-install system/platform = 'Linux] [
        link-pokuston-clap/portable-linux linked link-inputs gui-build/link-args
    ] [
        link-pokuston-clap linked link-inputs gui-build/link-args
    ]
    if exit-code/exit-code != 0 [
        print "ERROR: Linking failed."
        quit/return 1
    ]

    print "Step 9: Staging the bundle..."
    bundle: join pokuston-root ajoin [manifest/name ".clap" either system/platform = 'macOS ["/"] [""]]
    either system/platform = 'macOS [
        stage-pokuston-bundle bundle linked manifest 'clap
    ] [
        write/binary bundle read/binary linked
        run-shell-command compose/deep [chmod 755 (bundle)]
    ]
    print rejoin ["  Built " bundle]

    either no-install [
        print "Step 10: Install skipped (--no-install)."
    ] [
        print "Step 10: Installing..."
        either manifest/retire [
            install-pokuston-bundle/retire bundle manifest 'clap manifest/retire
        ] [
            install-pokuston-bundle bundle manifest 'clap
        ]
    ]
    return reduce ['clap bundle]
]


;; ── The VST3 / AUv2 build ──────────────────────────────────────────────

find-pokuston-bundles: function [
    {Every outer bundle with `suffix` under `root`.

     CMake's output layout varies by generator -- Xcode nests under a
     configuration directory, Ninja does not -- so the bundles are found rather
     than predicted.}
    root [file!] "Directory to search."
    suffix [string!] {Bundle suffix, e.g. ".vst3".}
    /local found result line pattern
] [
    result: copy []
    unless exists? root [return result]
    found: copy ""
    pattern: ajoin ["*" suffix]
    ; AU embeds a VST3 dependency. Stop at bundle boundaries so that copy is
    ; never reported or staged as another standalone VST3 output.
    call/wait/output reduce [
        "find" (form root) "-maxdepth" "6"
        "(" "-name" "*.vst3" "-o" "-name" "*.component" "-o" "-name" pattern ")"
        "-prune" "-name" pattern "-print"
    ] found
    foreach line split found newline [
        line: trim copy line
        unless empty? line [append result to-file line]
    ]
    return result
]

build-pokuston-vst3: function [
    {Build a plug-in's VST3 target, and on macOS the AUv2 component the VST3
     SDK's wrapper produces beside it.

     Both come out of one CMake project, so this is a single step that yields
     up to two bundles rather than two builds.}
    manifest [object!] "Completed plug-in manifest."
    gui-build [object!] "Resolved GUI package graph."
    selected-formats [block!] "Validated VST3/AU formats requested by the shared driver."
    /required "Retained for existing callers; selected SDKs are always required."
    /no-install "Build without installing."
    /local sdk-project vst3-sdk au-sdk core-c editor-c source-dir build-dir configure
           exit-code macos-arch bundles bundle output-format suffix built
] [
    ; AU's graph also contains the VST3 SDK for its internal wrapper dependency.
    sdk-project: select manifest/projects either find selected-formats 'vst3 ['vst3] ['au]
    vst3-sdk: join pokuston-resolved-package-root/upstream
        sdk-project 'pokuston-vst3-sdk %vst3sdk/
    unless exists? join vst3-sdk %CMakeLists.txt [
        do make error! ajoin ["Resolved pokuston-vst3-sdk in project " sdk-project
            " lacks vst3sdk/CMakeLists.txt: " vst3-sdk]
    ]
    au-sdk: none
    if find selected-formats 'au [
        au-sdk: pokuston-resolved-package-root/upstream
            select manifest/projects 'au 'pokuston-audio-unit-sdk
        unless exists? join au-sdk %include/AudioUnitSDK/AudioUnitSDK.h [
            do make error! ajoin ["Resolved pokuston-audio-unit-sdk in project "
                select manifest/projects 'au " lacks AudioUnitSDK headers: " au-sdk]
        ]
    ]

    source-dir: join manifest/root %vst3/
    build-dir: pokuston-cache-file manifest "vst3-build/"
    core-c: pokuston-cache-file manifest "vst3-core.c"
    editor-c: pokuston-cache-file manifest "vst3-editor.c"

    print "Step 2: Checking plug-in identity against its descriptor..."
    check-pokuston-plugin-identity manifest

    if manifest/core [
        print "Step 3: Transpiling the shared synth core..."
        pokuston-transpile manifest/root manifest/core core-c "core"
    ]
    if manifest/editor [
        print "Step 4: Transpiling the editor..."
        pokuston-transpile manifest/root manifest/editor editor-c "editor"
    ]

    print "Step 5: Building PIC Recoil runtime objects..."
    ensure-runtime-pic-o
    ensure-internal-pic-o
    ensure-tls-backend-stub-pic-o

    configure: reduce [
        "cmake"
        "-S" source-dir
        "-B" build-dir
        rejoin ["-DVST3_SDK_PATH=" vst3-sdk]
        rejoin ["-DRECOIL_ROOT=" recoil-root]
        rejoin ["-DPOKUSTON_ROOT=" pokuston-root]
        rejoin ["-DRECOIL_PLUGIN_C=" core-c]
        rejoin ["-DRECOIL_EDITOR_C=" editor-c]
        rejoin ["-DRECOIL_RUNTIME_OBJECT=" target-artifact-file %runtime-pic.o]
        rejoin ["-DRECOIL_INTERNAL_OBJECT=" target-artifact-file %internal-pic.o]
        rejoin ["-DRECOIL_TLS_BACKEND_STUB_OBJECT=" target-artifact-file %tls-backend-stubs-pic.o]
        rejoin ["-DRECOIL_GUI_LIBRARIES=" cmake-list-from-values gui-build/archives]
        rejoin ["-DRECOIL_GUI_INCLUDE_DIRS=" cmake-list-from-values gui-build/includes]
        rejoin ["-DRECOIL_GUI_SYSTEM_LIBRARIES=" cmake-list-from-values gui-build/system-libs]
        rejoin ["-DRECOIL_GUI_FRAMEWORKS=" cmake-list-from-values gui-build/frameworks]
    ]
    if system/platform = 'macOS [
        ; Package-native objects are compiled for the Rebol host ABI. Tell Xcode
        ; to build that architecture rather than its universal default; mixing
        ; an x64 Rebol cache with an arm64 link is not valid.
        macos-arch: switch system/build/arch [
            x64 ["x86_64"]
            arm64 ["arm64"]
        ]
        unless string? macos-arch [
            print rejoin ["ERROR: Unsupported macOS Rebol architecture: " system/build/arch]
            quit/return 1
        ]
        repend configure ["-G" "Xcode" rejoin ["-DCMAKE_OSX_ARCHITECTURES=" macos-arch]]
    ]
    either au-sdk [
        append configure ajoin ["-DAUDIOUNIT_SDK_PATH=" au-sdk]
        append configure "-DSMTG_ENABLE_AUV2_BUILDS=ON"
    ] [
        ; Reusing an AU build directory must not preserve AU's SDK or targets.
        append configure ["-UAUDIOUNIT_SDK_PATH" "-USMTG_AUDIOUNIT_SDK_PATH"
            "-DSMTG_ENABLE_AUV2_BUILDS=OFF"]
    ]

    print "Step 6: Configuring VST3/AUv2 targets..."
    exit-code: run-shell-command configure
    if exit-code/exit-code <> 0 [
        print "ERROR: CMake configuration failed."
        quit/return 1
    ]

    print "Step 7: Building plug-in bundles..."
    exit-code: run-shell-command reduce ["cmake" "--build" build-dir "--config" "Release"]
    if exit-code/exit-code <> 0 [
        print "ERROR: Plug-in build failed."
        quit/return 1
    ]

    built: copy []
    print either no-install ["Step 8: Install skipped (--no-install)."] ["Step 8: Installing..."]
    foreach [output-format suffix] [vst3 ".vst3" au ".component"] [
        if find selected-formats output-format [
            bundles: find-pokuston-bundles build-dir suffix
            if all [empty? bundles output-format = 'au] [
                print "  no requested AUv2 component was produced by CMake."
            ]
            foreach bundle bundles [
                repend built [output-format bundle]
                either no-install [
                    print rejoin ["  Built " bundle]
                ] [
                    either manifest/retire [
                        install-pokuston-bundle/retire bundle manifest output-format manifest/retire
                    ] [
                        install-pokuston-bundle bundle manifest output-format
                    ]
                ]
            ]
        ]
    ]
    return built
]


run-pokuston-build-script: function [
    {Everything a per-target build.r3 does after loading this module.

     Kept here so the six target scripts hold no logic at all: six copies of
     even trivial argument handling is how the previous six drifted into
     disagreeing about whether they installed.}
    manifest-file [file!] "Plug-in manifest, relative to the repo root."
    formats [block! none!] "Shortcut formats, or none for all manifest formats."
    args [block!] "Raw system/options/args the script captured."
    /local no-install? dist? options manifest selected
] [
    ; A nested `do` restores the per-format script path on return. Compiler
    ; backends loaded by recoil.r3 still resolve their files from Recoil.
    system/script/path: recoil-root
    options: pokuston-build-options args formats
    no-install?: not none? find args "--no-install"
    dist?: not none? find args "--dist"
    selected: options/formats
    if all [formats not options/explicit-format?] [
        ; An omitted selector uses the shortcut's declared formats for this host.
        selected: pokuston-selected-formats none formats system/platform
    ]
    manifest: load join pokuston-root manifest-file
    ; The format is named explicitly here, so a missing SDK is an error rather
    ; than a skip: running a target's script IS the request for that format.
    case [
        all [no-install? dist?] [build-pokuston-plugin/formats/no-install/dist manifest selected]
        no-install?             [build-pokuston-plugin/formats/no-install manifest selected]
        dist?                   [build-pokuston-plugin/formats/dist manifest selected]
        true                    [build-pokuston-plugin/formats manifest selected]
    ]
]


;; ── Distributable installer ────────────────────────────────────────────

pokuston-system-install-location: function [
    "System plug-in directory a format installs to inside a .pkg payload."
    format [word!] "clap, vst3 or au."
] [
    return switch format [
        clap [%Library/Audio/Plug-Ins/CLAP/]
        vst3 [%Library/Audio/Plug-Ins/VST3/]
        au   [%Library/Audio/Plug-Ins/Components/]
    ]
]

package-pokuston-plugin: function [
    {Build a distributable macOS installer from the bundles a build produced.

     Signed and notarized when the credentials are in the environment, and
     unsigned with a printed notice when they are not -- an installer you can
     hand to a tester today is worth more than a build that refuses for want of
     a Developer ID.}
    manifest [object!] "Completed plug-in manifest."
    built [block!] "Pairs of [format bundle] from the format builders."
    /local stage-root payload dist-dir pkg identity profile exit-code
           format bundle destination
] [
    unless system/platform = 'macOS [
        print "  --dist builds a macOS .pkg; skipped on this platform."
        return none
    ]
    if empty? built [
        print "  --dist: nothing was built to package."
        return none
    ]

    stage-root: pokuston-cache-file manifest "dist-root/"
    dist-dir: join pokuston-root %dist/
    run-shell-command compose/deep [rm -rf (stage-root)]
    make-dir/deep dist-dir

    foreach [format bundle] built [
        destination: join stage-root pokuston-system-install-location format
        make-dir/deep destination
        run-shell-command compose/deep [
            cp -R (bundle) (join destination ajoin [manifest/name pokuston-bundle-extension format])
        ]
        print rejoin ["  payload: " manifest/name pokuston-bundle-extension format
                      " -> /" pokuston-system-install-location format]
    ]

    pkg: join dist-dir ajoin [manifest/name "-" manifest/version ".pkg"]
    payload: reduce [
        "pkgbuild"
        "--root" stage-root
        "--identifier" ajoin [manifest/id ".pkg"]
        "--version" manifest/version
        "--install-location" "/"
    ]
    identity: get-env "DEVELOPER_ID_INSTALLER"
    either all [string? identity not empty? trim copy identity] [
        repend payload ["--sign" identity]
        print rejoin ["  signing with " identity]
    ] [
        print "  no DEVELOPER_ID_INSTALLER in the environment; building unsigned."
    ]
    append payload pkg

    exit-code: run-shell-command payload
    if exit-code/exit-code <> 0 [
        print "ERROR: pkgbuild failed."
        quit/return 1
    ]

    ; Notarization needs an Apple ID credential stored as a keychain profile
    ; (`xcrun notarytool store-credentials`). Without one the package is still
    ; a valid installer -- it just prompts Gatekeeper on another machine.
    profile: get-env "NOTARY_KEYCHAIN_PROFILE"
    either all [string? profile not empty? trim copy profile] [
        print "  submitting for notarization..."
        exit-code: run-shell-command reduce [
            "xcrun" "notarytool" "submit" pkg
            "--keychain-profile" profile "--wait"
        ]
        either exit-code/exit-code = 0 [
            run-shell-command reduce ["xcrun" "stapler" "staple" pkg]
            print "  notarized and stapled."
        ] [
            print "  WARNING: notarization failed; the unsigned-notarization package is still at the path below."
        ]
    ] [
        unless all [string? identity not empty? trim copy identity] [
            print "  not notarized (recipients will need to bypass Gatekeeper once)."
        ]
    ]

    print rejoin ["Wrote: " pkg]
    return pkg
]

;; ── Entry point ────────────────────────────────────────────────────────

build-pokuston-plugin: function [
    {Build every requested format for one plug-in.

     Install is the default end of a build, not a flag. A build that leaves the
     previous binary in the plug-in directory is how a fixed bug gets
     "reproduced": the CLAP scripts installed only under --install, the VST3
     ones never, and a rebuild-and-retest cycle silently tested the old build.}
    spec [block!] "Declared manifest fields."
    /formats wanted [block! none!] "Formats to build, or none for host manifest defaults."
    /no-install "Build without installing."
    /dist "Also produce a distributable installer."
    /local manifest gui-build selected-formats build-formats built produced
] [
    manifest: pokuston-plugin-manifest spec
    build-formats: pokuston-selected-formats wanted manifest/formats system/platform

    print "Step 1: Refreshing selected vendored package stores..."
    refresh-pokuston-stores manifest build-formats
    selected-formats: copy build-formats

    ; Archive order and every system link input come from package metadata.
    gui-build: resolve-pokuston-pugl-build/materialize system/platform

    ; VST3 and AUv2 come out of one CMake project -- the SDK's wrapper builds the
    ; component beside the VST3 target -- so a manifest naming both must not run
    ; that build twice. Collapse them into the single vst3 step.
    if all [find build-formats 'vst3 find build-formats 'au] [
        build-formats: remove-each format copy build-formats [format = 'au]
    ]

    built: copy []
    foreach format build-formats [
        print ""
        print rejoin ["── " manifest/name " (" form format ") ──"]
        produced: switch/default format [
            clap [
                either no-install [
                    build-pokuston-clap/no-install manifest gui-build
                ] [
                    build-pokuston-clap manifest gui-build
                ]
            ]
            vst3 au [
                case [
                    all [wanted no-install] [build-pokuston-vst3/required/no-install manifest gui-build selected-formats]
                    wanted                  [build-pokuston-vst3/required manifest gui-build selected-formats]
                    no-install              [build-pokuston-vst3/no-install manifest gui-build selected-formats]
                    true                    [build-pokuston-vst3 manifest gui-build selected-formats]
                ]
            ]
        ] [
            print rejoin ["  skipped: " form format " is not a format this driver builds"]
            none
        ]
        if block? produced [append built produced]
    ]

    if dist [
        print ""
        print rejoin ["── " manifest/name " (installer) ──"]
        package-pokuston-plugin manifest built
    ]

    print ""
    print rejoin [manifest/name " " manifest/version " done."]
]
