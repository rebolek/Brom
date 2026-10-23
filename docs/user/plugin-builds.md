# Building, installing and packaging plug-ins

The Pokuston plug-ins (Kaprálová, Loos, Mach I) build through one shared driver,
`packages/recoil-brom/plugin-build.r3`. A plug-in declares *what it is* in a
manifest; the driver owns *how* it is built, bundled, installed and packaged.

Run these commands from the Pokuston repository root. Each build selects its
Recoil checkout from `--recoil-root <path>`, then `RECOIL_ROOT`, then the
sibling `../recoil/` directory. A relative `--recoil-root` path is resolved
from the caller's working directory. Recoil supplies compiler/runtime inputs
and shared packages; the manifest, Brom package, cache, and output bundles
remain in Pokuston.

## Building

```sh
r3 -s examples/pokuston/build.r3 kapralova                # all supported formats
r3 -s examples/pokuston/build.r3 kapralova --no-install   # build without installing
r3 -s examples/pokuston/build.r3 kapralova --format clap
r3 -s examples/pokuston/build.r3 kapralova --format vst3
r3 -s examples/pokuston/build.r3 kapralova --format au
r3 -s examples/pokuston/build.r3 kapralova --no-install --dist
```

The instrument argument is `mach`, `loos`, or `kapralova`. Omitting `--format`
builds CLAP and VST3, plus AUv2 on macOS. `--format clap|vst3|au` selects one
format. Explicit AU selection on another platform fails before package fetching
or CMake configuration.

The existing per-format shortcuts select just their named format:

```sh
r3 -s examples/pokuston/kapralova/clap/build.r3 --no-install
r3 -s examples/pokuston/kapralova/vst3/build.r3 --no-install
```

Use the generic entry with `--format au` for an AU-only build. The same Recoil
checkout selection and `--no-install` / `--dist` flags work on every entry.

**Install is the default**, not a flag. `--install` is still accepted so older
habits keep working. This matters more than it sounds: when installing was
opt-in, a rebuild-and-retest cycle would silently test the *previous* binary,
and a bug that had already been fixed would keep reproducing.

### Formats

| Format | Built by | Installs to (macOS) | Installs to (Linux) |
|---|---|---|---|
| CLAP | `clap/build.r3` | `~/Library/Audio/Plug-Ins/CLAP/` | `~/.clap/` |
| VST3 | `vst3/build.r3` | `~/Library/Audio/Plug-Ins/VST3/` | `~/.vst3/` |
| AUv2 | `build.r3 <plugin> --format au` | `~/Library/Audio/Plug-Ins/Components/` | n/a |

VST3 and AUv2 share a CMake build: the VST3 SDK's wrapper produces the
`.component` from the VST3 target, so they share the DSP, parameters and editor.
When both formats are selected, that build runs once. AU-only builds also compile
the internal VST3 dependency, but only the selected `.component` is reported,
installed, or staged for an installer.

An effect whose manifest sets `au-entry` (Punkva, and the Brom probe effect)
builds its AU differently: from its own `au/` CMake project on the generic Brom
AU shell (`packages/recoil-brom/brom/plugin/au/`), which uses Apple's
AudioUnitSDK directly and needs no VST3 SDK. Its VST3 and AU are then two
separate steps, and its `au/` project depends only on `pokuston-audio-unit-sdk`.
An effect that must receive MIDI passes `MIDI_INPUT` to `brom_add_effect_au`
and is published as a music effect (`aumf`), the AU type hosts route MIDI to;
Punkva does (`aumf Pnkv PkDv`), so validate it with `auval -v aumf Pnkv PkDv`.
The instruments keep the wrapper. See `packages/recoil-brom/docs/effect-abi.md`,
"AU".

The AU component embeds a **second** VST3 module, built with the AU program rule (the
pinned wrapper sends factory preset `i` of `N` as `i / N`; see
`brom-vst3-presets.md`). It is created under the package name `<Name>-au-embedded` and
sits beside the real VST3 in the build directory, so the driver skips any bundle with
that suffix when it reports, installs or packages VST3 outputs. The suffix is
reserved. A product defines its VST3 target as a CMake function and instantiates it
twice (`pokuston_<name>_add_vst3`), the AU one with `brom_vst3_program_rules(...
AU_WRAPPER)`, and calls `brom_assert_au_flavor` before `smtg_target_add_auv2`.

When verifying a change to the build, pass `--no-install`: the driver installs by
default, and a test build should not replace the plug-ins you use.

### SDKs

The build refreshes the instrument's shared project and each selected format's
project, then fetches checksum-pinned SDK archives through Recoil's package
manager. The SDK dependencies are scoped to the format:

| Project | SDK packages |
|---|---|
| Instrument root | none |
| `clap/` | `pokuston-clap-sdk` (CLAP 1.2.10) |
| `vst3/` | `pokuston-vst3-sdk` (complete VST3 SDK 3.8.1) |
| `au/` | `pokuston-vst3-sdk` and `pokuston-audio-unit-sdk` (AudioUnitSDK 1.4.0); an `au-entry` effect only the latter |

The driver reads the selected projects' generated `recoil.lock.reb` files to
resolve SDK sources. Lockfiles contain checkout-specific paths and remain
untracked. A CLAP-only build fetches no VST3 or AU SDK; a VST3-only build fetches
no AudioUnitSDK. SDK acquisition failures stop the selected build with a
diagnostic.

For offline builds, provide local copies of the exact pinned archives. The
overrides still undergo the package's SHA-256 check:

```sh
POKUSTON_CLAP_SDK_SOURCE=/archives/clap-1.2.10.tar.gz \
POKUSTON_VST3_SDK_SOURCE=/archives/vst-sdk_3.8.1_build-84_2026-08-11.zip \
POKUSTON_AUDIO_UNIT_SDK_SOURCE=/archives/AudioUnitSDK-1.4.0.tar.gz \
  r3 -s examples/pokuston/build.r3 kapralova --no-install
```

`VST3_SDK_PATH` and `AUDIOUNIT_SDK_PATH` environment variables are no longer
needed; the driver passes the resolved package paths to CMake. AUv2 additionally
requires a user-installed Xcode and macOS SDK selected with `xcode-select`.
Downloading AudioUnitSDK does not supply Apple's platform toolchain. The macOS
VST3/AU build uses CMake's Xcode generator; the generic effect AU build uses
CMake's default generator.

For exact archive URLs, SHA-256 digests, package scope, and offline override
details, see [Pinned plug-in SDK sources](plugin-sdk-sources.md).

## The manifest

One per plug-in, at the plug-in root, shared by every format:

```rebol
[
    name       "Kapralova"                 ; bundle name and CFBundleName; keep it ASCII
    slug       "kapralova"                 ; ASCII build-artifact stem
    id         "com.pokuston.kapralova"    ; must match the descriptor
    bundle-id  "com.pokuston.kapralova.clap"
    version    "0.3.0"                    ; must match the descriptor
    vendor     "Pokuston"
    root       %examples/pokuston/kapralova/
    project    %examples/pokuston/kapralova/clap/   ; holds recoil.project
    projects   [
        clap %examples/pokuston/kapralova/clap/
        vst3 %examples/pokuston/kapralova/vst3/
        au   %examples/pokuston/kapralova/au/
    ]
    descriptor %clap/clap_entry.rcl       ; where identity is declared in Recoil
    core       %core.rcl                  ; omit when the entry carries the engine
    entry      %clap_entry.rcl
    editor     %editor.rcl
    adapter    %kapralova_clap_editor_adapter.c     ; C compiled with the GUI args
    support    none                       ; C compiled without them
    retire     none                       ; a previous bundle name to delete
    formats    [clap vst3 au]
]
```

Only `name`, `id`, `version`, `root` and `descriptor` are required.
An accented visible `name` may declare an ASCII `slug` for intermediate files;
the final bundle, executable and host-visible product retain `name`.

Mach I shows the two variation points. It declares no `core`, because its engine
reaches the binary through `clap_entry.rcl`'s package imports rather than as its
own translation unit; and it declares `retire "Mach"`, because its CLAP bundle
used to be `Mach.clap` while its VST3/AU bundles were already `Mach I` — the old
bundle is deleted on install so a host offers the plug-in once, not twice.

### Identity is checked, not restated

The driver reads `id`, `name`, `version` and `vendor` out of the Recoil
descriptor and **refuses to build** if the manifest disagrees. `Info.plist` is
generated from the manifest rather than written by hand.

This guard exists because the hand-written scripts had already drifted:
Kaprálová's bundle shipped `com.pokuston.kapralova.clap` at version `0.1` while its
descriptor declared `com.pokuston.kapralova` at `0.2.0`. A stale plist produces no
diagnostic in any host — it is simply a plug-in that reports the wrong version
forever. `tests/pokuston.reb` covers the agreement.

## Distributable installers

```sh
r3 -s examples/pokuston/mach/clap/build.r3 --dist
```

Stages every selected bundle the build reported into a payload rooted at
`/Library/Audio/Plug-Ins/<Format>/` and writes
`dist/<Name>-<version>.pkg`.

Signing is opportunistic:

| Environment | Result |
|---|---|
| `DEVELOPER_ID_INSTALLER` set | the package is signed with that identity |
| `NOTARY_KEYCHAIN_PROFILE` set | it is also submitted to `notarytool` and stapled |
| neither | an unsigned package, and a printed notice |

An unsigned installer is a valid installer — recipients bypass Gatekeeper once.
The build never fails for want of a Developer ID.

Store notarization credentials with
`xcrun notarytool store-credentials <profile-name>`.

## Adding a plug-in

1. Write `examples/pokuston/<name>/plugin.reb` with the manifest above.
2. Create `clap/build.r3` (and `vst3/build.r3`) as thin shims — copy an
   existing one and change the two arguments on its last line:

   ```rebol
   run-pokuston-build-script %examples/pokuston/<name>/plugin.reb [clap] pokuston-build-args
   ```

3. The target scripts hold no build logic. If you find yourself adding some,
   it belongs in the driver or as a manifest field — six copies of "trivial"
   logic is how the previous six scripts came to disagree about whether they
   installed at all.

## Verifying a change to the driver

Transpile output is deterministic under the default `--build-seed 1`, so a
refactor that should not change behaviour can be checked exactly:

```sh
cp "Kapralova.clap/Contents/MacOS/Kapralova" /tmp/baseline
# ... make the change ...
r3 -s examples/pokuston/kapralova/clap/build.r3 --no-install
otool -s __TEXT __text /tmp/baseline | tail -n +2 > /tmp/a
otool -s __TEXT __text "Kapralova.clap/Contents/MacOS/Kapralova" | tail -n +2 > /tmp/b
cmp /tmp/a /tmp/b        # must be identical
```

Compare sections, not whole files: `LC_UUID` and the debug-map timestamps change
on every rebuild, so a whole-file `cmp` always reports a difference. Section
comparison caught a real one during this driver's own conversion — link order is
bundle layout, and appending objects in compile order rather than the declared
order produced a different binary.
