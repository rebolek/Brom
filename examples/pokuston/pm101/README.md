# PM-101

A real-time Brom port of Sintezar PM-101, the phase-manipulation (Casio CZ
style) synthesizer written in REBOL/View in 2005-06. Original:
<https://rebol2.blogspot.com/2013/08/digital-synthesizer-pm-101.html>.

**Status: written, never compiled.** The Recoil compiler was not available
when this was written, so nothing here has been built, linted or run. The
first thing to do is `core-test.rcl`; expect to fix syntax and type-inference
slips before it passes. The DSP was checked by reading it against the
original, not by listening to it.

## What it is

Four polyphonic voices. Each voice is a complete PM-101:

- **DCO1, DCO2** -- a phase counter running at the note frequency, scaled by
  the pitch envelope (+/- 2 octaves) and the shared vibrato. **DCW** bends the
  phase before a cosine reads it (the phase-distortion trick), **DPS**
  stretches the phase so the wave finishes early, and a **window** function
  fades the distortion across the cycle. Six waves (sawtooth, square, pulse,
  saw-pulse, triangle, sine-sync); wave B alternates with wave A on successive
  cycles (octave modulation). Each DCO has its own state-variable filter
  (low, band, notch, high) and DCA, and six envelopes: pitch, DCW, DPS, cutoff,
  resonance, DCA.
- **Mixer** -- line 1, line 2, mix, ring, ring2, mix-ring or noise; a second
  filter, DCA and three envelopes (cutoff, resonance, DCA); then the
  **FinalCrusher**: waveshaper, bit-depth reduction, frequency divider.
- **Global** -- detune for DCO2 (octave, semitone, cent), vibrato, and hard
  sync from DCO1 to DCO2.

213 parameters, all host-automatable. Envelopes have six stages (a rate and a
level each). Stages 1-5 run on note-on and hold at the end of stage 5;
note-off jumps to stage 6, the release. Rate `r` is `(r+1)^14` ms, 1 ms to 16 s.

## What changed from the original

Gone, because the host or Brom provides it: the on-screen keyboard and octave
switch, sample length, the offline render and play button, WAV and AIFF export,
the `.sin`/`.rps` presets and their file dialogs, the intro screen.

Fixed bugs the original recorded: square and triangle raised a negative base
to a fractional power (its missing #8); phase was not reset per note (#9).
Dropped: the fourth wave slot, which duplicated the square while the double
sine was pending, and the "1 + 1'" line selection, which was never connected.

**Six stages, not eight.** The original had eight. Eight stages of rate and
level for fifteen envelopes is 240 parameters, and Brom's descriptor holds 256
in total (`BROM-MAX-PARAMS`). `PM101_ENV_STAGES` and `STAGES` in
`tools/gen_*.py` are the numbers to change if that ceiling moves.

**No 2006 skin.** The teal-on-black look needs palette and knob styles in
`recoil-gui`, which lives outside this repository. The editor uses the stock
dark palette and `mach` / `kapralova-choice` knobs.

**CLAP only.** `plugin.reb` lists one format. VST3 and AUv2 need the same
C++ bridge, CMake project and Info.plist Loos carries under `vst3/` and `au/`.

## Layout

| File | |
|---|---|
| `core.rcl` | DSP, voices, parameter schema, state |
| `roles.rcl` | what kind of control each parameter is (shared by core and editor) |
| `plugin.rcl` | Brom engine descriptor, state and factory-bank glue |
| `param-names.rcl` | host-facing names -- **generated** by `tools/gen_param_names.py` |
| `editor.rcl` | VID editor, six tabbed pages -- **generated** by `tools/gen_editor.py` from `tools/editor.head` / `editor.tail` |
| `presets.rcl` | five factory patches |
| `core-test.rcl` | schema, silence/sound/release, depth-zero cosine, bounded output, reproducibility |
| `clap/` | CLAP project, entry and editor adapter |

Parameter ids are block base plus offset: DCO1 `0..80`, DCO2 `81..161`, mixer
`162..206`, global `207..212`. A DCO block is nine scalars then six envelopes
of twelve values (six rates, six levels); the mixer is nine scalars then three
envelopes.

## Build and test

Run from the repository root, with a Recoil checkout as in
`docs/user/plugin-builds.md`:

    r3 -s examples/pokuston/pm101/clap/build.r3 --no-install

`core-test.rcl` is windowless and compiles like Loos's `architecture-test.rcl`.

## Regenerating

    python3 examples/pokuston/pm101/tools/gen_param_names.py > examples/pokuston/pm101/param-names.rcl
    python3 examples/pokuston/pm101/tools/gen_editor.py      > examples/pokuston/pm101/editor.rcl
