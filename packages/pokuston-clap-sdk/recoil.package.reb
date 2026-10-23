Recoil-Package [
    Name: pokuston-clap-sdk
    Version: 1.2.10
    Module-Roots: [pokuston/clap/sdk]
    Upstream-Source: [
        source: [tarball https://github.com/free-audio/clap/archive/refs/tags/1.2.10.tar.gz]
        sha256: "58fdb977c6678859c15d57942378b651399178f49a3a8ecad1813b9f231dc096"
        override-env: POKUSTON_CLAP_SDK_SOURCE
        local-cache-file: %clap-1.2.10.tar.gz
    ]
]
