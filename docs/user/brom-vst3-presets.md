# Brom VST3 preset services

This describes the reusable Brom pieces that carry preset commands between a VST3
processor and its controller, and what a product has to supply to use them. Nothing
here is adopted by an instrument yet: the services are tested against a synthetic
host that links no instrument. Product migration is separate work.

The pieces live in `packages/recoil-brom/brom/plugin/`:

| File | Role |
| --- | --- |
| `vst3/preset_protocol.hpp/.cpp` | Bounded, versioned copied messages |
| `vst3/preset_bridge.hpp/.cpp` | Processor and controller endpoints |
| `vst3/process_timeline.hpp` | The run-to-completion timeline overload |
| `vst3/state_stream.hpp` | Session read and write through `IBStream` |
| `preset/catalog_service.h/.c` | Browser pins and host program mappings |
| `cmake/BromPresetVst3.cmake` | The support target and the function that attaches it |
| `preset/program_map.h` | The program rules (VST3 and AU), header-only, usable without linking Brom |
| `cmake/BromProgramMap.cmake` | The program-rule flavor targets and the configure-time flavor checks |
| `vst3/preset_program_parameter.hpp` | A host list parameter that names the preset that sounds |

## Who owns what

The **processor** owns the one transaction, and with it the working, reference and
baseline snapshots. The **controller** holds a copied published view and a table of
pending request receipts. Neither holds a pointer into the other; they exchange
copied messages over `IConnectionPoint`.

Controller commands (`submitLoad`, `submitCompare`, `submitEdit`, `submitSaveAck`)
are requests. The processor answers each with an acceptance reply and, later, a
completion reply. The acceptance reply carries the generation the processor
accepted. A command sent before that reply depends on the earlier request, so the
controller never guesses the next generation.

A receipt is `complete` when the result is **published**, not when a message was
delivered. A load or restore completes after it is committed, or, while the
processor is inactive, after it is pending. Something a newer request replaced
completes as `BROM_PRESET_SUPERSEDED`; something aimed at a view that no longer
exists completes as `BROM_PRESET_STALE`.

## Threads

Verified against the pinned VST3 SDK (3.8.1) rather than assumed:

* `IConnectionPoint::connect`, `disconnect` and `notify` are all documented as
  **UI-thread** calls.
* The SDK's own `ConnectionProxy::notify` **discards** a message sent from any other
  thread and returns `kResultFalse`.
* `IAudioProcessor::setProcessing` may be called on the UI thread **or** the
  processing thread, and a host may not call it at all. `setActive` is a UI-thread
  call that never overlaps `process()`.

So the rules are:

* `process()` calls only `audioEdit`, `audioSelectProgram` and `finishTimeline`.
  They never allocate, take a lock, send a message or call a host notification.
  The fixture counts heap allocations and `notify` calls made inside them.
* **`pump()` must run on the host's main (UI) thread**, because that is where
  `notify` is accepted. A worker thread would have its messages discarded.
  Everything else (`receive`, `connect`, `disconnect`, `setActive`, `restore`) is
  also main-thread work.
* A failed `notify` is not an error: the endpoint keeps the message, with its
  original request ID and sequence, and `pump()` sends it again. The receiver
  recognizes a repeat by request ID and never runs a command twice.

### The pump needs a host service, not just an editor

The editor's own timer is not enough. With the editor closed, nothing would call
`pump()`, so processor replies and views would sit unsent and a controller
somewhere else (a host parameter list, a state save) would show stale data.

A product must therefore provide a **main-thread timer that exists while no editor
is open**. The SDK's own `Timer` class (`base/source/timer.h`) is such a timer on
macOS and Windows: it hangs off the run loop of the thread that creates it, so
create it in `initialize()`, on the main thread, and stop it in `terminate()`. The
synthetic plug-in does exactly this, with a 10 ms interval, and calls `pump()` on its
processor and its controller from it. The SDK's `IRunLoop` timer is a Linux-only
interface that a plug-in reaches through an open editor's `IPlugFrame`, so it cannot
be the only source on any platform.

If a host provides no main-thread run loop at all, a product should say so before
migrating rather than send notifications from the audio thread. No Brom code does
that.

## Activation: `setActive` is the ownership boundary

Two consumers can drain the transaction, and they never overlap:

* **Active:** the audio thread, at the end of every `process()` call, through
  `finishTimeline()`. It captures its cutoff after the whole callback timeline, so
  later arrivals wait for the next boundary, and a zero-frame call still reaches it.
* **Inactive:** the main thread, inside the bridge. It reduces whatever is queued
  through the same boundary, **calls no DSP hooks**, and publishes the result as a
  pending view. Pending state answers `readView` and state saves at once.

`setActive(false)` hands the queue to the inactive consumer. `setActive(true)` is
the single resume function: under the endpoint lock and before any `process()`, it
applies the pending view through the hooks (apply, then hard reset, then note
clear), publishes it, and only then lets the audio side consume. If nothing changed
while inactive, resume applies nothing. Activating and deactivating without a
`process()` call in between never applies anything.

`setProcessing` is **only a hint**. It may run on the processing thread or be
skipped, so Brom never uses it to decide who owns the transaction and takes no lock
there.

## Host parameter events and refresh echoes

A host parameter event carries only an ID, a sample offset and a value, so an echo
cannot be told from a user edit by looking at it. Brom avoids echoes by design and
offers one explicit rule for hosts that still echo.

* **Refresh without events.** When a view arrives, update the host's parameters with
  `setParamNormalized` followed by `restartComponent(kParamValuesChanged)`. Do not
  call `performEdit`. The processor then sees no parameter events and nothing is
  marked Changed.
* **One-shot echo rule (limited).** `armEcho(id, plainValue)` is a processor-side
  primitive: the first `audioEdit` with that exact ID and value, before the next
  boundary, is taken as an echo and changes nothing. It has real limits, found in
  review and not designed away:
  * There is no controller-to-processor message that arms it. The controller pushes
    the value, so only code that holds the processor object can arm (a combined or
    in-process adapter); a split VST3 controller cannot.
  * An expectation expires at the next boundary. A host that echoes a value some
    blocks after the refresh finds it already expired.
  * There are 8 slots, and a view refresh can touch many more parameters.
  * Matching is exact on the plain value after the normalized-to-plain round trip.

  So **refresh without events is the supported rule**. A host that echoes
  refreshed values back as parameter events is not covered by the bridge: an
  adopting product must handle it itself (for example by ignoring an event equal to
  the working value) or accept the edits. Giving the rule a controller-originated,
  time-bounded arm message is a product decision recorded in the adoption checklist.

`audioEdit` returns whether the DSP should move now. While Reference is audible an
edit does not reach the DSP; the next boundary returns to Working and applies it.

## State

* `getState`: read one coherent view with `PresetProcessorBridge::readView` and write
  it with `writePresetSession`. Queued active requests are still pending and are not
  in it; the committed view is.
* `setState`: read with `readPresetSession` into temporary storage, then call
  `restore` only on success. The reader frames by the envelope's own header, consumes
  exactly its declared payload, bounds the declared size before reading further, and
  copes with short counts. A wrong product, a damaged payload or an oversize header
  leaves the processor untouched.
* An inactive restore is pending at once: it answers `readView` and the next state
  save immediately, and reaches the DSP only when `setActive(true)` resumes.
* Legacy fixed-size blobs stay product callbacks: decode the legacy state to a
  validated view and pass that to `restore`. Brom assumes no fixed-size product
  layout. The existing `readState`/`writeState` helpers remain for those callers.
* On the controller, `setComponentView` shows a validated copy of the component's
  saved view until the processor's own view arrives. It sends nothing and creates no
  request, and it is refused once a live view is showing.

## Showing errors

Every receipt carries a `brom_preset_result`. A product should surface at least:

| Result | Meaning for the user |
| --- | --- |
| `BROM_PRESET_BUSY` | Not connected yet, or no fresh view since reconnect |
| `BROM_PRESET_CAPACITY` | Too many requests in flight; try again |
| `BROM_PRESET_STALE` | The preset changed underneath the request |
| `BROM_PRESET_SUPERSEDED` | A newer load replaced it |
| `BROM_PRESET_NOT_FOUND` | The selected preset's source is gone |
| `BROM_PRESET_INVALID` | The data failed validation; nothing changed |

A save that finishes durably refreshes the catalog whether or not its metadata
acknowledgment still matches the current preset. A refresh failure keeps the
previous catalog and sets a status the product can offer to retry.

## Browsers and the host program list

* A **browser** pin (`browser_open`) keeps the rows it opened with, whatever refresh
  does afterwards. Selecting a row resolves that row's identity, never a fresh row
  lookup, and fails with `BROM_PRESET_NOT_FOUND` rather than loading a different
  preset. Sixteen pins are available; nothing is evicted. The controller releases
  the pins it opened when it disconnects.
* A **program mapping** is a separately retained, fixed list for the host's program
  parameter, limited to its first **128** entries. Its count and numbering are fixed
  for the instance's lifetime: one entry normalizes to 0, several to
  `index / (count - 1)`. A host value decodes with the build's program rule, the
  VST3 rule `round(value * (count - 1))` by default and the AU rule behind the AUv2
  wrapper (see below); non-finite values are rejected.
* The controller prepares the mapping's snapshots off the audio thread and sends it
  in pages of at most 16. The processor holds a copy, so a host program event is
  resolved at its sample offset with no controller round trip and no file access.
  Until a complete list has arrived a program value returns `BROM_PRESET_BUSY` and
  changes nothing.
* A refresh may send the same slots again with new availability or content. The
  processor swaps in the new immutable list atomically and frees the old one in
  `pump()` once no audio callback can still be reading it. A slot whose source is
  gone stays in place as `BROM_PRESET_NOT_FOUND`.
* **A different slot set is not the same list.** New presets appear in the browser
  at once but become program slots only when a new mapping is created, which happens
  at the next connection (instance recreation or a host rescan). Live structural
  program-list refresh is not supported.

## Connection lifecycle

* Each connection has an epoch from `allocateConnectionEpoch()`: a process-wide
  counter, never derived from object addresses, and increasing: a welcome with a
  lower epoch than one already adopted is never adopted late. Messages from another
  epoch are ignored, so two instances (or a reconnected one) cannot publish into
  each other.
* A sender retries a message until `notify` reports success, and the pinned
  `ConnectionProxy` (and the AU wrapper) return the receiver's own result. A receiver
  therefore returns `kResultOk` for anything it will never accept (stale, from an
  older connection, undecodable) and drops it, and `kResultFalse` only for "not
  ready yet": a hello is refused while the processor has no peer, so the controller
  keeps it and retries.
* Each view carries its publication sequence and a revision. A re-send of the same
  publication (after a hello or a synchronous inactive change) raises only the
  revision, so a later real publication always orders after it.
* Until the handshake finishes, and after a reconnect until a fresh view arrives,
  controller submissions return `BROM_PRESET_BUSY`.
* `disconnect` cancels the controller's pending receipts as `BROM_PRESET_STALE`, drops
  queued messages and releases its browser pins. The processor's committed DSP and
  session state are untouched. A cancelled request may still have committed on the
  processor, so after reconnecting the fresh view is authoritative.
* `acknowledge(request)` releases a terminal receipt on both sides. A late duplicate
  of a command whose record was reclaimed is refused by a per-connection sequence
  floor, so storage is bounded without ever replaying an old command.
* Destruction waits for calls already in flight, including a `pump()` that is inside
  `notify`, and discards queued traffic without sending it. Stop processing and
  deliver no further messages before destroying an endpoint.

## Bounds

| Limit | Value |
| --- | --- |
| Parameters per schema | 256 |
| Message size | 65536 bytes |
| Session payload | 16384 bytes |
| Pending controller requests | 32 |
| Transaction queue | 32 by default |
| Browser pins | 16 |
| Program slots | 128 |
| Catalog page | 16 entries |

At a limit the new operation is refused; accepted work is never dropped.

## Linking it

```cmake
include(<pokuston>/packages/recoil-brom/cmake/BromPresetVst3.cmake)
brom_add_preset_vst3_support(TARGET brom_preset_support
    POKUSTON_ROOT <pokuston checkout> VST3_SDK_TARGET sdk)
brom_attach_preset_vst3_support(my_plugin_vst3 brom_preset_support)
```

`brom_add_preset_vst3_support` creates one position-independent static library with
C11 and C++17 requirements. It compiles `transaction.c`, `catalog.c`,
`catalog_service.c`, `save.c`, `session.c`, `preset_protocol.cpp` and
`preset_bridge.cpp` once, however many plug-ins attach to it, and a second call with
the same target is a no-op. Symbols are hidden, so a plug-in exports only what the
SDK's export list names. It links the SDK target, `m` and the platform thread
library, takes its architecture and optimization from the consumer's graph, and
changes no SDK source.

`tests/fixtures/brom-preset-vst3/` is a worked example: two synthetic plug-ins
attach to one support target, and a host loads both bundles.

## Behind the AUv2 wrapper

The pinned wrapper (`public.sdk/source/vst/auwrapper/auwrapper.mm`, VST3 SDK 3.8.1,
with Pokuston's overlay of its CMake module) embeds the VST3 plug-in inside the
component and drives it as a host would. This is what it actually does, read from the
source and confirmed by `Brom presets: AU bridge`:

* **Connection and state.** At initialization it connects the controller and the
  processor directly (no proxy), then writes the processor's state and hands it to
  the controller's `setComponentState`. So `setComponentState` must be implemented
  and cheap, and the endpoints must tolerate being connected before any editor
  exists. Saving reads `getState` on both; restoring calls the processor's
  `setState`, then the controller's `setComponentState` and `setState`.
* **Programs.** Factory presets are the controller's **first `IUnitInfo` program
  list**, enumerated **once**, at `Initialize`. The list needs a unit that names it
  and a parameter flagged `kIsProgramChange` in that unit. There is no path that
  re-enumerates it.
* **Notifications.** `restartComponent` handles `kParamValuesChanged`,
  `kParamTitlesChanged`, `kLatencyChanged` and `kMidiCCAssignmentChanged`, with a
  source comment saying the rest is unfinished. A structural change to the program
  list is therefore **not** picked up while the unit is initialized; it needs a new
  instance. This matches the pinned-mapping rule above, and makes the "new
  presets appear as program slots only at the next mapping" boundary a real one for
  AU.
* **Activation.** On an offline-mode change it calls `setProcessing(false)` and then
  `setActive(false)`, reconfigures, and calls `setActive(true)` then
  `setProcessing(true)`: the same quiescence order `setActive` relies on.

### Program values behind the AUv2 wrapper

A Program Change is an unsigned integer. The floats come from the layers that turn it
into a VST3 parameter value, and they disagree. A VST3 host sends **`index / (N - 1)`**
(the SDK's `ToNormalized` for a list parameter with `stepCount = N - 1`). The pinned
wrapper's factory-preset menu (`NewFactoryPresetSet`) sends **`(float)i / (float)N`**
in single precision and decodes a value it is set with as `floor(value * N)`
(`SetParameter`). A plug-in decoding the VST3 way therefore selects the preset before
the intended one for every preset in the **upper half** (every `i` with `2i > N`):
for `N = 10`, presets 6 to 9 are wrong, not only the last.

One AU instance can receive both encodings: the menu's `i / N`, and any value
`SetParameter` forwards, such as a controller's own `performEdit(program, i / (N - 1))`
or host automation. Brom therefore has two program rules in `preset/program_map.h`,
chosen when the binary is compiled:

* **VST3 rule (the default):** `floor(value * (count - 1) + 0.5)`. Every VST3 and CLAP
  build uses it, and it decodes exactly as it always did.
* **AU rule:** `min(count - 1, floor(value * count + 1/1024))`. It is the SDK's list
  decode plus a bias that absorbs the wrapper's single-precision rounding
  (`(float)7 / (float)10` is 6.9999999 times ten), and it still accepts `i / (N - 1)`.
  Both encodings of slot `i` lie in `[i/N, (i+1)/N)`, the bias is above the float
  error and below the VST3 margin, and the argument is proven for 512 slots (the cap
  is 128). It is selected by `BROM_PRESET_PROGRAM_AU_WRAPPER`.

The AU component embeds a VST3 module built with the AU rule; the plain VST3 of the
same product is built with the VST3 rule. In CMake this is
`brom_add_preset_vst3_support(... AU_WRAPPER)` for a product on the support library, or
`brom_vst3_program_rules(... AU_WRAPPER)` for one that includes `program_map.h` from
its own source, as the Kapralova, Loos and Mach VST3 targets do. A binary uses exactly
one flavor, and the configure step enforces it: attaching one target to both flavors,
or wrapping a target as an AU that is not the AU flavor (`brom_assert_au_flavor`),
fails. A bundle carries its flavor as a string a stripped binary keeps:
`strings <module> | grep BROM_PROGRAM_FLAVOR=`.

The AU rule gives up round-to-nearest's half-step tolerance on the `N - 1` grid: a
value that was quantized coarsely before it reached Brom (7- or 8-bit controller
steps) can select the slot below. That only concerns AU builds, and a real Program
Change is an integer and never takes this path. VST3 and CLAP builds are untouched.

Things this does not fix, and a product should know:

* The wrapper's own `SetParameter` bookkeeping (`(int)((float)v * (float)N)`) names the
  wrong "current preset" for some exact `(float)i / (float)N` writes (282 pairs for
  `N <= 128`, the first at `N = 22, i = 13`) and leaves it stale for a host-written
  `i / (N - 1)` at the last slot. Brom selects the right preset; only the name the AU
  host shows can lag.
* `presetNumber == N` makes the wrapper send `1.0` without setting a current preset;
  Brom selects the last slot.
* The wrapper exposes the program parameter as a continuous 0..1 control and never
  sets `kAudioUnitParameterFlag_ValuesHaveStrings`, so many AU hosts will not ask for
  its value strings. `PresetProgramParameter` keeps the strings equal to the sound for
  hosts and editors that do.
* **MIDI program change** through the wrapper needs the program parameter to be
  **parameter index 0**: `updateProgramChangesCache` reads the step count of the
  parameter at index 0 (its local `paramInfo` shadows the argument), so with another
  parameter first it sends `0 / 0`, which Brom rejects. It also needs
  `getUnitByBus` to map the event bus to the program unit. Treat it as unsupported
  otherwise.
* A `NewFactoryPresetSet` that arrives before the program table does is dropped as
  `BROM_PRESET_BUSY` (inferred from `audioSelectProgram`; the Reaper check below
  confirmed the behaviour for the VST3 plug-in only, not through the AU wrapper), yet
  the wrapper still calls `SetAFactoryPresetAsCurrent` and `setParamNormalized` and
  never re-sends, so the AU host shows that preset as current while the old sound
  plays.

The three shipping instruments (Kapralova, Loos, Mach) carried private copies of the
VST3 decode, so their AUs selected the upper-half factory presets one slot low. They now
take the rule from `program_map.h`, and their AU builds embed the AU-flavor module
(`<Name>-au-embedded.vst3`, a package-name suffix the driver never installs or
packages). A product-level AU load was not run: it needs an installed component.

## Adoption checklist

For a product plan that moves an instrument onto these services:

1. Attach `brom_add_preset_vst3_support` / `brom_attach_preset_vst3_support` to the
   VST3 target. The VST3 target an AU component embeds attaches to a support library
   created with `AU_WRAPPER`; every other VST3 and CLAP target uses the default. A
   plug-in links exactly one, through the attach helper (a target linked to a support
   library directly bypasses the flavor record).
2. Supply the schema, the preparation callbacks that turn a catalog identity into a
   validated snapshot, the parameter conversion, and the DSP callbacks
   (`apply`, `hardReset`, `clearNotes`).
3. Create the endpoints in `initialize()`, give them a message factory from the host
   application, and forward `connect`, `disconnect` and `notify` to them. Use
   `allocateConnectionEpoch()` for the processor's epoch.
4. Call `setActive` from `setActive`, and use `processTimeline` with a final callback
   that calls `finishTimeline()`. Route host parameter events to `audioEdit` and the
   program parameter to `audioSelectProgram`.
5. Implement `getState` and `setState` with `writePresetSession` and
   `readPresetSession` plus `restore`, and `setComponentState` with
   `setComponentView`. Keep any legacy decoder as a callback that yields a validated
   view.
6. Create the controller's browser pins through `openBrowser`, load rows with
   `submitBrowserLoad`, and publish a program list with `buildPresetProgramTable` and
   `publishProgramTable` once the controller is ready.
7. Start a main-thread timer in `initialize()` that calls `pump()` on both endpoints
   and does not depend on an editor.
8. Refresh the catalog through `brom_preset_catalog_service_saved` after a durable
   save, and show receipt results as above.
9. Use `PresetProgramParameter` (or `brom_program_index`) for the host program
   parameter, so the name shown is the preset that sounds.
10. Decide how a host that echoes refreshed values is handled (see the echo rule).
11. Validate in a real host. The synthetic fixtures prove the services, not an
    instrument.
12. Take the program rules from `program_map.h`, not a private copy. A product that
    includes it from its own source calls `brom_vst3_program_rules` so that source is
    compiled with `-fno-finite-math-only` (the SDK builds with `-ffast-math`, which
    makes the compiler fold every NaN test; the header refuses to compile otherwise).
13. The `IUnitInfo` program list, the program parameter's `stepCount + 1` and the
    published table's slot count must be equal: the wrapper encodes with the list
    count and Brom decodes with the table count. Cap the list at
    `BROM_PRESET_PROGRAM_MAX_SLOTS` (128); a longer list against a 128-slot table
    mismatches completely.
14. For wrapper MIDI program change, make the program parameter parameter index 0.

## Verification record

Run on 2026-10-21 with `r3 -s ../recoil/rut.r3 --test-root <checkout> -j 1 -F
brom-presets.reb`:

* macOS 15.6.1, Xcode 26.2 (17C52), CMake 4.4.3, architecture **x86_64** (the
  architecture of the Rebol runner; nothing was built universal).
* Pokuston branch `feat/brom-vst3-preset-bridge` at the commit that adds this record,
  based on `203a0c7`; Recoil `1997dd69`.
* VST3 SDK 3.8.1 (sha256 `64965f1b74e0…`), AudioUnitSDK 1.4.0 (sha256
  `a3ac2f94e8df…`), both resolved from the product's package manifests.
* Result: `Run 10 tests: 10 passed, 0 failed, 0 errored, 0 skipped, 0 inconsistent`,
  no `FAIL` and no `INCOMPLETE` line. The AU check host prints `checks 11 failures 0
  incomplete 0`, including the program-value tests behind the wrapper.
* The catalog service fixture runs in three configurations: the VST3 flavor, the AU
  flavor, and the AU flavor under the SDK's shipping float flags (`-O3 -ffast-math
  -ffp-contract=fast -fno-finite-math-only`, running only the non-finite test).
* The synthetic bundles were built with ad hoc signing and loaded in-process; none
  was installed in a host plug-in directory. Kapralova, Loos and Mach were built as
  VST3 and AU through the shared driver with `--no-install`; their plain VST3 modules
  carry `BROM_PROGRAM_FLAVOR=vst3` and the modules their AU components embed carry
  `BROM_PROGRAM_FLAVOR=au`. No product component was loaded in a host.
* **Not covered by any suite:** the registrations build with Unix Makefiles, not the
  Xcode generator the driver uses on macOS, whose SDK flags are `-O3 -ffast-math
  -ffp-contract=fast`. Run under those flags, the VST3 host fixture fails three
  tests that rely on rejecting a NaN on the wire (`wire_rejects_truncation_nan_and_unknown_variant`,
  `bridge_invalid_newer_preserves_pending`,
  `refresh_uses_no_performedit_and_one_shot_echo_rule`), so Brom's wire-codec NaN
  checks are probably not effective in shipping macOS builds. The program rule is not
  affected (its header requires `-fno-finite-math-only`); the wire codec is a
  separate piece of work.

## Reaper check (2026-10-21)

The synthetic plug-in (now a sine at the played pitch, with its level set by the
loaded preset: Bright 0.41, Dark 0.37, Wide 0.225 of full scale before Reaper's
mono pan law) was installed, scanned and loaded by Reaper v7.65 (macOS 15.6.1). A
project with three notes and a step envelope on the program parameter was rendered
headlessly and analyzed (`rms`/`peak`/zero-crossing frequency per window):

* **Real-time render (`RENDER_1X 1`):** notes sound at the right pitch with silence
  between them, and program changes at 2 s and 4 s selected Dark and Wide (peaks
  0.185 and 0.1125 = half of 0.37 and 0.225). The program value Reaper sent at
  transport start, and one at 0.3 s, were **not** applied: the first note kept the
  initial level (0.025).
* **Full-speed offline render:** no program change took effect at all. The notes
  sounded at the initial level.

That check loaded the **VST3** plug-in (`VST3i: Brom Synthetic A (Pokuston)` in the
project files), not the AU, and its envelope points were authored at 0, 0.5 and 1.0.
It shows that Reaper forwards those values; it did not test what Reaper's own program
menu sends. The VST3 rule is unchanged, so it stands without a rerun.

Both are the documented rule that a program value is `BROM_PRESET_BUSY` and inert
until the copied list has crossed the connection. The list travels on the main-thread
pump, so it needs wall-clock time on a running main thread; a host that renders faster
than real time, or sends a program value at the first sample after load, outruns it.
Between 0.3 s and 2 s after the project loaded was enough here; it was not measured
more precisely. A product that must honour an initial program value should treat it
as pending and re-apply it once the list is ready, or populate the processor's list
without waiting for the controller. This is not solved by the bridge.
