Rebol [
    Title: "Pokuston plug-in build bootstrap"
    Description: "Select a Recoil checkout before loading its compiler and the product build driver."
]

; `do` changes system/script/path to this bootstrap's directory. Resolve its
; product root before loading recoil.r3, which changes that path again.
pokuston-root: dirize clean-path join system/script/path %../../
pokuston-build-args: copy []
pokuston-cli-recoil-root: none
pokuston-await-recoil-root?: false
foreach pokuston-arg copy any [system/options/args []] [
    either pokuston-await-recoil-root? [
        pokuston-cli-recoil-root: pokuston-arg
        pokuston-await-recoil-root?: false
    ] [
        either pokuston-arg = "--recoil-root" [
            pokuston-await-recoil-root?: true
        ] [
            append pokuston-build-args pokuston-arg
        ]
    ]
]
if pokuston-await-recoil-root? [
    print "ERROR: --recoil-root requires a path."
    quit/return 1
]

pokuston-env-recoil-root: get-env "RECOIL_ROOT"
pokuston-recoil-root-source: case [
    not none? pokuston-cli-recoil-root ["--recoil-root"]
    not none? pokuston-env-recoil-root ["RECOIL_ROOT"]
    true ["sibling ../recoil"]
]
pokuston-recoil-root-value: any [
    pokuston-cli-recoil-root
    pokuston-env-recoil-root
    join pokuston-root %../recoil/
]
if all [string? pokuston-recoil-root-value empty? trim copy pokuston-recoil-root-value] [
    print rejoin ["ERROR: " pokuston-recoil-root-source " is empty."]
    quit/return 1
]
pokuston-recoil-root-file: to-file pokuston-recoil-root-value
pokuston-selected-recoil-root: dirize clean-path either any [
    find/match form pokuston-recoil-root-file "/"
    all [
        system/platform = 'Windows
        (length? form pokuston-recoil-root-file) >= 2
        (form pokuston-recoil-root-file)/2 = #":"
    ]
] [
    pokuston-recoil-root-file
] [
    join system/options/path pokuston-recoil-root-file
]
unless all [
    exists? join pokuston-selected-recoil-root %recoil.r3
    exists? join pokuston-selected-recoil-root %src/compiler.r3
    exists? join pokuston-selected-recoil-root %lib/runtime.rcl
    exists? join pokuston-selected-recoil-root %packages/
] [
    print rejoin [
        "ERROR: " pokuston-recoil-root-source " does not name a Recoil checkout: "
        pokuston-selected-recoil-root
    ]
    quit/return 1
]

system/options/args: copy []
recoil-suppress-auto-run?: true
system/script/path: pokuston-selected-recoil-root
do join pokuston-selected-recoil-root %recoil.r3
system/script/path: recoil-root
do join pokuston-root %packages/recoil-brom/build.r3
