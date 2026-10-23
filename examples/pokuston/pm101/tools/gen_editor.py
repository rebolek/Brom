#!/usr/bin/env python3
"""Generate editor.rcl: the PM-101 editor, with its layout block spelled out.

The layout is a literal VID block (the dialect parses it as written), so the
two hundred-odd control declarations are produced here from the same offsets
core.rcl uses rather than typed by hand. Run:

    python3 examples/pokuston/pm101/tools/gen_editor.py > examples/pokuston/pm101/editor.rcl
"""

DCO_PARAMS, MIX_BASE, GLOBAL_BASE = 81, 162, 207
STAGES = 6
EDIT = "on-edit 'edited on-default 'defaulted on-value 'reads on-format 'shown"
DCO_ENVS = ["PITCH", "DCW", "DPS", "CUTOFF", "RESONANCE", "DCA"]
MIX_ENVS = ["CUTOFF", "RESONANCE", "DCA"]
W, H = 760, 560


def knob(label, param, radius, style="mach"):
    return (f'        knob "{label}" radius {radius} style {style} param {param}\n'
            f'            {EDIT}')


def small_knob(param, style, cx, cy):
    return (f'        knob "" radius 13 style {style} param {param} centre {cx}x{cy}\n'
            f'            {EDIT}')


def row(x, y, items):
    return [f"        at {x}x{y} across"] + items


def dco_page(d):
    base = d * DCO_PARAMS
    out = [f'        at 24x120 text "DCO {d + 1}" font 14 colour accent',
           '        at 24x140 text "PHASE-DISTORTED OSCILLATOR LINE" font 9 colour muted']
    out += row(30, 176, [
        knob("WAVE A", base + 0, 30, "kapralova-choice"),
        knob("WAVE B", base + 1, 30, "kapralova-choice"),
        knob("WINDOW", base + 2, 30, "kapralova-choice"),
        knob("FILTER", base + 5, 30, "kapralova-choice"),
    ])
    out += row(30, 330, [
        knob("DCW", base + 3, 36),
        knob("DPS", base + 4, 36),
        knob("CUTOFF", base + 6, 36),
        knob("RESONANCE", base + 7, 36),
        knob("DCA", base + 8, 36),
    ])
    out.append('        at 24x470 text "DCW BENDS THE PHASE  DPS STRETCHES IT  WAVE B ALTERNATES WITH WAVE A EACH CYCLE" font 9 colour muted')
    return out


def mixer_page():
    b = MIX_BASE
    out = ['        at 24x120 text "MIXER" font 14 colour accent',
           '        at 24x140 text "LINES  FILTER  DCA  FINALCRUSHER" font 9 colour muted']
    out += row(30, 176, [
        knob("MODE", b + 0, 30, "kapralova-choice"),
        knob("FILTER", b + 1, 30, "kapralova-choice"),
        knob("CUTOFF", b + 2, 36),
        knob("RESONANCE", b + 3, 36),
        knob("DCA", b + 4, 36),
    ])
    out.append('        at 24x290 text "FINALCRUSHER" font 11 colour accent')
    out += row(30, 326, [
        knob("SHAPER", b + 5, 30, "kapralova-choice"),
        knob("KNEE", b + 6, 36),
        knob("BITS", b + 7, 30, "kapralova-choice"),
        knob("DIVIDER", b + 8, 30, "kapralova-choice"),
    ])
    out.append('        at 24x420 text "GLOBAL" font 11 colour accent')
    g = GLOBAL_BASE
    out += row(30, 456, [
        knob("OCTAVE", g + 0, 26, "kapralova-choice"),
        knob("NOTE", g + 1, 26, "kapralova-choice"),
        knob("CENT", g + 2, 26, "kapralova-choice"),
        knob("VIB RATE", g + 3, 26),
        knob("VIB DEPTH", g + 4, 26),
        knob("SYNC", g + 5, 26, "kapralova-choice"),
    ])
    return out


def env_page(title, first_param, names):
    out = [f'        at 24x120 text "{title}" font 14 colour accent',
           '        at 24x140 text "STAGES 1-5 RUN ON NOTE-ON AND HOLD   STAGE 6 IS THE RELEASE   RATE 1 MS TO 16 S" font 9 colour muted']
    pitch = 36
    left = 170
    # Knobs are placed on their centres, so the column heads line up exactly.
    for s_ in range(2 * STAGES):
        cx = left + s_ * pitch + (12 if s_ >= STAGES else 0)
        head = f"R{s_ + 1}" if s_ < STAGES else f"L{s_ - STAGES + 1}"
        out.append(f'        at {cx - 8}x160 text "{head}" font 9 colour muted')
    for i, name in enumerate(names):
        cy = 210 + i * 56
        base = first_param + i * (2 * STAGES)
        out.append(f'        at 24x{cy - 7} text "{name}" font 11 colour accent')
        for s_ in range(STAGES):
            out.append(small_knob(base + s_, "default", left + s_ * pitch, cy))
        for s_ in range(STAGES):
            out.append(small_knob(base + STAGES + s_, "mach", left + (STAGES + s_) * pitch + 12, cy))
    return out


pages = [
    ("DCO 1", dco_page(0)),
    ("DCO 2", dco_page(1)),
    ("MIXER", mixer_page()),
    ("ENV DCO 1", env_page("ENVELOPES  DCO 1", 9, DCO_ENVS)),
    ("ENV DCO 2", env_page("ENVELOPES  DCO 2", DCO_PARAMS + 9, DCO_ENVS)),
    ("ENV MIXER", env_page("ENVELOPES  MIXER", MIX_BASE + 9, MIX_ENVS)),
]

layout = [f"        panel 0x0 {W}x{H} colour panel [",
          '            at 24x20 text "POKUSTON" font 12 colour muted',
          '            at 24x36 text "PM-101" font 30 colour accent',
          '            at 24x78 text "PHASE MANIPULATION DIGITAL SYNTHESIZER" font 10 colour muted',
          '            at 470x30 button "<" 24x26 on-click \'preset-prev',
          '            at 650x30 button ">" 24x26 on-click \'preset-next',
          '            at 510x37 preset: text "" font 12 colour accent',
          '            at 24x96 tabs 712x26']
for title, body in pages:
    layout.append(f'            page "{title}" [')
    layout += ["    " + ln for ln in body]
    layout.append("            ]")
layout.append("        ]")

HEAD = open(__file__.replace("gen_editor.py", "editor.head")).read()
TAIL = open(__file__.replace("gen_editor.py", "editor.tail")).read()
print(HEAD + "\n".join(layout) + "\n" + TAIL)
