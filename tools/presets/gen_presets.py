#!/usr/bin/env python3
"""Write scrupea's factory presets.

    python3 tools/presets/gen_presets.py

These are written here, not lifted: Skrewell's own snapshots are Native
Instruments' content and none of them is in this repository (there is a
converter, `skrewell_to_scrupea.py`, for anyone who owns a copy). What was
taken from them is the *shape* of a Skrewell bank, measured over 44 of them:

  - every row is wide. The mean spread within a row is 0.22, against 0.29 for
    a uniform draw, and between 17 and 28 of the 44 banks have a row spanning
    more than 0.7 of its range. Nothing is drawn as a neat curve.
  - `pitch` sits lower than the rest (mean 0.36 against 0.44-0.50), so the
    carriers are low and the brightness comes from the FM.
  - `fm` is the one row that is ever flat -- 11 of 44 point every voice at the
    same modulator, which is a star rather than a ring, and it sounds quite
    different from a scattered row.
  - the four macros sit near the middle, medians 0.41 to 0.53.

Each preset below is a deliberate point in that space rather than an average
of it, since the average of forty-four banks is no bank at all.
"""
import json
import os

NCH, NFUNC = 8, 8
CH = 0
FUNC, MODE, EDIT = 64, 65, 66
PITCH, CUTOFF, DELAY, FLOW = 67, 68, 69, 70
LEVEL = 75
MORPH_TIME = 78
SCOPE_X, SCOPE_Y = 79, 80
NPARAM = 81

# rows are pitch, fm, amp, am, cutoff, type, time, fbk
PRESETS = [
    # ---------------------------------------------------------------------
    ("stagnum", dict(  # a standing pool: the calm end, and the one that wanders
        mode=0, pitch=0.42, cutoff=0.50, delay=0.62, flow=0.18, level=7.0,
        rows=[
            [0.30, 0.24, 0.35, 0.27, 0.33, 0.22, 0.37, 0.28],  # pitch, close
            [0.30, 0.22, 0.36, 0.18, 0.28, 0.34, 0.24, 0.32],  # fm
            [0.72, 0.66, 0.78, 0.70, 0.74, 0.62, 0.80, 0.68],  # amp
            [0.26, 0.34, 0.20, 0.38, 0.30, 0.24, 0.36, 0.28],  # am
            [0.55, 0.41, 0.63, 0.47, 0.59, 0.37, 0.67, 0.51],  # cutoff
            [0.20, 0.55, 0.10, 0.70, 0.30, 0.90, 0.15, 0.45],  # type
            [0.74, 0.87, 0.70, 0.94, 0.81, 1.00, 0.77, 0.90],  # time, long
            [0.95, 0.91, 0.99, 0.88, 0.97, 0.92, 1.00, 0.89],  # fbk, near unity
        ])),
    # ---------------------------------------------------------------------
    ("grando", dict(  # hail: short loops, so the combs ring rather than echo
        mode=0, pitch=0.55, cutoff=0.62, delay=0.14, flow=0.72, level=0.0,
        rows=[
            [0.71, 0.18, 0.94, 0.42, 0.63, 0.07, 0.86, 0.33],
            [0.62, 0.11, 0.88, 0.29, 0.55, 0.94, 0.17, 0.71],
            [0.88, 0.51, 0.97, 0.34, 0.79, 0.62, 0.91, 0.46],
            [0.13, 0.77, 0.35, 0.92, 0.05, 0.58, 0.81, 0.26],
            [0.83, 0.36, 0.95, 0.61, 0.74, 0.19, 0.88, 0.52],
            [0.05, 0.92, 0.31, 0.67, 0.14, 0.79, 0.43, 0.88],
            [0.11, 0.26, 0.05, 0.34, 0.18, 0.09, 0.29, 0.15],
            [0.93, 0.78, 1.00, 0.85, 0.96, 0.72, 0.99, 0.81],
        ])),
    # ---------------------------------------------------------------------
    ("unum", dict(  # one: the star topology, every voice modulated by voice 3
        mode=1, pitch=0.48, cutoff=0.44, delay=0.47, flow=0.55, level=2.0,
        rows=[
            [0.12, 0.87, 0.41, 0.63, 0.05, 0.74, 0.29, 0.96],  # pitch, wide
            [0.28, 0.28, 0.28, 0.28, 0.28, 0.28, 0.28, 0.28],  # fm, all -> 3
            [0.64, 0.71, 0.58, 0.83, 0.69, 0.55, 0.77, 0.62],
            [0.28, 0.28, 0.28, 0.28, 0.28, 0.28, 0.28, 0.28],  # am, all -> 3
            [0.09, 0.68, 0.37, 0.91, 0.22, 0.55, 0.79, 0.44],
            [0.71, 0.14, 0.88, 0.33, 0.62, 0.05, 0.95, 0.47],
            [0.55, 0.38, 0.72, 0.44, 0.61, 0.33, 0.68, 0.50],
            [0.84, 0.91, 0.77, 0.96, 0.88, 0.72, 0.93, 0.80],
        ])),
    # ---------------------------------------------------------------------
    ("vitrum", dict(  # glass: high and thin, highpassed, barely recirculating
        mode=1, pitch=0.74, cutoff=0.80, delay=0.35, flow=0.40, level=10.0,
        rows=[
            [0.68, 0.91, 0.55, 0.83, 0.74, 0.97, 0.61, 0.88],
            [0.44, 0.17, 0.72, 0.05, 0.61, 0.88, 0.33, 0.55],
            [0.51, 0.44, 0.62, 0.38, 0.57, 0.33, 0.66, 0.41],
            [0.72, 0.55, 0.88, 0.33, 0.61, 0.94, 0.17, 0.77],
            [0.82, 0.94, 0.71, 0.88, 0.77, 0.99, 0.85, 0.91],
            [0.88, 0.94, 0.79, 1.00, 0.85, 0.91, 0.97, 0.82],  # type, high
            [0.28, 0.41, 0.19, 0.47, 0.33, 0.24, 0.44, 0.36],
            [0.44, 0.31, 0.55, 0.26, 0.48, 0.38, 0.59, 0.35],  # fbk, modest
        ])),
    # ---------------------------------------------------------------------
    ("limus", dict(  # silt: everything low and slow, the sludge at the bottom
        mode=0, pitch=0.56, cutoff=0.66, delay=0.72, flow=0.33, level=4.0,
        rows=[
            [0.44, 0.58, 0.36, 0.52, 0.48, 0.39, 0.55, 0.46],
            [0.55, 0.22, 0.71, 0.38, 0.61, 0.17, 0.83, 0.44],
            [0.83, 0.77, 0.91, 0.69, 0.86, 0.74, 0.95, 0.80],
            [0.41, 0.62, 0.28, 0.74, 0.35, 0.57, 0.19, 0.66],
            [0.24, 0.38, 0.15, 0.44, 0.29, 0.11, 0.35, 0.21],
            [0.31, 0.48, 0.22, 0.39, 0.27, 0.53, 0.18, 0.44],  # type, low-band
            [0.88, 0.96, 0.82, 1.00, 0.91, 0.85, 0.98, 0.93],  # time, longest
            [0.97, 0.92, 1.00, 0.95, 0.99, 0.90, 0.98, 0.94],
        ])),
    # ---------------------------------------------------------------------
    ("chorda", dict(  # a string: pitches on a ramp, so the bank is a chord
        mode=2, pitch=0.50, cutoff=0.50, delay=0.44, flow=0.28, level=-3.0,
        rows=[
            [0.22, 0.31, 0.40, 0.48, 0.57, 0.66, 0.74, 0.83],  # pitch, a ramp
            [0.13, 0.25, 0.38, 0.50, 0.63, 0.75, 0.88, 0.00],  # fm, a cycle
            [0.80, 0.74, 0.68, 0.62, 0.56, 0.50, 0.44, 0.38],  # amp, a taper
            [0.88, 0.75, 0.63, 0.50, 0.38, 0.25, 0.13, 0.00],  # am, the reverse
            [0.50, 0.50, 0.50, 0.50, 0.50, 0.50, 0.50, 0.50],
            [0.50, 0.50, 0.50, 0.50, 0.50, 0.50, 0.50, 0.50],
            [0.66, 0.62, 0.58, 0.54, 0.50, 0.46, 0.42, 0.38],  # time, a ramp
            [0.92, 0.90, 0.88, 0.86, 0.84, 0.82, 0.80, 0.78],
        ])),
]


def build(spec):
    p = [0.0] * NPARAM
    for f, row in enumerate(spec["rows"]):
        for c in range(NCH):
            p[CH + f * NCH + c] = row[c]
    p[MODE] = float(spec["mode"])
    p[PITCH], p[CUTOFF] = spec["pitch"], spec["cutoff"]
    p[DELAY], p[FLOW] = spec["delay"], spec["flow"]
    p[LEVEL] = spec["level"]
    p[MORPH_TIME] = 0.5
    p[SCOPE_X] = p[SCOPE_Y] = 1.0
    return {"plugin": "forsitan", "model": "scrupea",
            "params": [{"id": i, "value": v} for i, v in enumerate(p)]}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "..", "presets", "scrupea")
    out = os.path.normpath(out)
    os.makedirs(out, exist_ok=True)
    for name, spec in PRESETS:
        assert all(len(r) == NCH for r in spec["rows"]) and \
            len(spec["rows"]) == NFUNC, name
        with open(os.path.join(out, name + ".vcvm"), "w") as fh:
            json.dump(build(spec), fh, indent=2)
            fh.write("\n")
    print(f"{len(PRESETS)} presets written to {out}")


if __name__ == '__main__':
    main()
