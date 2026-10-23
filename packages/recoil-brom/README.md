# Brom

Brom is Pokuston's DSP and plug-in package: the building blocks the
instruments and effects are made of, and the CLAP and VST3/AU shells that turn
an engine descriptor into a plug-in.

## Layout

| Path | Contents |
|---|---|
| `brom/dsp/` | DSP building blocks: oscillators, filters, envelopes, reverb, shared audio blocks (`audio.h`) |
| `brom/plugin/engine.rcl` | `engine!`, the descriptor every product fills in |
| `brom/plugin/effect.rcl` | `brom-effect!`, the optional table that turns an engine into an audio effect |
| `brom/plugin/effect.h` | The same constants (`BROM_EFFECT_*`, sample formats, tail and latency sentinels), the info and parameter records, and the shell-interface prototypes, for C and C++ shells |
| `brom/plugin/preset.rcl`, `values.rcl` | Factory presets and parameter values |
| `brom/plugin/state/` | State format codec and dispatch |
| `brom/plugin/clap/` | The generic CLAP shell (`entry.rcl`) and its C support |
| `brom/plugin/vst3/` | Shared VST3 helpers (`process_timeline.hpp`, `state_stream.hpp`); the generic effect shell (`effect_vst3.cpp`, its editor hook `effect_editor.h`, its CMake helper `brom-effect-vst3.cmake`) and the Recoil trampolines it calls (`effect-shell.rcl`) |
| `brom/plugin/au/` | The generic AUv2 effect shell on Apple's AudioUnitSDK (`effect_au.mm`), its editor hook and Cocoa view (`effect_editor.h`), its CMake helper (`brom-effect-au.cmake`) and Info.plist template; it calls the same trampolines as the VST3 shell |
| `plugin-build.r3` | The shared plug-in build driver |
| `test/probe-effect/` | A minimal f64 effect with a sidechain, used to exercise the CLAP, generic VST3 and generic AU shells' conversion, ports, MIDI, latency and tail paths |
| `test/`, `bench/` | Package tests and benchmarks |

## Documentation

- [`docs/effect-abi.md`](docs/effect-abi.md): the audio effect ABI
  (`brom-effect!`): sample formats, sidechain, latency, tail, threading,
  error paths, the C interface the VST3 and AU shells use, and how each shell
  maps it, with every deviation.

## Known issues

### VST3 state load races the audio thread

The CLAP shell no longer has this race: its `state-load` queues the decoded
state as a Brom preset transaction, installed at the next block boundary on
the audio thread, or at once when the plug-in is inactive (see
[`docs/effect-abi.md`](docs/effect-abi.md), CLAP shell behaviour > Presets and
state). The AU shell hands loaded bytes to its render thread.

The generic VST3 shell's `setState` still calls `brom_effect_state_load`
directly on the calling thread. Nothing stops it from running while the audio
thread is inside `process` on the same core, so the engine's parameters can be
changed underneath a render. Moving it onto the audio thread is Istar #199.
