#!/usr/bin/env python3
"""Write gradus's factory presets.

    python3 tools/presets/gen_gradus_presets.py

Each preset is one use case, stated in volts here and converted to param
positions on the way out: the step knobs are square-law (volts = 10 * x^2),
so nothing in this file has to know where a value sits on a knob.

A preset is eight rows of (volts, mode) plus the clip setting. Modes are
`add` (the sides step the output by the row) and `jump` (the sides send it
straight to plus or minus the row).
"""
import json
import math
import os

# param ids, from src/gradus.cpp
STEP, MODE, PLUS, MINUS = 0, 8, 16, 24
CLIP, RESET = 32, 33
NPARAM = 34

ADD, JUMP = 1, 0
UNI, BI5, BI10, NOCLIP = 0, 1, 2, 3

SEMI = 1.0 / 12.0        # one semitone in V/oct

# eight bits that sum to exactly 10V: 255 * (10/255)
LSB = 10.0 / 255.0

PRESETS = [
    ("scale", dict(  # trigger-addressed melody: a row per scale degree
        clip=BI10, mode=JUMP,
        rows=[0.0, 2 * SEMI, 4 * SEMI, 5 * SEMI,
              7 * SEMI, 9 * SEMI, 11 * SEMI, 12 * SEMI])),

    ("arpeggio", dict(  # the same, on chord tones: a minor 7th over two octaves
        clip=BI10, mode=JUMP,
        rows=[0.0, 3 * SEMI, 7 * SEMI, 10 * SEMI,
              1.0, 1 + 3 * SEMI, 1 + 7 * SEMI, 1 + 10 * SEMI])),

    ("transpose", dict(  # relative intervals on top, absolute octaves below
        clip=BI10,
        rows=[(1.0, ADD), (7 * SEMI, ADD), (SEMI, ADD),
              (1.0, JUMP), (2.0, JUMP), (3.0, JUMP), (4.0, JUMP), (5.0, JUMP)])),

    ("binary", dict(  # eight gates in one sample are an eight-bit number
        clip=UNI, mode=ADD,
        rows=[LSB * (1 << i) for i in range(8)])),

    ("drift", dict(  # a drunk walk with two rails to snap back to
        clip=BI5,
        rows=[(0.05, ADD), (0.083, ADD), (0.12, ADD), (0.2, ADD),
              (0.35, ADD), (0.6, ADD), (2.5, JUMP), (4.5, JUMP)])),

    ("fader", dict(  # played by hand: three step sizes and five levels
        clip=UNI,
        rows=[(0.1, ADD), (0.5, ADD), (1.0, ADD),
              (2.0, JUMP), (4.0, JUMP), (6.0, JUMP), (8.0, JUMP), (10.0, JUMP)])),
]


def knob(volts):
    """Volts to knob position, the inverse of the module's square law."""
    return round(math.sqrt(min(max(volts, 0.0), 10.0) / 10.0), 6)


def build(spec):
    rows = spec["rows"]
    default_mode = spec.get("mode")
    values = [0.0] * NPARAM
    for i, row in enumerate(rows):
        volts, mode = row if isinstance(row, tuple) else (row, default_mode)
        values[STEP + i] = knob(volts)
        values[MODE + i] = float(mode)
    values[CLIP] = float(spec["clip"])
    return values


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    version = json.load(open(os.path.join(repo, "plugin.json")))["version"]
    out = os.path.join(repo, "presets", "gradus")
    os.makedirs(out, exist_ok=True)
    for n, (name, spec) in enumerate(PRESETS, start=1):
        preset = {
            "plugin": "forsitan",
            "model": "gradus",
            "version": version,
            "params": [{"value": v, "id": i} for i, v in enumerate(build(spec))],
            "data": {"value": 0.0},
        }
        path = os.path.join(out, f"{n}_{name}.vcvm")
        with open(path, "w") as f:
            json.dump(preset, f, indent=2)
            f.write("\n")
        print(f"wrote {os.path.relpath(path, repo)}")


if __name__ == "__main__":
    main()
