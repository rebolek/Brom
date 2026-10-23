# Pokuston Loos VST3 and AUv2 targets

This target reuses the instance-safe Recoil DSP/control core at
`examples/pokuston/loos/core.rcl`; it does not reuse CLAP’s host entry point. The VST3
bridge accepts note-on/note-off events, renders stereo `float` audio, and
limits each call into the current 512-frame Loos buffer. It also presents the
shared custom canvas editor. On macOS, the official VST3 SDK AUv2 wrapper
builds an `.component` from that same VST3 processor and controller, so VST3
and AUv2 share the DSP, parameters, and editor.

The editor accepts an NSView parent on macOS and an X11 embed window ID on
Linux. A VST3 main-thread timer pumps the shared Pugl editor every 33 ms while
it is attached.

The build selects the architecture of the running Rebol binary (`x86_64` or
`arm64`) so it matches Recoil's cached native runtime and GUI artifacts.

## Dependencies

The shared driver fetches checksum-pinned VST3 and AudioUnitSDK archives through
Recoil packages and resolves their paths from the selected format projects'
generated, untracked lockfiles. VST3-only uses the VST3 graph; AU-only uses the AU
graph with both SDKs. VST3 builds on macOS and Linux through CMake. AUv2 is
macOS-only and also requires a user-installed Xcode and macOS SDK. The macOS
build uses the Xcode generator.

## Build

From the repository root:

```sh
r3 -s examples/pokuston/loos/vst3/build.r3 --no-install
r3 -s examples/pokuston/build.r3 loos --format au --no-install
r3 -s examples/pokuston/build.r3 loos --no-install
```

The shortcut selects VST3 only. The generic entry builds CLAP and VST3 by
default, plus AU on macOS; `--format clap|vst3|au` selects one. Omit
`--no-install` to install the selected bundles. AU-only builds compile an
internal VST3 dependency but report/install only the component.

For offline AU builds, supply the exact pinned source archives:

```sh
POKUSTON_VST3_SDK_SOURCE=/archives/vst-sdk_3.8.1_build-84_2026-08-11.zip \
POKUSTON_AUDIO_UNIT_SDK_SOURCE=/archives/AudioUnitSDK-1.4.0.tar.gz \
  r3 -s examples/pokuston/build.r3 loos --format au --no-install
```

See [plug-in builds](../../../../docs/user/plugin-builds.md) for the CLAP
override and package-fetching details.

Artifacts are placed below `cache/plugin-build/`. The exact platform
subdirectory is owned by the VST3 SDK/CMake generator.

Local builds use ad-hoc signing. To create a distribution build, configure
CMake with `-DLOOS_CODE_SIGN_IDENTITY='Apple Development: …'` (and apply your
normal notarization/release-signing process).

Before distributing the AU, replace the demonstration `Poku` manufacturer
OSType in `resource/au-info.plist` with Pokuston's Apple-registered manufacturer code,
and use your own bundle identifier and signing identity.
