#!/usr/bin/env python3
"""Write materiae's factory presets.

    python3 tools/presets/gen_materiae_presets.py

One preset per sound family the module is meant to cover. The first eight are
named for what they are; the last four are named for what they are like, which
is a thread the module's own name started -- chaff, glint, vigil and slag are
all things made of matter. doc/materiae.md carries the table that says which is
the snare. Every setting is
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
    ("kick", {
        # the filter is the body: a near-self-oscillating sine at 62 Hz that
        # the square strikes, with the pitch envelope on top of it
        PITCH: pitch(48), RATIO: ratio("1:1"), RELATION: relation("sum"),
        BLEND: 0.35, CUTOFF: cutoff(62), RESO: 0.93,
        DECAY: decay(0.55), CURVE: 0.75, DECAY2: decay2(0.045), CURVE2: 0.9,
        E2PITCH: -1.0, E2CUT: 0.35,
    }),

    ("tom", {
        PITCH: pitch(120), RATIO: ratio("3:2"), RELATION: relation("sum"),
        BLEND: 0.5, CUTOFF: cutoff(420), RESO: 0.72,
        DECAY: decay(0.45), CURVE: 0.6, DECAY2: decay2(0.12), CURVE2: 0.5,
        E2PITCH: -0.8, E2CUT: 0.2,
    }),

    ("metal", {
        # an irrational ratio into ring: the two never share a period, so the
        # partials never line up and the hit keeps moving
        PITCH: pitch(620), RATIO: ratio("pi"), RELATION: relation("ring"),
        XMOD: 0.35, TILT: -0.4,
        CUTOFF: cutoff(3800), RESO: 0.55, FILTER: BP,
        DECAY: decay(1.1), CURVE: 0.8, DECAY2: decay2(0.2), CURVE2: 0.6,
        E2REL: 0.6,
    }),

    ("digital", {
        # the coarse grid is doing most of the work here, not the operator
        PITCH: pitch(260), RATIO: ratio("sqrt2"), RELATION: relation("and"),
        DIV: div(4), XMOD: 0.5, GRID: grid(6200),
        CUTOFF: cutoff(5200), RESO: 0.4,
        DECAY: decay(0.18), CURVE: 0.4, DECAY2: decay2(0.06), CURVE2: 0.6,
        E2PITCH: 0.5,
    }),

    ("click", {
        PITCH: pitch(2400), RATIO: ratio("7:4"), RELATION: relation("ring"),
        GRID: grid(21000), CUTOFF: cutoff(7000), RESO: 0.3,
        ATTACK: attack(0.0002), DECAY: decay(0.012), CURVE: 0.9,
        DECAY2: decay2(0.006), CURVE2: 0.6,
    }),

    ("cymbal", {
        # the shift register is the only route to noise that does not break the
        # two-sources premise
        PITCH: pitch(1500), RATIO: ratio("e"), RELATION: relation("noise"),
        XMOD: 0.6, TILT: 0.3,
        CUTOFF: cutoff(6500), RESO: 0.25, FILTER: BP,
        DECAY: decay(0.9), CURVE: 0.85, DECAY2: decay2(0.3), CURVE2: 0.6,
        E2CUT: 0.5,
    }),

    ("bass", {
        # unstable: the latch's duty cycle is the phase difference, and the two
        # oscillators are close enough to drift through it as the hit decays
        PITCH: pitch(55), RATIO: ratio("1:1 detuned"), RELATION: relation("flip"),
        BLEND: 0.8, XMOD: 0.45, TILT: -0.5, DIV: div(2), GRID: grid(9000),
        CUTOFF: cutoff(220), RESO: 0.85, GAIN: 0.45,
        DECAY: decay(0.9), CURVE: 0.35, DECAY2: decay2(0.35), CURVE2: 0.4,
        E2PITCH: -0.35, E2REL: 0.5, E2CUT: 0.3,
    }),

    ("strange", {
        # asymmetric feedback with the operator swept across the whole list by
        # env 2: the relationship is different at the start of the hit and the
        # end of it, which is the thing this module is for
        PITCH: pitch(74), RATIO: ratio("sqrt2"), SHAPE: 0.78,
        RELATION: relation(("and", "sum", 0.4)), BLEND: 0.9,
        XMOD: 0.85, TILT: -0.7, DIV: div(2), DEST: BOTH, GRID: grid(11000),
        CUTOFF: cutoff(900), RESO: 0.8, GAIN: 0.35,
        DECAY: decay(1.4), CURVE: 0.2, DECAY2: decay2(0.55), CURVE2: -0.3,
        E2PITCH: -0.5, E2REL: 1.0, E2CUT: 0.6,
    }),
    ("chaff", {  # a snare: dry husks beaten loose, a rattling scatter
        # noise pulled back toward the latch, so the rattle keeps a pattern in
        # it rather than being flat hiss, over a band-passed body
        PITCH: pitch(190), RATIO: ratio("7:4"), SHAPE: 0.62,
        RELATION: relation(("flip", "noise", 0.6)), XMOD: 0.3,
        CUTOFF: cutoff(1800), RESO: 0.45, FILTER: BP, GAIN: 0.25,
        DECAY: decay(0.22), CURVE: 0.7, DECAY2: decay2(0.05), CURVE2: 0.85,
        E2CUT: 0.4, E2PITCH: -0.25,
    }),

    ("glint", {  # a hat: one brief flash and gone
        # the short bright one the bank was missing: cymbal is nearly a second
        # long, this is forty-five milliseconds
        PITCH: pitch(3200), RATIO: ratio("e"), RELATION: relation("noise"),
        XMOD: 0.4, GRID: grid(28000),
        CUTOFF: cutoff(9000), RESO: 0.2, FILTER: BP, GAIN: 0.40,
        ATTACK: attack(0.0002), DECAY: decay(0.045), CURVE: 0.9,
        DECAY2: decay2(0.02), CURVE2: 0.7, E2CUT: 0.3,
    }),

    ("vigil", {  # a drone: a watch kept unbroken
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

    ("slag", {  # the coarse vitreous waste a furnace leaves behind
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
