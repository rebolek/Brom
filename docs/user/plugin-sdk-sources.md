# Pinned plug-in SDK sources

Pokuston's shared build driver resolves SDK source packages through each
instrument format project's `recoil.lock.reb`. The root project contains shared
product dependencies; each format project contains only the SDKs that format
needs. Generated lockfiles contain local paths and remain untracked.

## Versions and archives

These package versions are checksum-pinned. Local source overrides use the
same SHA-256 checks as fetched archives.

| Package | Upstream version | Archive | SHA-256 |
|---|---|---|---|
| `pokuston-clap-sdk` | CLAP 1.2.10 | [1.2.10 source archive](https://github.com/free-audio/clap/archive/refs/tags/1.2.10.tar.gz) | `58fdb977c6678859c15d57942378b651399178f49a3a8ecad1813b9f231dc096` |
| `pokuston-vst3-sdk` | VST3 SDK 3.8.1 | [complete 3.8.1 distribution](https://github.com/pongasoft/vst3sdk/releases/download/v3.8.1_build_84/vst-sdk_3.8.1_build-84_2026-08-11.zip) | `64965f1b74e08a6d4087a35af7a716f4dcff5852c66ad7ee13f1c47e79c1ab77` |
| `pokuston-audio-unit-sdk` | AudioUnitSDK 1.4.0 | [AudioUnitSDK 1.4.0 source archive](https://github.com/apple/AudioUnitSDK/archive/refs/tags/AudioUnitSDK-1.4.0.tar.gz) | `a3ac2f94e8df5370df46f8bdf5e34cc07d1329d011bfa61503c8e503309c29e7` |

The complete VST3 3.8.1 distribution is a ZIP and includes the SDK's required
submodule content. GitHub's generated source archive for the SDK tag contains
submodule references without those trees, so it is not a substitute for the
complete release archive. Package composition retains the upstream license and
notice files. CLAP is MIT-licensed; AudioUnitSDK is Apache-2.0 licensed. Consult
the retained VST3 SDK and dependency notices for their applicable terms.

## Format package scope

| Project root | SDK dependencies |
|---|---|
| Instrument root | none |
| `clap/` | `pokuston-clap-sdk` |
| `vst3/` | `pokuston-vst3-sdk` |
| `au/` | `pokuston-vst3-sdk`, `pokuston-audio-unit-sdk` |

Each VST3/AU project is a package root with its own `recoil.package.reb` and a
unique `project.rcl` metadata module. Recoil currently requires package metadata and
at least one module to resolve a project; these marker modules are not imported
by the instrument sources and do not add SDK dependencies.

The lockfile's package store path contains package metadata; Recoil composes the
upstream SDK files separately. The Pokuston driver resolves that composed
source automatically. Users should not set `VST3_SDK_PATH` or
`AUDIOUNIT_SDK_PATH` environment variables.

The AU wrapper builds an internal VST3 target as a dependency. The AU component
contains a copy of that VST3 bundle, but AU-only builds report and stage only
the AU component. The wrapper does not install directly to the user's plug-in
directory; installation and installer staging are controlled by the shared
Pokuston driver.

## Offline archive overrides

Set one or more variables to local copies of the corresponding pinned archives.
The package manager still checks each archive against its SHA-256 digest.

| Variable | Archive |
|---|---|
| `POKUSTON_CLAP_SDK_SOURCE` | CLAP 1.2.10 `.tar.gz` |
| `POKUSTON_VST3_SDK_SOURCE` | VST3 SDK 3.8.1 `.zip` |
| `POKUSTON_AUDIO_UNIT_SDK_SOURCE` | AudioUnitSDK 1.4.0 `.tar.gz` |

For example, set only the CLAP override when building CLAP:

```sh
POKUSTON_CLAP_SDK_SOURCE=/archives/clap-1.2.10.tar.gz \
  r3 -s examples/pokuston/build.r3 kapralova --format clap --no-install
```

AU builds additionally require macOS, Xcode, and Apple's macOS SDK; downloading
AudioUnitSDK does not provide Apple's platform toolchain. See [Building,
installing and packaging plug-ins](plugin-builds.md) for commands and
installation behavior.

## Verification status

On macOS with Xcode 26.2, the default, CLAP-only, VST3-only, and AU-only builds
were verified with `--no-install --dist`; each installer contained only its
selected formats. The final Pokuston RUT run passed 44 tests. Non-macOS build
acceptance and interactive host audio checks were not performed.
