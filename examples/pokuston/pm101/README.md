# PM-101

Sintezar PM-101, the phase-manipulation synthesizer Rebolek wrote in Rebol 2
in 2005-2006 ([blog post](https://rebol2.blogspot.com/2013/08/digital-synthesizer-pm-101.html)),
ported to a Brom instrument in Recoil.

The original rendered one sound at a time into a WAV buffer. The port keeps its
signal path and its controls, and makes it playable: eight voices, MIDI pitch,
and a sustain point for the envelopes.

## Signal path

Per voice, two identical **lines**, each:

1. **DCO** (PHiDO2): a cosine read at a bent phase. Wave 1 is one of seven
   waveforms (sawtooth, new sawtooth, square, pulse, saw-pulse, sine-sync,
   triangle). Wave 2 is none or one of the same seven. With a second wave set,
   the oscillator alternates between the two on each cycle (the original's
   octave modulation).
2. **DCW**: how far the phase is bent. At nought every waveform is a pure
   cosine.
3. **DPS**: phase shaper. The wave runs faster and stops at the end of its
   cycle, so it gets shorter while the period stays the same.
4. **Window**: none, sawtooth, triangle, trapezoid or saw-pulse, across the
   cycle (or across the pair of cycles when two waves alternate). A sine-sync
   wave under a window gives the CZ resonance waves.
5. **DCF**: a Chamberlin state-variable filter (none, low, band, high, notch).
6. **DCA**: the output level, clipped to -1..1.

Each line has six **ESE envelopes**: pitch (two octaves either way around 0.5),
DCW, DPS, cutoff, resonance and DCA. Each envelope has eight segments, set by
eight rates and nine levels. A rate of 0 lasts 1 ms and a rate of 1 lasts
about 16 s, on the original's `(rate + 1)^14 / 1000` second curve.

The **mixer** plays line 1, line 2, both mixed, their ring product, or
Mix-Ring (the ring product with line 2 added back). Line 2 can be shifted by
up to two octaves and detuned by up to a semitone.

**FinalCrusher** works on the voice sum: a frequency divider
(sample-and-hold), bit reduction, then a waveshaper with an adjustable
threshold.

## Parameters

| Ids | Group | Controls |
|---|---|---|
| 0-2 | Mixer | Mode, Line 2 Octave, Line 2 Detune |
| 3-5 | Pitch | Octave, Vibrato Rate, Vibrato Depth |
| 6 | Envelope | Sustain Step |
| 7-9 | FinalCrusher | Bits, Divider, Threshold |
| 10-11 | Master | Volume, Velocity |
| 12-20 | Line 1 | Wave 1, Wave 2, Window, DCW, DPS, Filter, Cutoff, Resonance, DCA |
| 21-122 | Line 1 *X* Env | Rate 1-8, Level 1-9, for Pitch, DCW, DPS, Cutoff, Resonance, DCA |
| 123-233 | Line 2 ... | The same as line 1 |

That is 234 parameters in all. Preset keys are the folded module and name, for
example `line-1-dcw-env-level-3`.

**Sustain Step** *n* (1-7) holds every envelope at level *n* + 1 while the key
is down. A note-off then plays segment *n* + 1 onwards; if the note is
released before the hold, the envelope jumps straight to that segment from
wherever it is. At 0 the envelopes run freely, as they did in the original,
and a note lasts as long as its DCA envelope. The default is 2.

## What is reconstructed

The source on the blog page is damaged where the post was converted, and the
mixer, ring modulator, detune and FinalCrusher code is not in it at all. The
following are reconstructions, and each one is marked in `core.rcl`:

- the new-sawtooth and triangle waveforms (only their constants could be read);
- the DPS scale of 6;
- the filter resonance. The original's damping formula divides by a knob that
  defaults to nought, so the port uses damping from 2 (no resonance) down to
  0.1 instead;
- the mixer modes, the line 2 octave and detune, the vibrato, and all of
  FinalCrusher, rebuilt from the documentation's descriptions.

The original's filter was only correct at 44.1 kHz. The port keeps its cutoff
curve at any sample rate and runs the filter at twice the host rate, so the
top of the cutoff range stays stable.

The original `.rps` sound files were not recovered, so the nine factory
presets in `presets.rcl` are new.

## Building

```sh
r3 -s examples/pokuston/pm101/clap/build.r3 --no-install
```

CLAP only for now, with no editor of its own: the host shows its generic
parameter view. VST3 and AUv2 need an instrument C++ bridge, as Loos has in
`vst3/source/loos_vst3.cpp`.

`pm101-test.rcl` checks the schema, voices, state round trip and factory bank;
each check prints `PASS` or `FAIL`.
