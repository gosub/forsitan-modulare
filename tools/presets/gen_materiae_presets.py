#!/usr/bin/env python3
"""Write materiae's factory presets.

    python3 tools/presets/gen_materiae_presets.py

The bank is a tour of the module rather than a drum kit. Two of the twelve are
sounds -- kick and click, the two ends of the envelope -- and the other ten each
sit on one mechanism, with everything else near neutral so that mechanism is
what you hear. Emulating a classic kit is the one thing this module is bad at;
what it is for is the relationship between two square waves, and a preset is
worth a slot when it shows one of those relationships plainly. Every setting is
stated here the way it would be described out loud -- a pitch in Hz, a ratio by
name, a cutoff in Hz, a decay in seconds, an operator by name -- and converted
to knob positions on the way out through the same laws src/materiae.cpp uses to
read them back. Nothing in this file knows where a value sits on a knob.

The one setting that is not absolute is `grid`: the top of that knob is eight
times the *host* sample rate, so a given position means a slightly different
core rate at 44.1 kHz than at 48 kHz. The rates below are converted assuming
48 kHz, which is where they were auditioned.

`test/materiae_presets` loads what this writes back into a real module and
measures it.
"""
import json
import math
import os

SR = 48000.0        # the rate the grid settings below were chosen at

# param ids, in the order of the ParamId enum in src/materiae.cpp
(PITCH, RATIO, SHAPE, GRID, DIV, XMOD, TILT, DEST, RELATION, BLEND,
 CUTOFF, RESO, FILTER, GAIN, DECAY2, CURVE2, ATTACK, DECAY, CURVE,
 E2PITCH, E2REL, E2CUT, HIT) = range(23)
NPARAM = 23

# from materiae_dsp.hpp
BASE_HZ = 32.703
MIN_CUT, MAX_CUT = 20.0, 12000.0
GRID_MAX_MULT, GRID_MIN = 4.0, 250.0

FM, AM, BOTH = 0, 1, 2
LP, BP = 0, 1

# kRatios, in knob order
RATIOS = ["1:4", "1:3", "1:2", "2:3", "3:4", "1:1", "1:1 detuned", "5:4",
          "4:3", "sqrt2", "3:2", "5:3", "7:4", "2:1", "5:2", "e", "3:1",
          "pi", "4:1"]
# kOpNames, in knob order
OPS = ["and", "sum", "ring", "flip", "noise"]


def pitch(hz):
    return math.log2(hz / BASE_HZ)


def ratio(name):
    return float(RATIOS.index(name))


def relation(spec):
    """An operator by name, or a pair as ("and", "sum", 0.4) to sit between."""
    if isinstance(spec, tuple):
        a, b, f = spec
        return OPS.index(a) + f * (OPS.index(b) - OPS.index(a))
    return float(OPS.index(spec))


def div(n):
    return float(int(math.log2(n)))


def grid(hz):
    top, bot = math.log2(SR * GRID_MAX_MULT), math.log2(GRID_MIN)
    return (math.log2(hz) - top) / (bot - top)


def cutoff(hz):
    return math.log2(hz / MIN_CUT) / math.log2(MAX_CUT / MIN_CUT)


def seconds(s, base, span):
    return math.log2(s / base) / span


def attack(s):
    return seconds(s, 0.0002, 11.3)


def decay(s):
    return seconds(s, 0.005, 9.6)


def decay2(s):
    return seconds(s, 0.003, 9.4)


# menu state every preset takes unless it says otherwise
DEFAULT_DATA = {
    "freeRun": False, "env2Free": False, "trackCutoff": False,
    "freeRatio": False, "useVelocity": True,
    "phaseIdx": 0, "outputLevel": 1,
}

PRESETS = [
    # Two of these are sounds. The other ten are demonstrations: each one is
    # built around a single thing the module does that nothing else quite
    # does, with everything else left near neutral so that thing is what you
    # hear. A drum kit is what this module is worst at pretending to be.

    ("kick", {
        # the filter is the body: a near-self-oscillating sine at 62 Hz that
        # the square strikes, with the pitch envelope on top of it
        PITCH: pitch(48), RATIO: ratio("1:1"), RELATION: relation("sum"),
        BLEND: 0.35, CUTOFF: cutoff(62), RESO: 0.93,
        DECAY: decay(0.55), CURVE: 0.75, DECAY2: decay2(0.045), CURVE2: 0.9,
        E2PITCH: -1.0, E2CUT: 0.35,
    }),

    ("click", {
        # the other end of the envelope: twelve milliseconds of high square
        PITCH: pitch(2400), RATIO: ratio("7:4"), RELATION: relation("ring"),
        GRID: grid(21000), CUTOFF: cutoff(7000), RESO: 0.3,
        ATTACK: attack(0.0002), DECAY: decay(0.012), CURVE: 0.9,
        DECAY2: decay2(0.006), CURVE2: 0.6,
    }),

    ("precession", {
        # RATIO on an irrational. sqrt2 means the two oscillators never share a
        # period, so the pattern the operator reads out never comes back round
        # -- given a long enough decay to hear it not repeating
        PITCH: pitch(330), RATIO: ratio("sqrt2"), RELATION: relation("ring"),
        CUTOFF: cutoff(7000), RESO: 0.3,
        DECAY: decay(2.4), CURVE: 0.25, DECAY2: decay2(0.4), CURVE2: 0.5,
    }),

    ("eclipse", {
        # SHAPE. One pulse width opens as the other closes, and AND is high
        # only while they overlap, so the knob is setting how much of one disc
        # covers the other
        PITCH: pitch(150), RATIO: ratio("3:2"), SHAPE: 0.85,
        RELATION: relation("and"),
        CUTOFF: cutoff(4000), RESO: 0.4, GAIN: 0.2,
        DECAY: decay(0.5), CURVE: 0.5, DECAY2: decay2(0.1), CURVE2: 0.6,
    }),

    ("moire", {
        # the latch, whose duty cycle *is* the phase difference between the two
        # oscillators. At a 1% detune that difference sweeps a full cycle twice
        # a second, so the timbre slides continuously without a knob moving:
        # two regular grids overlaid slightly out of true
        PITCH: pitch(200), RATIO: ratio("1:1 detuned"), RELATION: relation("flip"),
        CUTOFF: cutoff(3000), RESO: 0.35,
        DECAY: decay(2.2), CURVE: 0.1, DECAY2: decay2(0.3), CURVE2: 0.5,
    }),

    ("lattice", {
        # GRID near the bottom. The relationship between the two oscillators is
        # quantized onto a 350 Hz time grid; every edge lands late by a
        # different amount and the fold-down of that is the whole timbre
        PITCH: pitch(220), RATIO: ratio("3:2"), RELATION: relation("ring"),
        GRID: grid(350),
        CUTOFF: cutoff(8000), RESO: 0.2,
        DECAY: decay(0.9), CURVE: 0.5, DECAY2: decay2(0.15), CURVE2: 0.6,
    }),

    ("undertow", {
        # DIV as a subharmonic operand, with no cross-modulation at all: the
        # operator is reading a 40 Hz A against a 320 Hz B, which is a
        # different relationship and not the same one an octave down
        PITCH: pitch(320), RATIO: ratio("5:4"), RELATION: relation("ring"),
        DIV: div(8),
        CUTOFF: cutoff(5000), RESO: 0.45, GAIN: 0.2,
        DECAY: decay(0.8), CURVE: 0.45, DECAY2: decay2(0.12), CURVE2: 0.6,
        E2PITCH: -0.3,
    }),

    ("ouroboros", {
        # XMOD full with TILT centred: each oscillator modulating the other at
        # full depth, both directions at once, which is the far end of the
        # continuum the module is built around
        PITCH: pitch(120), RATIO: ratio("sqrt2"), RELATION: relation("ring"),
        XMOD: 1.0, TILT: 0.0,
        CUTOFF: cutoff(6000), RESO: 0.4,
        DECAY: decay(1.5), CURVE: 0.3, DECAY2: decay2(0.35), CURVE2: 0.4,
        E2PITCH: -0.2,
    }),

    ("transit", {
        # env 2 on RELATION. The hit begins on the shift register and crosses
        # the operator list back down to ring as the envelope falls, so what
        # changes over the length of the note is not the timbre of an operator
        # but which operator is reading the pair
        PITCH: pitch(300), RATIO: ratio("7:4"), RELATION: relation("ring"),
        CUTOFF: cutoff(6000), RESO: 0.35,
        DECAY: decay(1.2), CURVE: 0.4, DECAY2: decay2(0.6), CURVE2: -0.2,
        E2REL: 1.0,
    }),

    ("chatter", {
        # the shift register: an 8-bit register clocked by A and fed from its
        # own top bit exclusive-or'd with B. Noise, but deterministic and tied
        # to the pitch, so it has a grain rather than a hiss
        PITCH: pitch(400), RATIO: ratio("pi"), RELATION: relation("noise"),
        XMOD: 0.3, TILT: -0.5,
        CUTOFF: cutoff(5000), RESO: 0.3, GAIN: 0.25,
        DECAY: decay(1.2), CURVE: 0.5, DECAY2: decay2(0.25), CURVE2: 0.6,
        E2CUT: 0.3,
    }),

    ("vigil", {
        # for the DRONE output, where env 1 never closes: everything is slow,
        # nothing settles, and the oscillators free-run so it does not restart
        # from the same place each time
        PITCH: pitch(110), RATIO: ratio("sqrt2"), SHAPE: 0.4,
        RELATION: relation(("ring", "flip", 0.2)), BLEND: 0.85,
        XMOD: 0.5, TILT: -0.3, DIV: div(2), GRID: grid(15000),
        CUTOFF: cutoff(1200), RESO: 0.65, GAIN: 0.2,
        DECAY: decay(3.0), CURVE: -0.2, DECAY2: decay2(1.2), CURVE2: -0.4,
        E2PITCH: -0.15, E2REL: 0.7, E2CUT: 0.3,
        "data": {"freeRun": True, "env2Free": True},
    }),

    ("slag", {
        # gain most of the way up and the grid most of the way down: the
        # saturator and the coarse clock doing the damage between them
        PITCH: pitch(90), RATIO: ratio("4:3"), RELATION: relation("and"),
        DIV: div(2), XMOD: 0.2, GRID: grid(1200),
        CUTOFF: cutoff(3000), RESO: 0.5, GAIN: 0.85,
        DECAY: decay(0.35), CURVE: 0.5, DECAY2: decay2(0.08), CURVE2: 0.8,
        E2PITCH: -0.6, E2CUT: 0.25,
    }),
]

# knob defaults, for everything a preset does not mention
DEFAULTS = {
    PITCH: 1.0, RATIO: 10.0, SHAPE: 0.5, GRID: 0.0, DIV: 0.0,
    XMOD: 0.0, TILT: 0.0, DEST: float(FM), RELATION: 1.0, BLEND: 1.0,
    CUTOFF: 0.65, RESO: 0.2, FILTER: float(LP), GAIN: 0.0,
    DECAY2: 0.35, CURVE2: 0.5,
    ATTACK: 0.0, DECAY: 0.5, CURVE: 0.5,
    E2PITCH: 0.0, E2REL: 0.0, E2CUT: 0.0, HIT: 0.0,
}

LIMITS = {
    PITCH: (-2.0, 7.0), RATIO: (0.0, 18.0), DIV: (0.0, 4.0),
    RELATION: (0.0, 4.0), DEST: (0.0, 2.0), FILTER: (0.0, 1.0),
    TILT: (-1.0, 1.0), CURVE: (-1.0, 1.0), CURVE2: (-1.0, 1.0),
    E2PITCH: (-1.0, 1.0), E2REL: (-1.0, 1.0), E2CUT: (-1.0, 1.0),
}


def build(spec):
    values = []
    for i in range(NPARAM):
        v = spec.get(i, DEFAULTS[i])
        lo, hi = LIMITS.get(i, (0.0, 1.0))
        if not lo - 1e-9 <= v <= hi + 1e-9:
            raise ValueError(f"param {i} = {v} outside [{lo}, {hi}]")
        values.append(round(min(max(v, lo), hi), 6))
    return values


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    version = json.load(open(os.path.join(repo, "plugin.json")))["version"]
    out = os.path.join(repo, "presets", "materiae")
    os.makedirs(out, exist_ok=True)
    for n, entry in enumerate(PRESETS, start=1):
        name, spec = entry[0], dict(entry[1])
        data = dict(DEFAULT_DATA)
        data.update(spec.pop("data", {}))
        preset = {
            "plugin": "forsitan",
            "model": "materiae",
            "version": version,
            "params": [{"value": v, "id": i} for i, v in enumerate(build(spec))],
            # the module's own menu state: every preset takes the defaults
            "data": data,
        }
        path = os.path.join(out, f"{n}_{name}.vcvm")
        with open(path, "w") as f:
            json.dump(preset, f, indent=2)
            f.write("\n")
        print(f"wrote {os.path.relpath(path, repo)}")


if __name__ == "__main__":
    main()
