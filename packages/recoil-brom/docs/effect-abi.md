# Brom audio effect ABI

> **Status: revision 2 is implemented.** It was agreed on 2026-10-15 and
> revised after review on 2026-10-17. `brom/plugin/effect.rcl`,
> `brom/plugin/effect.h`, `brom/dsp/audio.h` / `audio.rcl` and the generic CLAP
> shell implement it, and Punkva runs on it. Roadmap 2 is implemented: the
> shell-interface trampolines (`brom/plugin/vst3/effect-shell.rcl`), the
> generic VST3 shell (`brom/plugin/vst3/effect_vst3.cpp`) with its editor hook
> (`brom/plugin/vst3/effect_editor.h`), and the generic AUv2 shell
> (`brom/plugin/au/effect_au.mm`) with its own editor hook
> (`brom/plugin/au/effect_editor.h`). The probe effect and Punkva build their
> VST3 and AU on them; Punkva's per-product `punkva_vst3.cpp` and its AU through
> the VST3 SDK's wrapper are gone. The AU shell has been driven only by
> in-process fixtures, never by `auval` or a DAW (see [AU](#au)). Roadmap 3 is
> implemented too: Macocha (`examples/pokuston/macocha/`), an f64 effect with a
> variable tail, runs on the table in all three formats with its editor.

An `engine!` descriptor is an instrument by default. Setting its `effect-api`
field to a `brom-effect!` table makes it an audio effect: the plug-in shells
advertise audio input instead of a note port and drive the table's callbacks
instead of the synth `process-fn` / `output-fn` path.

`engine!` stays append-only: the effect revision adds nothing to it beyond the
single `effect-api: [c-pointer!]` pointer. Identity, lifecycle
(`create-fn`, `destroy-fn`, `activate-fn`, `reset-fn`), parameters and state
remain on `engine!` and mean the same for effects as for instruments.

## The table

```rebol
brom-effect!: make c-struct! [
    magic:              [u32!]  ; BROM-EFFECT-MAGIC, 0x42524658 ("BRFX")
    revision:           [u32!]  ; BROM-EFFECT-REVISION, currently 2
    category:           [c-string!]
    input-layouts:      [u32!]  ; bit 0 mono -> stereo, bit 1 stereo -> stereo
    sidechain-channels: [u32!]  ; 0 = no sidechain port, 1 mono, 2 stereo
    sample-format:      [u32!]  ; BROM-SAMPLE-F32 | BROM-SAMPLE-F64
    bypass-param-id:    [u32!]  ; a parameter id, or BROM-NO-BYPASS
    flags:              [u32!]  ; bit 0 BROM-EFFECT-VARIABLE-TAIL; other bits 0

    ; Exactly one of these is set: the one matching sample-format.
    effect-process-fn: [fn-ptr! [
        :core [c-pointer!]
        :input [brom-audio-f32!] :sidechain [brom-audio-f32!] :output [brom-audio-f32!]
        channels [i32!] sidechain-channels [i32!] frames [i32!]
        return: [logic!]
    ]]
    effect-process-f64-fn: [fn-ptr! [
        :core [c-pointer!]
        :input [brom-audio-f64!] :sidechain [brom-audio-f64!] :output [brom-audio-f64!]
        channels [i32!] sidechain-channels [i32!] frames [i32!]
        return: [logic!]
    ]]

    effect-midi-fn:            [fn-ptr! [:core [c-pointer!] status [u32!] data1 [u32!] data2 [u32!] return: [logic!]]]
    effect-program-fn:         [fn-ptr! [:core [c-pointer!] index [u32!] return: [logic!]]]
    effect-current-program-fn: [fn-ptr! [:core [c-pointer!] return: [i32!]]]
    effect-latency-fn:         [fn-ptr! [:core [c-pointer!] return: [u32!]]]
    effect-tail-fn:            [fn-ptr! [:core [c-pointer!] return: [u32!]]]
    effect-deactivate-fn:      [fn-ptr! [:core [c-pointer!] return: [none!]]]
]
```

Every callback is required. An effect that ignores MIDI or programs returns
`false`; one with no latency returns `0`.

`flags` bit 0, `BROM-EFFECT-VARIABLE-TAIL`, declares that the tail may change
while the plug-in is active. CLAP does not need it (it has `tail.changed`);
the VST3 shell does, because VST3 has no tail notification (see
[VST3 and AU](#vst3-and-au)). Punkva leaves it clear; Macocha sets it.

### Revisions and layout

`magic` and `revision` come first so a shell can read them before anything
else. A shell rejects a table whose `magic` is not `BROM-EFFECT-MAGIC`, and
accepts revisions from 2 up to the newest it knows, rejecting a higher one or
`0`. After `revision`, the table is append-only: a later revision adds fields
at the end and never moves or retypes an existing one.

What these words protect is narrow, and worth saying plainly. Today every
shell and effect is compiled into the same plug-in binary from the same
`brom-effect!` definition, so a shell cannot meet a table of another revision.
`revision` exists only for a future consumer built separately from its
effect: an effect loaded at run time, or a prebuilt shell. Revision 2 can
still reorder the fields of revision 1, which had no `revision` field, for
the same reason.

`magic` covers the one case `revision` cannot. Revision 1 begins with the
`category` pointer, so reading a revision-1 table, or any stray pointer, as
revision 2 sees the low half of that pointer as `revision` on a little-endian
64-bit target, and the whole pointer on a 32-bit one. That word can be `2`.
A 32-bit magic makes such a match vanishingly unlikely. A table-size word was
considered instead and not chosen: the revision already fixes the layout, so
a size would only repeat it.

## Sample formats

| Constant | Value | Block type | Process slot |
|---|---|---|---|
| (invalid) | 0 | | |
| `BROM-SAMPLE-F32` | 1 | `brom-audio-f32!` | `effect-process-fn` |
| `BROM-SAMPLE-F64` | 2 | `brom-audio-f64!` | `effect-process-f64-fn` |
| reserved: Q15 / int16 | 3 | | |
| reserved: Q31 / int32 | 4 | | |

An effect processes in one format. The shells convert whatever the host
supplies (CLAP `data32`/`data64`, VST3 `kSample32`/`kSample64`) to and from it.
Desktop hosts only ever provide 32- or 64-bit float, so f32 and f64 cover every
current shell. The integer formats are reserved for a future embedded
fixed-point shell (I2S codecs, Cortex-M Q15 SIMD).

**Adding a format** appends, never reorders:
1. a `BROM-SAMPLE-*` constant (the next reserved value),
2. a `brom-audio-*!` block in `brom/dsp/audio.h` and its Recoil mirror in
   `brom/dsp/audio.rcl`, each with its static asserts,
3. a typed process slot at the **end** of `brom-effect!`, with a revision bump
   and its own pair-width static assert,
4. a converter in each shell that supports it,
5. a trampoline in `brom/plugin/effect.h` (see [Shell interface](#shell-interface)).

A shell built before a format existed rejects the effect in
`brom-effect-valid?`, because the `revision` or the `sample-format` is unknown
to it. It never misreads samples.

A format is the type of the audio crossing the shell boundary, not the
effect's internal precision. Punkva quantizes its feedback delay lines to 12
bits internally while processing f32 at the boundary. That emulation belongs
in the DSP, not in this table.

### Audio blocks

`brom/dsp/audio.h` and `brom/dsp/audio.rcl` define fixed-layout planar blocks
of `BROM_AUDIO_CAPACITY` (512) frames:

| Type | C type | Size | `left` | `right` |
|---|---|---|---|---|
| `brom-audio-f32!` | `brom_audio_f32` | 4096 | 0 | 2048 |
| `brom-audio-f64!` | `brom_audio_f64` | 8192 | 0 | 4096 |

Shells call the process slot with at most 512 frames, splitting larger host
blocks and splitting again at event times. The `:sidechain` block is always a
valid block, never null.

Only the first `frames` samples of each plane are defined for a call. Where
this document says the shell zeroes a plane (a mono main input's right plane,
a missing or disconnected sidechain plane), it zeroes those `frames` samples,
not all 512, as `recoil_clap_read_effect_audio` does today. Samples past
`frames` hold whatever an earlier chunk left, and an effect must not read
them.

## Channels and sidechain

- `channels` is the main input's channel count (1 or 2) for the selected
  `input-layouts` configuration. Output is always stereo.
- With a mono main input the shell passes `channels = 1` and zeroes the right
  plane of `:input`. It never duplicates left into right; the effect decides
  how to spread a mono input.
- A main input port with more channels than the layout is read for its first
  `channels` planes; one with fewer is an error (see
  [Error paths](#error-paths)).
- `sidechain-channels` in the table is the width of the sidechain port the
  effect declares. The argument of the same name is the number of sidechain
  planes the shell read for this block: the host's `channel_count` clamped to
  the declared width, so a host that supplies more planes than declared has
  the extra ones ignored. Supplied planes are copied, missing planes are
  zeroed, and when nothing is connected the argument is `0` and the whole
  `:sidechain` block is zero. An effect never sees stale sidechain data.

"Disconnected gives `0`" is best effort. CLAP has no connected flag on a
buffer, and many hosts pass a silent buffer of full width for an unrouted
sidechain rather than none. An effect must therefore treat silence on a
connected sidechain the same as `0` channels, and never rely on the argument
alone to mean "no key signal".

**Constant planes.** A CLAP buffer's `constant_mask` marks planes whose value
is the same for the whole buffer (`clap/audio-buffer.h`). The shell reads such
a plane's sample 0 and fills the chunk with it, so an effect never sees the
difference. On output the shell writes every sample and clears
`constant_mask` on the main output.

**Both widths set.** CLAP says a buffer sets either `data32` or `data64`. If
a host sets both for a plane, `data32` wins, decided per plane, as the helpers
do today and whatever the effect's format. One rule in every helper keeps the
conversion tests exhaustive; it only matters for a misbehaving host.

## Bypass

`bypass-param-id` names the parameter the shells flag as the host's bypass
control, or is `BROM-NO-BYPASS` (`0xFFFFFFFF`) when there is none. The shells
never bypass by themselves: bypass is the effect's job. A bypassed effect
keeps its applied latency: it delays the dry signal by the value
`effect-latency-fn` currently returns (see below), so toggling bypass neither
changes that answer nor shifts the audio in time.

The CLAP shell sets `CLAP_PARAM_IS_BYPASS` and `CLAP_PARAM_IS_STEPPED` on it
(its host value follows [Parameters](#clap-parameters)). The VST3 mapping
is under [VST3 and AU](#vst3-and-au), the AU one under [AU](#au).

## Latency and tail

- `effect-latency-fn` returns, in its low 31 bits, the input-to-output delay
  in samples **that the DSP applies now**. Bit 31, `BROM-LATENCY-PENDING`
  (`0x80000000`), means a different latency is staged for the next
  `activate-fn`.
- `effect-tail-fn` returns how long output continues after input goes silent,
  in samples. `BROM-TAIL-INFINITE` (`0xFFFFFFFF`) means indefinitely; a
  finite tail is at most `0xFFFFFFFE`.

Both may change at run time, and not only in a block that carried an event:
a lookahead parameter moves the latency at once, while a convolver may finish
crossing over to a new response, and so change its tail, many blocks after
the parameter that started it.

Both functions, and `effect-current-program-fn`, run on the audio thread at
the end of every block. They must be wait-free: no locks, no allocation, no
I/O, no system calls, bounded time. In practice each reads a field the DSP
already maintains.

### Latency changes and the restart window

CLAP and VST3 fix the host-facing latency while the plug-in is active: CLAP
allows it to change only during `activate` (`clap/ext/latency.h`), and VST3
hosts pick up a new value by deactivating and reactivating the plug-in
(`kLatencyChanged`, see below; AU through the wrapper differs, see
[VST3 and AU](#vst3-and-au)). So between the block in which the effect's
latency changes and the host's reactivation, the host compensates the old
value while the DSP applies the new one, and the output is misplaced in time
by the difference. The length of that window is up to the host.

Effects therefore have two choices:

- **Apply at once.** The DSP switches immediately and `effect-latency-fn`
  returns the new value. The shell sees it differ from the host-facing value
  and asks for a restart. Audio is misaligned until the host reactivates.
- **Stage (recommended).** The effect records the new latency, keeps applying
  the old one, and returns the old value with `BROM-LATENCY-PENDING` set.
  The shell asks for a restart; `activate-fn` applies the staged value; the
  shell re-reads it after `activate-fn` and reports it. The DSP and the host
  never disagree, at the cost of the change taking effect only on
  reactivation.

The pending bit exists because a staged effect's applied latency does not
change until `activate-fn`, so without it the shell would never see a reason
to ask the host for the restart that applies it. Real latencies are far below
2^31 samples, so the bit costs nothing.

## Threading contract

The `brom-effect!` callbacks are never called concurrently with each other or
with `process`. The `engine!` callbacks keep the rules they already have; in
particular CLAP `params.get_value` calls `get-param-fn` on the main thread
while the audio thread may be processing.

| Callback | Called from |
|---|---|
| `effect-process-fn` / `effect-process-f64-fn` | audio thread, inside `process` |
| `effect-midi-fn`, `effect-program-fn` | audio thread, inside `process` |
| `effect-latency-fn`, `effect-tail-fn`, `effect-current-program-fn` | audio thread at the end of every `process` and of every active `params.flush`; main thread inside `activate`, after `activate-fn` and `reset-fn`; main thread, `effect-current-program-fn` only, while inactive |
| `effect-deactivate-fn` | main thread, inside `deactivate` |

Because every active block re-queries, a change is seen within one block
whatever caused it, and the shell needs no dirty flag. The shell caches all
three answers, and the host-facing getters and the editor read only the
caches, so an effect never has to make these callbacks thread-safe:

- `clap.latency.get`, VST3 `getLatencySamples` and AU
  `kAudioUnitProperty_Latency` read the latency cache;
- `clap.tail.get`, VST3 `getTailSamples` and AU `kAudioUnitProperty_TailTime`
  read the tail cache;
- `recoil_clap_plugin_current_factory_preset`, which the product editors call
  on the main thread, reads the current-program cache while the plug-in is
  active. While it is inactive no `process` can run, so it calls
  `effect-current-program-fn` directly and refreshes the cache.

The current-program cache fixes a race in revision 1:
`recoil-clap-plugin-current-factory-preset` (`entry.rcl`, around line 1254)
calls `effect-current-program-fn` on the GUI thread while `process` may be
running, which Punkva's implementation answers by reading the parameter values
the audio thread is writing. A program change reaches the cache at the end of
the block that applies it. A preset loaded from the editor or the host reaches
it at the end of the block whose transaction boundary commits it (see
[Presets and state](#clap-presets)); an editor cannot tell the two apart.

The caches start at zero (latency and tail) and `-1` (current program) when
the instance is created, before the first `activate`.

A change made on the main thread does not query the effect, except for the
inactive current-program read above. While the plug-in is inactive (an
inactive `params.flush`, a state load), the next `activate` picks it up.
While it is active, a state load is a preset transaction installed at the
next boundary, and the shell calls `host->request_flush` and
`host->request_process`. The second matters because the CLAP shell returns
`CLAP_PROCESS_TAIL` (see [Process status](#process-status)): a host may stop
calling `process` once the input is silent and the tail has run out, and the
request makes it run a block, whose boundary installs the state and whose
re-query sees it.

## Error paths

- **`process-fn` returns `false`.** The shell zeroes that chunk's output
  frames, writes them to the host, and goes on with the next chunk; the block
  returns its normal status. Both shells do the same. Zeroing is needed either
  way, because a refusing effect leaves its output block untouched (Punkva's
  `punkva-process` documents "false leaves audio and history unchanged"), so
  the scratch would otherwise replay an earlier chunk. Returning
  `CLAP_PROCESS_ERROR` instead was rejected: hosts react to it inconsistently,
  some by disabling the plug-in for the session, which is out of proportion
  to one refused chunk. This changes revision 1, which returns
  `CLAP_PROCESS_ERROR`.
- **Structural errors** still fail the block: no output buffer, an output
  with fewer than two channels or no data pointer, or a main input port with
  fewer channels than the selected layout. CLAP returns `CLAP_PROCESS_ERROR`
  and VST3 returns `kResultFalse`, as today.
- **No main input at all.** A process call with `audio_inputs` null or
  `audio_inputs_count == 0` reads the main input as silence, not as an error
  (`clap_support.c`, `recoil_clap_read_effect_audio`), so an effect inserted
  without input still renders its tail. A main input plane whose `data32` and
  `data64` entries are both null also reads as silence.
- **Activation.** `effect-deactivate-fn` runs only after a successful
  `activate`: the shell sets its active flag after `activate-fn` and
  `reset-fn` return, and `deactivate` calls `effect-deactivate-fn` only when
  that flag is set. An `activate-fn` that fails must release whatever it
  acquired itself.

## Validation

`brom-effect-valid?` runs once, when the plug-in binary is loaded: in CLAP
from `brom-clap-init-engine`, which the entry's `init` calls
(`entry.rcl`, `entry-init`; the check is around line 576), and in VST3 from
the module's init through `brom_effect_init` (see
[Shell interface](#shell-interface)), and in AU from the component's factory
function on every instantiation (see [AU](#au)). On failure the entry's `init`
returns `false`, so the host unloads the binary and the factory never exposes
a plug-in; no instance is created. It requires:

- `max-voices = 0`, parameter and state counts within `BROM-MAX-PARAMS` /
  `BROM-MAX-STATE-BYTES`, `output-capacity` within `BROM-AUDIO-CAPACITY`;
- `magic = BROM-EFFECT-MAGIC` and a `revision` from 2 up to the shell's own;
- a non-null `category`; `input-layouts` in 1..3; `sidechain-channels` in
  0..2; no `flags` bit other than `BROM-EFFECT-VARIABLE-TAIL`;
- `bypass-param-id` below `param-count`, or `BROM-NO-BYPASS`;
- a known `sample-format`, its matching process slot set, and every other
  process slot null (today one other; each format added later adds one);
- every other effect callback set, plus the engine lifecycle callbacks, and
  the parameter callbacks when `param-count > 0`.

Null callables are detected by comparing the slot's invocation bytes against a
zero-initialized descriptor (`brom-effect-callable?`), not by reading the slot,
since a zero-argument `fn-ptr!` field read invokes it.

## CLAP shell behaviour

**Ports**
- Input 0, "Main In": `IS_MAIN`, channels from the selected input layout,
  `in_place_pair` 0 (paired with the main output).
- Input 1, "Sidechain": present only when `sidechain-channels <> 0`; not main;
  that many channels; `in_place_pair` `CLAP_INVALID_ID`.
- Output 0, "Main Out": `IS_MAIN`, stereo, `in_place_pair` 0.
- An f64 effect sets `SUPPORTS_64BITS | PREFERS_64BITS` on every port; an f32
  effect sets neither. `REQUIRES_COMMON_SAMPLE_SIZE` is never set, because
  each port is converted independently.
- `clap.audio-ports-config` enumerates the input layouts; each configuration's
  `input_port_count` includes the sidechain.

**Features.** The descriptor's features are `audio-effect` plus the
`category` when it names a CLAP feature: `reverb`, `delay`, `filter`,
`equalizer`, `compressor`, `limiter`, `distortion`, `chorus`, `flanger`,
`phaser`, `pitch-shifter`, `analyzer` or `utility`. Any other category adds
nothing beyond `audio-effect`. Punkva's `"reverb"` reproduces today's fixed
list.

**Scratch.** The instance holds an input, a sidechain and an output block in
each format: 3 × 4096 + 3 × 8192 = 36,864 bytes, of which an effect uses one
set. The instance is heap-allocated, so the size is not a stack concern.

**Conversion.** Typed helpers in `clap_support.c` read and write one port
chunk:

- `recoil_clap_read_effect_f32/f64` fill a block from an input port and
  return an `int32_t`: the number of planes read, or `-1` for an error. A
  main input with fewer channels than the selected layout is an error. A
  sidechain is disconnected, returning `0` with the block zeroed, when the
  port has no buffer (`audio_inputs_count < 2`), its `channel_count` is 0, or
  both `data32` and `data64` are null.
- `recoil_clap_write_effect_f32/f64` write the stereo output and return
  `false` when it has fewer than two channels or no data pointer.

Each direction has four cases: an f32 effect copies from or to `data32` and
narrows from or widens to `data64`; an f64 effect widens from or narrows to
`data32` and copies from or to `data64`. A host may send 32-bit data even when
64-bit is preferred. The whole chunk, main input and sidechain, is read into
scratch before the effect runs and the output is written after, so an
in-place host buffer is safe in every width combination.

**Latency.** The shell keeps the value reported at activation, served by
`clap.latency.get`, which CLAP only allows to change during `activate`. If
`activate` sees a value different from the previous activation, it calls
`latency.changed` on the host's latency extension, when the host provides
one. The first activation compares against the initial cache of 0, so it
calls `latency.changed` only for a nonzero latency; the call is permitted
while being activated and harmless to a host that reads the latency
afterwards anyway. A different applied value, or `BROM-LATENCY-PENDING`,
seen at the end of a block does not change what `get` returns: the shell
calls `host->request_restart` (thread-safe) once, latched until the next
`activate`, and the host's deactivate/activate cycle reports the new value.

**Tail.** Cached through C11 `_Atomic uint32_t` helpers, because
`clap.tail.get` may be called from either thread. The shell reports
`BROM-TAIL-INFINITE` as `INT32_MAX` and clamps a finite tail to at most
`INT32_MAX - 1`, since CLAP reads any value at or above `INT32_MAX` as
infinite. A change seen at the end of an active block calls `tail.changed`,
which CLAP marks audio-thread, when the host provides it. A change seen in
`activate` only updates the cache: `tail.changed` may not be called from the
main thread, and the host reads the tail after activation anyway.
`BROM-EFFECT-VARIABLE-TAIL` does not change the CLAP shell's behaviour.

<a id="process-status"></a>
**Process status.** After the end-of-block re-query, the block returns
`CLAP_PROCESS_TAIL` when the cached tail is finite, letting the host stop
processing once the input is silent and the tail has elapsed, and
`CLAP_PROCESS_CONTINUE` when it is `BROM-TAIL-INFINITE`. Structural errors
return `CLAP_PROCESS_ERROR` (see [Error paths](#error-paths)). Revision 1
always returns `CLAP_PROCESS_CONTINUE`.

**Events.** Parameter, MIDI CC and program events are applied in time order at
their sample offsets, in three passes per time group: parameters, then MIDI,
then program changes. Splitting at 512 frames never reorders events.

<a id="clap-parameters"></a>
**Parameters.** A host slider, automation lane and generic MIDI-learn are
linear between a parameter's `min_value` and `max_value`, so an effect's
parameters are exposed the way the VST3 and AU shells expose them: the host
sees a normalized position and the shell applies the engine's taper. A
parameter whose steps are whole numbers from an integer minimum (`steps > 1`,
`hi - lo = steps - 1`, integral `lo`; the AU shell's indexed test) keeps its
engineering range and whole steps as its host value and is flagged
`CLAP_PARAM_IS_STEPPED`. Every other parameter has range 0..1, default
`param-normalize (default)`, and no `IS_STEPPED` flag. The mapping is
`param-denormalize` / `param-normalize` over the parameter's own
`param-range!`, which is what `brom_effect_param_denormalize` / `_normalize`
compute for the other shells.

- A `CLAP_EVENT_PARAM_VALUE`, in `process` or `params.flush`, reaches
  `set-param-fn` as `param-denormalize (value)`, clamped to the range; NaN
  sets the default.
- `params.get_value` answers `param-normalize (get-param-fn)`, so a preset,
  a loaded state, a MIDI CC or program change, or an editor edit shows the
  host where the engine is once the transaction boundary has installed it
  (the shell asks for a flush after a preset, an edit or an active state
  load, and for a values rescan once a state load is installed).
- `value_to_text` renders the engine's text (or `%.2f`) for the denormalized
  value; `text_to_value` parses engineering units (the engine's parse, or
  `strtod`) and returns the normalized position.
- An editor edit (`recoil_clap_plugin_edit_param`) is in engineering units, as
  in the other shells; the shell queues it as a preset-transaction edit,
  installed at the next boundary (at once while inactive), and pushes the host
  `param-normalize` of the clamped value, between the gesture events.
  Product editor adapters therefore pass engineering values and read the
  core directly; Punkva's and Macocha's do, and needed no change.
- The bypass parameter is an ordinary parameter here: Punkva's (`0..1`, two
  steps) is indexed and keeps its `0`/`1` host values.

State holds engineering values, never host values (see
[Presets and state](#clap-presets) for its framing), so a saved CLAP state
loads unchanged. What does change meaning is anything a host stored as
a parameter *value* outside the plug-in's state -- an automation lane, a
modulation or MIDI-learn mapping, a host-side parameter snapshot: before Istar
#201 the CLAP shell's effect host values were engineering units (Punkva's
decay 0.1..8 s, damping 200..16000 Hz), and such stored values now read as
positions on 0..1, clamped. Punkva and Macocha never shipped, so this break was
accepted rather than versioned. Instruments on the same shell
(`effect-api` none) are unchanged: their host values remain engineering units.

<a id="clap-presets"></a>
**Presets and state.** The CLAP shell routes presets, editor edits and state
through Brom's shared preset transactions (`brom/plugin/preset/`, design in
`docs/superpowers/specs/2026-10-18-brom-preset-transactions-design.md`) for
effects as for instruments. An effect differs in four ways:

- **No reset.** A committed load, restore or Compare switch applies the
  complete snapshot but never calls `reset-fn` on an effect: a reverb's tail
  runs on through a preset change, as Punkva's design requires, and an effect
  has no voices. Instruments are hard-reset as the transaction design says.
- **The boundary is at block end.** The transaction boundary follows the
  block's event timeline, as for an instrument: a load, restore or editor edit
  queued before the block wins the block's final state, is audible from the
  next block, and is seen by that block's latency/tail/program re-query.
  Revision 2 applied a queued preset at the start of the following block, one
  block earlier, and let that block's parameter events override it. `params.flush`
  is a boundary too.
- **MIDI writes Working.** `effect-midi-fn` and `effect-program-fn` change
  parameters inside the effect, past the transaction. The shell records the
  engine values before the call and feeds every value the message changed to
  the transaction as an audio-side edit, so a later boundary, a state save or
  a Compare switch keeps the message's values instead of reinstating older
  ones. A program change is therefore an edit of Working, not a preset load:
  Reference and the preset identity stay what they were, and Working reads as
  Changed against Baseline.
- **Host values stay normalized.** A host parameter event is mapped through
  `param-denormalize` before it reaches the transaction, so Working, Reference,
  Baseline and saved state hold engineering values.

State:

- `state.save` writes the engine's own container (`state-size` bytes, or the
  ASCII preset text for a `state-save-ascii` engine such as Macocha) for the
  published Working snapshot, via a scratch core. The bytes are those of
  revision 2.
- `state.load` reads to the end of the stream and accepts a Brom session
  envelope, Recoil-Preset text, or the engine's container, in that order. The
  last two migrate to Working = Reference = Baseline with an unknown identity.
  While the plug-in is active the restore is installed at the next boundary,
  not on the main thread, so `params.get_value` reports the old values until
  then. This removes the main-thread state-load race (Istar #199) for CLAP.
- `clap.state-context` (session envelope for project and duplicate contexts)
  is offered only to an engine with `has-state-context`; Punkva and Macocha do
  not opt in, so their project state stays the engine container.

Preset discovery now also lists user presets from the per-product preset
directory (`brom/plugin/preset` `preset-dir`), which the shell creates when
the plug-in binary loads; the factory bank keeps its keys. The VST3 and AU
shells keep their own preset and program handling (see
[VST3 and AU](#vst3-and-au)) and do not use the transactions yet.

## Shell interface

The generic VST3 and AU shells are C++ (Objective-C++ for AU), and cannot
call `brom-effect!` slots directly: a Recoil `fn-ptr!` is an `{env, invoke}` pair whose layout is a
Recoil ABI detail. Two ways were considered:

- a C++ mirror of the table layout, calling the pairs by hand, or
- Recoil-exported C trampolines that call through the table on the Recoil
  side.

The shell uses **trampolines**. They keep the `fn-ptr!` representation and
the table layout private to Recoil, so appending a slot or changing how
callables are stored never touches C++, and the C++ shell needs no
null-callable logic of its own. The cost is one extra direct call per
callback, which is negligible next to a 512-frame block.

`brom/plugin/effect.rcl` exports them, and `brom/plugin/effect.h` declares
them, with the constants and the same `static_assert` / `_Static_assert`
pattern as `brom/dsp/audio.h`: it includes `audio.h` for the block types and
asserts the `BROM_SAMPLE_*`, `BROM_TAIL_INFINITE`, `BROM_LATENCY_PENDING`,
`BROM_NO_BYPASS` and flag values; `effect.rcl` static-asserts the same
values on the Recoil side. Every trampoline acts on the engine linked into
the binary (`active-engine`), so none takes a descriptor:

```c
int      brom_effect_init(void);              /* brom-effect-valid?, once per module load */
const brom_effect_info *brom_effect_describe(void);
void    *brom_effect_create(void);
void     brom_effect_destroy(void *core);
int      brom_effect_activate(void *core, double sample_rate, int32_t max_frames);
void     brom_effect_reset(void *core);
void     brom_effect_deactivate(void *core);
int      brom_effect_process_f32(void *core, brom_audio_f32 *input,
                                 brom_audio_f32 *sidechain, brom_audio_f32 *output,
                                 int32_t channels, int32_t sidechain_channels, int32_t frames);
int      brom_effect_process_f64(void *core, brom_audio_f64 *input,
                                 brom_audio_f64 *sidechain, brom_audio_f64 *output,
                                 int32_t channels, int32_t sidechain_channels, int32_t frames);
int      brom_effect_midi(void *core, uint32_t status, uint32_t data1, uint32_t data2);
int      brom_effect_program(void *core, uint32_t index);
int32_t  brom_effect_current_program(void *core);
uint32_t brom_effect_latency(void *core);
uint32_t brom_effect_tail(void *core);
/* parameters: brom_effect_param_info (fills a brom_effect_param), _get, _set
   (clamped, NaN sets the default), _normalize, _denormalize (the parameter's
   taper), _text, _parse (the engine's own text, false when it has none);
   state: brom_effect_state_save, _load (state_size bytes);
   factory presets: brom_effect_factory_count, _name, _values */
```

Recoil's `logic!` crosses the C ABI as `int`, so every predicate returns
`int`, nonzero for true, matching the product headers (`punkva.h`). The
trampolines live in their own module, `brom/plugin/vst3/effect-shell`, which
only a shell's Recoil entry imports (the AU entries import it too, despite its
directory): they reference `brom_engine_ptr`, which
only plug-in binaries define, so an executable importing
`brom/plugin/effect` must not link them.

`brom_effect_info` is a plain C struct the shell owns: `sample_format`,
`input_layouts`, `sidechain_channels`, `bypass_param_id`, `flags`,
`param_count`, `state_size`, and the `id`, `name`, `vendor`, `version` and
`category` strings, filled from the descriptors once. It is asserted field by
field like `brom_audio_f32`. The process trampoline for the format the effect
does not use returns `false`.

**Identity.** Format identity is packaging data, not ABI, so it lives in the
product's format projects, passed to the shells as compile definitions and to
the AU plist template:

- the VST3 processor and controller FUIDs (Punkva's are in
  `examples/pokuston/punkva/vst3/CMakeLists.txt`);
- the AU `subtype` and `manufacturer` four-character codes, passed to
  `brom_add_effect_au` (Punkva's `Pnkv` / `PkDv` are in
  `examples/pokuston/punkva/au/CMakeLists.txt`, unchanged from its former
  wrapper plist).

They are build-time data because the AU `Info.plist` is read without loading
any code, and because none of them may ever change once a version ships.
The AU type is `aufx` (`kAudioUnitType_Effect`), or `aumf`
(`kAudioUnitType_MusicEffect`) when the product's AU project passes
`MIDI_INPUT` to `brom_add_effect_au`; it is part of the identity as well, so
it must not change once a version ships either (Punkva changed it once, before
its AU shipped on this shell, see [AU](#au)).

The VST3 subcategory string comes from `category`; every value below is a
constant in `pluginterfaces/vst/ivstaudioprocessor.h` (SDK 3.8.1, lines
48-71):

| `category` | VST3 subcategory |
|---|---|
| `reverb` | `Fx\|Reverb` (`kFxReverb`) |
| `delay` | `Fx\|Delay` (`kFxDelay`) |
| `filter` | `Fx\|Filter` (`kFxFilter`) |
| `equalizer` | `Fx\|EQ` (`kFxEQ`) |
| `compressor`, `limiter` | `Fx\|Dynamics` (`kFxDynamics`) |
| `distortion` | `Fx\|Distortion` (`kFxDistortion`) |
| `chorus`, `flanger`, `phaser` | `Fx\|Modulation` (`kFxModulation`) |
| `pitch-shifter` | `Fx\|Pitch Shift` (`kFxPitchShift`) |
| `analyzer` | `Fx\|Analyzer` (`kFxAnalyzer`) |
| `utility` | `Fx\|Tools` (`kFxTools`) |
| anything else | `Fx` (`kFx`) |

Punkva's `"reverb"` gives today's `"Fx|Reverb"`.

## VST3 and AU

One generic Brom effect shell per format, built on the
[shell interface](#shell-interface), to replace per-product effect C++. This
section is the VST3 shell under `brom/plugin/vst3/`; the AU shell has its own
section, [AU](#au), and the points below that name the AU wrapper describe the
VST3 SDK's wrapper, which the instruments still use and the effects no longer
do.
`effect_vst3.cpp` is the shell; `brom-effect-vst3.cmake` gives a product's
VST3 project `brom_effect_vst3_setup()` and `brom_add_effect_vst3(<target>
PROCESSOR_UID ... CONTROLLER_UID ...)`; a manifest's `vst3-entry` names the
Recoil entry (the engine plus `brom/plugin/vst3/effect-shell`) the driver
transpiles in the vst3 project. The probe effect's `vst3/` project is the
reference, and "Brom probe effect VST3 contract" (`tests/punkva.reb`,
`tests/fixtures/brom-effect-vst3-host.cpp`) checks it; Punkva's
`vst3/` project is the editor reference, checked by "Punkva VST3 effect
contract". An effect's AUv2 no longer comes from the VST3 SDK's wrapper in
this project: a manifest with `au-entry` builds it from its own `au/` project
on the generic AU shell, and the VST3 build then neither enables nor reports
the wrapper's component.

What the shell does beyond the points below:

- **Validation.** `GetPluginFactory` runs `brom_effect_init` before creating
  the factory and returns null when it fails, so a broken table exposes no
  plug-in. The SDK's own `InitModule` (`moduleinit.cpp`, inside the `sdk`
  library) always succeeds, so `bundleEntry` itself cannot be made to fail.
  A factory released and created again re-runs the check.
- **Buses.** Main input ("Main In", stereo when the effect takes stereo,
  mono otherwise; `setBusArrangements` accepts the layouts in
  `input-layouts`), an aux "Sidechain" of the declared width that is not
  active by default, stereo "Main Out", and one MIDI event input.
- **MIDI.** VST3 delivers controllers only through `IMidiMapping`. Each
  controller 0..127 maps to a hidden parameter at `0x40000000 + cc` and
  reaches `effect-midi-fn` as status `0xB0` (channel 0: every channel maps to
  the same parameter). Punkva's former shell mapped CC 20-26 straight to its
  parameters; on the generic shell they reach `punkva-midi`, which applies
  the same assignment, so a host's MIDI learn shows the hidden parameter
  rather than "Mix". Program change maps to the program parameter, id
  `param-count` as in Punkva's build: the factory bank's program list when
  the effect has one, else a hidden 128-step parameter. Within one sample
  offset, parameters apply first, then controllers, then the program, the
  CLAP shell's order.
- **Input silence flags.** A plane the host flags in `silenceFlags` reads as
  zero; the output's flags are cleared.
- **Editor.** `brom/plugin/vst3/effect_editor.h` is the hook, the VST3 twin
  of the CLAP shell's `recoil_clap_editor_*` adapter. A product with an
  editor passes `EDITOR EDITOR_SOURCES <adapter>` to `brom_add_effect_vst3`,
  which compiles its transpiled editor, embedded assets and the GUI package
  graph and defines `BROM_VST3_EDITOR`; the adapter defines
  `brom_vst3_editor_create/destroy/attach/detach/tick/preset_changed` over
  the product editor, and calls back `brom_vst3_host_edit_param` (engineering
  units, gesture phase 0 begin / 1 perform / 2 end) and the
  `brom_vst3_host_preset_*` menu functions the shell defines. `createView`
  returns a view only when the hook is compiled in and the engine sets
  `has-editor`, sized from `editor-width`/`editor-height`
  (`brom_effect_editor_size`). The view accepts an NSView parent on macOS and
  an X11 window on Linux, and a 33 ms main-thread timer ticks the editor and
  tells it when the controller's current program changes. The editor reads
  the controller's private instance, which mirrors every parameter the host
  sees. `examples/pokuston/punkva/vst3/source/punkva_vst3_editor.c` is the
  reference adapter.
- **State.** `getState`/`setState` stream exactly `state-size` bytes through
  `brom_effect_state_save`/`_load`; the controller decodes the same bytes in
  a private instance to show the values. Unlike the CLAP shell, an active
  state load cannot ask the host to process again: VST3 has no request.

Header paths below are relative to the pinned VST3 SDK 3.8.1
(`pokuston-vst3-sdk-b613aa5cb319-64965f1b74e0/vst3sdk/` in the Recoil package
upstream cache) and the macOS 26.2 SDK's
`AudioToolbox.framework/Headers/`.

**Sample size.** `canProcessSampleSize` (`ivstaudioprocessor.h:315`, called on
the UI thread) answers `kSample32` for every effect and `kSample64` only for
an f64 effect. An f64 effect converts 32-bit buffers; an f32 effect declines
64-bit, as Punkva's former per-product shell did, so the host
converts, and the "Punkva VST3 effect contract" keeps passing. Accepting
64-bit for an f32 effect would only move the same narrowing into the shell.
The VST3 SDK's AU wrapper always sets up `kSample32` (`public.sdk/source/vst/auwrapper/auwrapper.mm`
lines 635, 895-900, 1840); the generic AU shell instead accepts Float32 or
Float64 per element (see [AU](#au)).

**Sidechain.** An aux input bus (`BusTypes::kAux`, `ivstcomponent.h:85`) of
the declared width; the host activates it with `activateBus`. An inactive aux
bus gives `sidechain-channels = 0`, best effort as in CLAP. The AU wrapper
makes one AU input element per VST3 input bus (`auwrapper.mm:573-577`), so
the sidechain is AU input element 1, which the generic AU shell keeps.

**Bypass.** The `bypass-param-id` parameter carries
`ParameterInfo::kIsBypass` (`ivsteditcontroller.h:99-102`, one per plug-in).
The AU wrapper finds that parameter (`auwrapper.mm:938`, `2553`) and maps
`kAudioUnitProperty_BypassEffect` to it (`auwrapper.mm:1791-1799` set,
`1871-1878` get). While bypassed, the wrapper calls `process` with
`numSamples = 0` only to deliver parameter changes (`auwrapper.mm:2741-2752`),
so the effect's own latency-preserving bypass does not render on AU. What the
AU base class outputs while bypassed is unverified: the AudioUnitSDK
(`AUBase.h`) is not on disk here.

**Zero-length process.** A `process` call with `numSamples = 0` applies its
parameter changes, calls no process trampoline, and re-queries latency, tail
and current program, like an active CLAP `params.flush`.

**Latency.** `getLatencySamples` (`ivstaudioprocessor.h:326`) is called on the
UI thread. A change is told to the host with
`IComponentHandler::restartComponent (kLatencyChanged)`
(`ivsteditcontroller.h:136-141`, `217-240`), which the *controller* holds,
while the shell detects the change on the processor's audio thread. The
mechanism:

1. Processor and controller share one block of atomics: the latency and tail
   caches, the current program, and a restart-wanted flag. The processor
   allocates it and hands its address to the controller in an `IMessage`
   during `IConnectionPoint::connect`/`notify` (`ivstmessage.h:77-87`, both
   UI-thread). This requires both in one process, so the shell does not set
   `kDistributable` ("Component can be run on remote computer",
   `ivstaudioprocessor.h:104`), which Punkva's former per-product shell
   set.
2. At the end of each block the processor re-queries, stores the new latency
   into the shared cache, and sets the flag when the applied value changed or
   `BROM-LATENCY-PENDING` is set.
3. A controller-owned `Timer` (`base/source/timer.h`, UI thread; Punkva's
   editor already uses one at 33 ms) runs whether or not an editor is open,
   polls the flag, and calls `restartComponent (kLatencyChanged)` once,
   latched until the next `setActive (true)`.
4. `getLatencySamples` returns the shared cache, so it returns the **new**
   value as soon as the processor has published it, which is before
   `kLatencyChanged` is sent. The SDK only requires the new value after the
   host's `setActive (true)` (`ivsteditcontroller.h:136-141`), but the AU
   wrapper answers `kLatencyChanged` by raising
   `kAudioUnitProperty_Latency` at once (`auwrapper.mm:2533-2536`) and reads
   `getLatencySamples` directly (`GetLatency`, `auwrapper.mm:2103-2109`), with
   no reactivation. A staged change has no new value to publish until
   `activate-fn` runs, so on AU it waits for the next reinitialization (a
   format or sample-rate change, or reloading the project); the shell does not
   force one. An effect that must change latency live on AU applies it at
   once. The generic AU shell follows the same rule for the same reason
   (see [AU](#au)).

AU latency is in seconds (`kAudioUnitProperty_Latency`, Float64,
`AudioUnitProperties.h:199-204`); the wrapper divides samples by the sample
rate.

**Tail.** `getTailSamples` (`ivstaudioprocessor.h:349-360`) is called on the
UI thread and returns the shared cache, never the effect: the generic shell
must not do what Punkva's former per-product shell did, calling
`punkva_tail_samples` directly on the UI thread while the audio thread
processes. `RestartFlags` (`ivsteditcontroller.h:115-203`) has no tail flag,
so a host reads the tail when it chooses and is never told it changed. The
policy:

- Without `BROM-EFFECT-VARIABLE-TAIL`, the shell reports the cached tail,
  with `BROM-TAIL-INFINITE` mapped to `kInfiniteTail`. These are the same
  value: `kInfiniteTail = kMaxInt32u` (`ivstaudioprocessor.h:168`) and
  `kMaxInt32u = UINT32_MAX` (`pluginterfaces/base/ftypes.h:59`). The effect
  promises its tail does not change while active, as Punkva's does.
- With `BROM-EFFECT-VARIABLE-TAIL`, the shell always reports `kInfiniteTail`.
  A host that read a short tail and then saw the effect's tail grow would cut
  the output off, and there is no way to tell it otherwise. The limitation:
  the host never stops processing such an effect on its own, and an offline
  bounce ends wherever the host's own policy for infinite tails puts it.

AU tail is `kAudioUnitProperty_TailTime`, a Float64 in seconds described as a
conservative estimate, with no infinite encoding
(`AudioUnitProperties.h:290-296`). The wrapper computes it as
`getTailSamples () / sampleRate` (`GetTailTime`, `auwrapper.mm:2112-2117`),
so the AU value follows the VST3 one: a finite tail becomes seconds, and
`kInfiniteTail` becomes 4,294,967,295 / sample rate, about 89,478 s (24.9 h)
at 48 kHz, which serves as "infinite". The wrapper sends no tail change
notification (`restartComponent`, `auwrapper.mm:2509-2539`, handles only
parameter, MIDI-mapping and latency flags). Whether the AU publishes
`TailTime` to hosts at all is decided by the AudioUnitSDK base class the
wrapper builds on, which is not on disk here; that is unverified.

## AU

The generic AUv2 effect shell, `brom/plugin/au/effect_au.mm`, on Apple's
AudioUnitSDK 1.4.0 (the pinned `pokuston-audio-unit-sdk`), built on the
[shell interface](#shell-interface) like the VST3 shell and with the same
revision-2 behaviour wherever AU can express it. `brom-effect-au.cmake` gives a
product's `au/` project `brom_effect_au_setup()` and `brom_add_effect_au(<target>
SUBTYPE <4cc> MANUFACTURER <4cc> [MIDI_INPUT] [TAGS ...] [EDITOR EDITOR_SOURCES ...])`, which
compiles the SDK sources, the shell and the transpiled entry into
`<target>.component`, writes its Info.plist from `Info.plist.in` and signs the
bundle ad hoc. A manifest's `au-entry` names the Recoil entry (the engine plus
`brom/plugin/vst3/effect-shell`, the same trampoline module the VST3 entry
imports) that the driver's `build-pokuston-au` transpiles in the au project;
with `au-entry` set the AU is its own build step and needs no VST3 SDK, and the
VST3 project no longer enables the SDK's wrapper. The probe effect's `au/`
project is the reference, checked by "Brom probe effect AU contract"
(`tests/fixtures/brom-effect-au-host.cpp`); Punkva's `au/` project is the
editor reference, checked by "Punkva AU effect contract"
(`tests/fixtures/punkva-au-host.mm`). Both fixtures load the built component
from the build directory and register it in their own process with
`AudioComponentRegister`, using the factory function Info.plist names, so no
test installs anything or needs `auval`.

The shell derives from `AUBase`, not `AUEffectBase`: that class assumes one
input and one output element of equal width processed per channel, and fails a
render with no main input, none of which fits the effect ABI. It is
instantiated through `AUMIDIEffectFactory`, so the MIDI selectors reach it.

**Identity.** Type `aufx`, or `aumf` with `MIDI_INPUT` (see MIDI below); `SUBTYPE` and `MANUFACTURER` from the product's
`au/CMakeLists.txt`; name `<vendor>: <name>`; version `major << 16 | minor << 8
| patch` from the manifest; bundle id `<id>.au`; factory function
`BromEffectAUFactory`; `sandboxSafe`; `tags` from `TAGS` (default `Effects`).
The tags are a CMake argument, not derived from `category`, because a host
reads Info.plist without loading code and `category` is a run-time value.
Punkva's codes (`Pnkv` / `PkDv`) are the ones its wrapper-built AU published;
it adds the tags `Effects Reverb` and declares `MIDI_INPUT`, so it is now
`aumf Pnkv PkDv` where the wrapper published `aufx Pnkv PkDv`. A host keys a
saved plug-in by type, subtype and manufacturer, so sessions saved with the
old `aufx` component do not find the new one; by decision they are not
migrated (Istar #197).

**Validation.** `BromEffectAUFactory` runs `brom_effect_init` on every call,
before an instance exists, and returns null when it fails, so instantiation
fails and the host gets no plug-in. [Accepted deviation](#accepted-deviations) 1: per instantiation rather than
once per module load, because a component bundle has no module-init hook the
shell controls (the VST3 shell re-validates per factory the same way).

**Elements.** Input element 0 "Main In" (stereo when `input-layouts` has bit
1, else mono), input element 1 "Sidechain" of the declared width when
`sidechain-channels <> 0`, output element 0 "Main Out" (stereo). Formats are
Float32 or Float64, non-interleaved, chosen per element and converted
independently, as in CLAP; `kAudioUnitProperty_SupportedNumChannels` lists
2→2 and/or 1→2 from `input-layouts`. A format the effect cannot take is
refused when the host sets it, and `Initialize` re-checks the set, so the
"more planes than declared" and "fewer main channels than the layout" cases
of the other shells cannot arise: AU negotiates widths before rendering.
Hosts almost always render Float32, so an f64 effect usually converts.

**Inputs at render time.**
- A main input with nothing connected (no render callback or connection)
  reads as silence, so an effect inserted without input still renders its
  tail. A main pull that fails is a structural error: the render returns its
  status. `AUEffectBase` would return `kAudioUnitErr_NoConnection` for the
  first case; the shell follows the ABI instead.
- A sidechain element with nothing connected gives `sidechain-channels = 0`
  and a zero block. [Accepted deviation](#accepted-deviations) 2: a sidechain pull that *fails* also reads as
  disconnected rather than failing the render, best effort as everywhere for
  the sidechain. (The SDK refuses a pulled buffer list with a null plane, so a
  null sidechain plane is such a failure; the VST3 shell's "null plane reads
  zero" case becomes "nothing read".)
- An input the host pulls back with `kAudioUnitRenderAction_OutputIsSilence`
  reads as zero (VST3's `silenceFlags`). The shell writes every output sample
  and clears that flag on its render.
- Every plane of a chunk is read into scratch before the effect runs, so an
  input that aliases the output buffer (`ShouldAllocateBuffer = 0`) is safe in
  every width combination.

**Parameters.** A host slider and automation lane are linear between a
parameter's minimum and maximum, so the AU shell does what the VST3 and
CLAP shells do: the host sees a normalized position and the shell applies the
engine's taper. A parameter whose steps are whole numbers from an integer
minimum is `kAudioUnitParameterUnit_Indexed`, keeps its whole steps as its
host value (minimum to maximum, the engine value itself) and has
`ParameterValueStrings` from the engine's text. Every other parameter is
`Generic` with range 0..1 and default `brom_effect_param_normalize (default)`;
a host value reaches the engine as `brom_effect_param_denormalize (value)`,
the parameter's own taper and quantisation, and an engine value (a preset,
loaded state, a program the render thread applied, an editor edit) is shown
to the host as `brom_effect_param_normalize (value)`. Nothing is flagged
`DisplayLogarithmic`: the taper is already in the mapping. Every parameter has
`ValuesHaveStrings`, answered by `ParameterStringFromValue` (the engine's text
for the denormalized value) and `ParameterValueFromString` (the engine's parse,
normalized), with plain `%.2f` / `strtod` as the fallback, as in the other
shells, so a host displays engineering units. Host values pass through AU's
`Float32`, which quantizes the normalized position to about 6e-8 of the
travel; the engine value itself is computed in `f64`. Unscheduled values,
scheduled points and ramp endpoints all go through the same mapping. Istar
#196 made this change; before it, AU parameters were engineering units and a
non-geometric taper displayed and automated wrongly.

**Event timing and order.** At the start of each render the shell applies
every parameter the host set without scheduling (`AudioUnitSetParameter`)
since the last render, by comparing against the values it last applied;
scheduled points (`AudioUnitScheduleParameters`) apply at their buffer
offsets, and MIDI at its own; the render is split at each event and at 512
frames. Within one offset parameters apply first, then MIDI, then program
changes: the CLAP order. Deviation: a scheduled ramp applies its start value
at its start and its end value at its end when that falls inside the render,
not the values in between (an effect that smooths, as Punkva does, smooths
across the step). One render takes at most 1024 scheduled points and 512 MIDI
events; more are dropped.

**MIDI.** An AUv2 effect receives MIDI 1.0 only through the MusicDevice
selectors, `MusicDeviceMIDIEvent` (`kMusicDeviceMIDIEventSelect`) above all,
which the host calls on its own thread before the render the events belong
to, with a sample offset into that render. Hosts offer MIDI routing to an
effect only when its component type is `aumf` (`kAudioUnitType_MusicEffect`,
"an effect that also responds to MIDI"); to an `aufx` they generally send
none. So an effect that must receive MIDI (Punkva's CC 20-26 and program
changes, Macocha) declares `MIDI_INPUT` in its AU project and is built as
`aumf`; nothing else in the shell differs. Both types are instantiated
through `AUMIDIEffectFactory`, so the MIDI selectors dispatch either way and
an `aufx` that a host does send MIDI to still receives it.

The status byte reaches `effect-midi-fn` unchanged, channel included (unlike
VST3, where every channel collapses to 0); program change (`0xCn`) goes to
`effect-program-fn` with its program number, as in CLAP; every other message
goes to `effect-midi-fn`. Each event applies at its offset in the next render;
events queued while the unit is uninitialized are discarded by `Initialize`.
MIDI 1.0 only: `MusicDeviceMIDIEventList` (UMP, MIDI 2.0) and
`MusicDeviceSysEx` keep `AUBase`'s `kAudio_UnimplementedError`, and the shell
publishes no `kAudioUnitProperty_AudioUnitMIDIProtocol`. MIDI 2.0 is deferred
to a later version (see [Later versions](#later-versions)).

The fixtures deliver MIDI through `MusicDeviceMIDIEvent` on the registered
component, the path a host uses: "Brom probe effect AU contract" checks every
MIDI 1.0 channel-voice kind, program changes and timing; "Punkva AU effect
contract" checks that CC 20 reaches Punkva's mix. What only a real host can
confirm is that it offers MIDI routing to the `aumf` and calls
`MusicDeviceMIDIEvent` rather than only `MusicDeviceMIDIEventList`; for Logic
Pro that is the MIDI side-chain selector in the plug-in header; no host has
been tried.

**Programs.** A factory bank is published as `kAudioUnitProperty_FactoryPresets`.
Choosing one (`PresentPreset`, or the editor's preset menu) on the main thread
sets the private instance and every parameter the host sees to the preset's
values and hands the program to the audio instance, which applies it at the
start of its next render (in the program pass), or at once when the unit is
not initialized. An effect whose program callback refuses still gets the
bank's values, as in the VST3 shell. A program the render thread applied (a
MIDI program change) becomes the present preset at the next 33 ms tick, and
the host-visible parameters are refreshed from the engine's
`get-param-fn` on the main thread, which the engine contract allows (CLAP's
`params.get_value` does the same). Parameter changes the engine makes itself
in response to a controller (Punkva's CC 20-26) are not reflected in the
host's parameters, as in the VST3 shell.

**Current program.** The editor's current-preset query reads the cache while
the unit is initialized and calls `effect-current-program-fn` directly while
it is not, as the threading contract says.

**State.** `kAudioUnitProperty_ClassInfo` is the SDK's dictionary (parameter
values, preset name) plus the effect's `state-size` bytes under `brom-state`,
saved from the audio instance on the main thread as the CLAP and VST3 shells
save it. On restore, `brom-state` must be exactly `state-size` bytes the engine
accepts, checked on the private instance before anything changes, or the
property is refused; the host-visible parameters then show the loaded values
at once. [Accepted deviation](#accepted-deviations) 4, a difference from the other shells: an initialized
unit hands the bytes to the render thread, which loads them at the start of
its next render, so a load never runs under a render (the race the
[known issue](../README.md) describes for CLAP); an uninitialized unit loads
them at once. CLAP and VST3 load directly on the calling thread; aligning them
is Istar #199 ([accepted deviation](#accepted-deviations) 4). A dictionary
without `brom-state` restores only the parameter values. Not migrated, by
decision: sessions saved by Punkva's former wrapper-built AU, which was
`aufx` and carried the wrapper's state, not `brom-state`
([accepted deviation](#accepted-deviations) 5).

**Bypass.** With a `bypass-param-id`, `kAudioUnitProperty_BypassEffect` reads
and writes that parameter (its maximum for on, its minimum for off, through
`AUParameterSet` so hosts see it), and a change of the parameter from
elsewhere is announced on the property at the next tick. The effect bypasses
itself and keeps its latency; the shell keeps rendering through it, unlike the
VST3 SDK's wrapper, which stopped rendering while bypassed. Without a bypass
parameter the property is absent and the host bypasses on its own.

**Activation.** `Initialize` is activation (`activate-fn` with the output
sample rate and `MaximumFramesPerSlice`, then `reset-fn`, then the caches are
read), `Cleanup` (uninitialize) runs `effect-deactivate-fn`. `AudioUnitReset`
on an initialized unit is deferred to the start of the next render, so
`reset-fn` never runs under a render.

**Latency.** `kAudioUnitProperty_Latency` is the latency cache divided by the
sample rate. An AU host rereads it when told it changed and does not
reinitialize, so the shell publishes a new applied value at the end of the
render that saw it, as the VST3 shell does for the SDK's wrapper, and a
CoreFoundation timer on the main run loop (33 ms, running whether or not an
editor is open, like the VST3 controller's) raises the property-change
notification once, latched until the next `Initialize`; `PropertyChanged` is
not render-thread safe. `BROM-LATENCY-PENDING` also asks for that
notification, but the host's reread finds the old value: a staged latency is
applied by the next `Initialize` (a format or sample-rate change, reloading the
project), which the shell does not force ([accepted
deviation](#accepted-deviations) 3). An effect that must change latency live
on AU applies it at once.

**Tail.** `kAudioUnitProperty_TailTime` is the tail cache divided by the
sample rate (`SupportsTail` is true). AU has no infinite encoding:
`BROM-TAIL-INFINITE` and, as in the VST3 shell, every tail of an effect with
`BROM-EFFECT-VARIABLE-TAIL` are reported as UINT32_MAX samples, ~89,478 s at
48 kHz. A change of the reported value raises a `TailTime` notification at
the next tick; whether a host acts on it is the host's.

**Zero-length render.** A render of 0 frames applies its events, pulls no
input, calls no process trampoline and re-queries latency, tail and current
program.

**Editor.** `brom/plugin/au/effect_editor.h` is the hook, the AU twin of
`brom/plugin/vst3/effect_editor.h` function for function
(`brom_au_editor_*` on the product side, `brom_au_host_*` on the shell side).
With `EDITOR` the shell defines `BROM_AU_EDITOR` and publishes
`kAudioUnitProperty_CocoaUI` when the engine sets `has-editor`: the bundle's
URL and a view factory class conforming to `AUCocoaUIBase`. Objective-C class
names are one namespace across everything a host loads, so the factory and
view classes are `BromAUViewFactory_<manufacturer><subtype>` and
`BromAUView_<manufacturer><subtype>`. The factory finds the shell instance
through the shell's own property (`64042`) and returns an NSView of
`editor-width` × `editor-height`. Deviations from the VST3 view: the editor is
attached the first time the view joins a window, not when it is created; the
33 ms tick (editor tick and preset-change notice) runs only while the view is
in a window; the shell tracks its views and detaches their editors if the unit
is disposed first. Edits go through `AUParameterSet` with begin/end gesture
events; the private instance follows the host's parameter values at each
33 ms tick rather than at once. `examples/pokuston/punkva/au/source/punkva_au_editor.c`
is the reference adapter.

**Build.** The component is built for the Rebol host's architecture (as the
VST3 build is), with CMake's default generator and C++23 (the SDK uses
`std::expected`). On a machine where Rebol runs as x86_64 the component is
x86_64, which an arm64 host can load only through Rosetta or out-of-process
hosting; arm64 and universal builds are deferred
([later versions](#later-versions), Istar #198).

**Not verified.** `auval`, any real host, out-of-process (AUHostingService)
loading, sandboxed hosts, and whether a host routes MIDI to the `aumf` and
calls `MusicDeviceMIDIEvent`. The fixtures exercise the AudioToolbox API in
process. After installing the component
(`~/Library/Audio/Plug-Ins/Components/`), `auval -v aumf Pnkv PkDv`
validates Punkva.

### Accepted deviations

Where the AU shell departs from the revision-2 behaviour of the CLAP shell,
deliberately and for good; each is described in its paragraph above.

1. **Validation on every instantiation.** `BromEffectAUFactory` runs
   `brom_effect_init` each time a host creates an instance, not once per
   module load: a component bundle has no module-init hook the shell
   controls. The check is cheap and has no side effects, so repeating it
   costs nothing; a broken table still yields no instance.
2. **A failed sidechain pull reads as disconnected.** When pulling the
   sidechain element fails, the render goes on with `sidechain-channels = 0`
   and a zero block instead of failing. The SDK refuses a pulled buffer list
   with a null plane, so the VST3 shell's "a null sidechain plane reads as
   zero" cannot be expressed: the whole element is unread, which is the
   ABI's best-effort sidechain rule.
3. **A pending latency waits for the next `Initialize`.** `BROM-LATENCY-PENDING`
   raises one `kAudioUnitProperty_Latency` notification, but an AU host
   rereads the property when told and does not reinitialize on request, and
   the shell does not force one. The staged latency is applied by the next
   `Initialize` (a format or sample-rate change, reloading the project). An
   effect that must change latency live on AU applies it at once.
4. **State load on the render thread.** An initialized AU unit hands loaded
   `brom-state` bytes to the render thread, which loads them at the start of
   its next render; the CLAP shell (`state.load`) installs them at its next
   transaction boundary, at block end (see [Presets and state](#clap-presets)),
   and the VST3 shell (`setState`) loads them directly on the calling thread.
   The AU and CLAP schemes are the safer ones, since a load never runs under a
   render. Aligning VST3 is a later version's work, tracked as Istar #199;
   until then the difference stands.
5. **Old Punkva AU sessions are not migrated.** Sessions saved with
   Punkva's former VST3-SDK-wrapper AU (`aufx Pnkv PkDv`, the wrapper's own
   state, no `brom-state`) do not load into the generic shell's component
   (`aumf Pnkv PkDv`): the component identity differs, and the state format
   would too. This is a decision, not an oversight (Istar #197).

### Later versions

What the AU shell leaves for a later version, on purpose:

- **MIDI 2.0.** `MusicDeviceMIDIEventList` (UMP) and the
  `AudioUnitMIDIProtocol` / `HostMIDIProtocol` properties. Until then an AU
  effect receives MIDI 1.0 through `MusicDeviceMIDIEvent` only, and a host
  that delivers only MIDI event lists reaches no effect callback.
- **arm64 and universal builds.** The component is built for the Rebol host's
  architecture, x86_64 on the machines the tests run on, which an arm64 host
  loads only through Rosetta or out-of-process hosting. arm64 or universal
  binaries for the AU, VST3 and CLAP bundles are needed before shipping and
  are tracked as Istar #198.
- **One state-load scheme for every shell.** Moving the VST3 state load onto
  the render thread as AU does and CLAP now does through preset transactions,
  or documenting why not: Istar #199 (accepted deviation 4 above).

## Revision 2 rollout

### Package

`recoil-brom` goes from version 0.1.0 to 0.2.0 in
`packages/recoil-brom/recoil.package.reb`, so a project or store pinned to
`recoil-brom-0.1.0` fails to resolve with a version error instead of linking a
revision-1 shell against revision-2 effects.

### Punkva

Punkva's DSP (`brom-reverb-process`) and its core API (`punkva-process` in
`core.rcl`, exported through `punkva.h`) do not change. The core is also
called directly by `core-test.rcl`; the per-product `punkva_vst3.cpp` that
also called it is gone, replaced by the generic VST3 shell.

- **Adapter.** A new `punkva-effect-process` in `plugin.rcl` has the
  revision 2 signature. It ignores `:sidechain` and `sidechain-channels` and
  forwards to `punkva-process`.
- **Table.**
  - `magic: BROM-EFFECT-MAGIC`, `revision: 2`, `sidechain-channels: 0` (no
    sidechain port), `bypass-param-id: 6` as today, `flags: 0`.
  - `sample-format: BROM-SAMPLE-F32`, because Punkva targets the ESP32-S3's
    single-precision FPU. `effect-process-f64-fn` stays null.
  - `effect-latency-fn: :punkva-latency`, which returns 0: the FDN has no
    lookahead.
- **Tail.** It stays the fixed, conservative horizon computed at activation
  (`brom-reverb-tail-samples`). The shell first reads it in `activate`, after
  the delay lines are sized, so it is never the inactive zero. The per-block
  re-query returns the same value while active, so `tail.changed` is never
  sent. Over-reporting costs a slightly longer bounce, while under-reporting
  would cut the reverb off. The dynamic-tail path is exercised by the probe
  effect and later by Macocha.
- **Sound.** Punkva's audio output must stay bit-identical.

### Tests

The existing tests are extended rather than duplicated: the Brom contract
tests and the host fixtures (`tests/fixtures/punkva-clap-host.c`,
`punkva-vst3-host.cpp`) in `tests/punkva.reb`.

1. **ABI contract** (`packages/recoil-brom/test/effect-test.rcl`, plain and
   optimized).
   - `brom-effect-valid?` rejects:
     - a wrong `magic`, and a revision-1-shaped table;
     - `revision` 0 and 3;
     - `sample-format` 0 and 5;
     - both process slots null, both set, and only the slot that does not
       match `sample-format`;
     - `input-layouts` 0 and 4, `sidechain-channels` 3, a `flags` bit other
       than bit 0;
     - a missing `effect-latency-fn`;
     - `bypass-param-id = param-count`.
   - It accepts a well-formed f64 effect, one with `BROM-NO-BYPASS`, and one
     with `BROM-EFFECT-VARIABLE-TAIL`.
   - The fn-ptr pair-width static assert moves to the two adjacent process
     slots. It covers only the pair it names, so each slot appended by a
     later revision gets its own assert.
   - `effect.h`'s constant and `brom_effect_info` asserts build in C and C++.
2. **Probe effect and generic CLAP fixture.**
   - **`brom-probe-effect`** lives in `packages/recoil-brom/test/probe-effect/`:
     `plugin.rcl`, with `clap/clap_entry.rcl`, `clap/build.r3` and
     `clap/recoil.project` laid out like `examples/pokuston/punkva/clap/`, and
     built the same way (`r3 -s packages/recoil-brom/test/probe-effect/clap/build.r3`
     from the Pokuston root). It is f64 with a stereo sidechain, category
     `"utility"`, and `BROM-EFFECT-VARIABLE-TAIL`. Its output is input plus
     sidechain times a marker gain, so routing checks exactly. A "latency"
     parameter drives a plain delay line, applied at once, and a "staged
     latency" parameter applies on the next activation through
     `BROM-LATENCY-PENDING`. A "tail" parameter's top value is
     `BROM-TAIL-INFINITE`. Changing the tail parameter takes effect a fixed
     number of blocks later, so the tail changes in a block with no event.
   - **`tests/fixtures/brom-effect-clap-host.c`** builds the probe as CLAP and
     checks:
     - **Ports:** input count 2, `SUPPORTS_64BITS | PREFERS_64BITS`, a
       sidechain port that is not main with `in_place_pair` `CLAP_INVALID_ID`,
       the port-config counts, and the `utility` feature.
     - **Conversion:** the four f64-effect cases, using 32- and 64-bit host
       buffers, in place and not, exact against expected values. The four
       f32-effect cases come from the Punkva fixture below. A buffer with both
       `data32` and `data64` set reads `data32`; a plane flagged in
       `constant_mask` reads as its sample 0.
     - **Sidechain:** zeroed when disconnected (each of the three
       disconnected forms), a mono buffer on the stereo port giving
       `sidechain-channels = 1` with the right plane zeroed, a three-channel
       buffer clamped to 2, and stereo passed through.
     - **Latency:** reported in `activate`, with `latency.changed` on a first
       activation only when nonzero. A parameter change calls
       `request_restart` once, which the fixture records, and `get` still
       returns the old value. On reactivation, `latency.changed` fires and
       `get` returns the new value. The staged parameter does the same, with
       the old latency applied until reactivation.
     - **Tail and status:** the delayed change fires `tail.changed` on the
       processing thread in an event-free block, and a change through
       `params.flush` does too. The infinite setting reports `INT32_MAX` and
       the block returns `CLAP_PROCESS_CONTINUE`; a finite one returns
       `CLAP_PROCESS_TAIL`.
     - **Errors:** a probe setting that makes `process-fn` return `false`
       yields zeroed output for that chunk and a normal status.
     - **Validation:** a broken table makes the entry's `init` return `false`.
3. **Punkva regression.**
   - The "Punkva CLAP effect contract" fixture also asserts:
     - no 64-bit flags and no sidechain port;
     - latency 0, and no `latency.changed` on first activation;
     - a nonzero tail that is unchanged after parameter moves, with
       `tail.changed` never sent, and `CLAP_PROCESS_TAIL` returned;
     - the editor's current-preset query while processing returns the cached
       program after a preset load.
   - **Bit-identity oracle.** The fixture renders a fixed input through CLAP
     and, in the same run, drives a second core through the exported
     `punkva_process` with the same calls the shell makes: the same chunk
     boundaries (event offsets and 512-frame splits) and the same parameter
     changes at those offsets. The two outputs must match bit for bit. It
     covers stereo and mono layouts, a host block longer than 512 frames, and
     parameter events in the middle of a block, with 32-bit host buffers and
     with 64-bit ones (off-spec for an f32 effect, but the shell accepts
     them). The oracle lands in its own commit, passing against revision 1,
     before any ABI change.
   - "Punkva VST3 effect contract" must pass unchanged.
4. **Instruments unaffected.** `entry.rcl` is shared by every product, so the
   CLAP instrument tests in `tests/pokuston.reb` (Kapralova, Loos, Mach) run
   too, on macOS, through the Recoil runner with `--test-root`. Runs are
   focused (`-F punkva.reb`, `-F pokuston.reb`, the Brom package tests), one
   at a time, never the full suite.

## Roadmap

1. This revision in `effect.rcl`, `effect.h`, `audio.h`, `audio.rcl` and the
   CLAP shell; Punkva updated (it stays f32), as above.
2. The generic VST3 and AU effect shells on the trampolines (done).
3. Macocha (convolution reverb, f64) ported onto this ABI, replacing its own
   effect mode (done; `examples/pokuston/macocha/`, Istar #187). What the
   port does:
   - **Table.** `sample-format` `BROM-SAMPLE-F64` with only
     `effect-process-f64-fn` set, `input-layouts` 3, no sidechain,
     `BROM-NO-BYPASS` (a host bypasses it), latency 0, and
     `BROM-EFFECT-VARIABLE-TAIL`. MIDI CC 20-32 move its thirteen parameters
     through their tapers; program changes select its four factory caves.
   - **Tail.** `macocha-tail` is the `u32!` adapter: the response being heard
     plus the longer of the target and the gliding pre-delay. While the
     convolver crosses over, `brom-conv-length` reports the longer of the old
     and new responses, so the tail never shrinks under audio that is still
     ringing ("Macocha DSP" checks both directions).
   - **Mono.** The core spreads the left plane to both sides when
     `channels = 1` and never reads the right plane.
   - **State.** `engine!` state is a fixed `state-size` of raw bytes, which
     cannot carry the path of an impulse-response file. Macocha therefore
     ships the parametric cave model only; recorded responses wait for that
     decision (Istar #188: an index into a bundled set, a content hash, or a
     later variable-size state extension).
   - **AU.** The generic AU shell takes Float64 where a host offers it, but
     hosts almost always render Float32, so f64 Macocha usually converts
     there, and its variable tail is reported as UINT32_MAX samples, ~89,478 s
     at 48 kHz (see [AU](#au)). VST3 reports it as `kInfiniteTail`.
