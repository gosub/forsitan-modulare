#!/usr/bin/env python3
"""Write spira's factory presets.

    python3 tools/presets/gen_spira_presets.py

Each preset is one way of getting past the plain stutter: a spiral, a lap
shape, a cloud. Values are stated in the units the panel shows (seconds,
ratios, Hz) and converted through the knob laws of src/spira/spira.hpp, so
they say what they do. A preset states every control and the menu's one
setting; HOLD is a performance state and is left as it is.
"""

import json
import math
import os

# param ids, from src/spira.cpp
(SIZE, SPIRAL, TAPE, FADE, RATE, PITCH, TONE, ANCHOR, JITTER, REACH, LINE,
 SHAPE, SOFT, DIRECTION, SPREAD, MIX, BIRTH, HOLD, SKIPS, LEVEL) = range(20)
NPARAM = 20

REVERSE, PINGPONG, FORWARD = 0.0, 1.0, 2.0     # the switch counts from the bottom

# the knob laws, as in spira.hpp
SIZE_MIN, SIZE_MAX = 0.01, 4.0
LINE_MIN, LINE_MAX = 1.0, 30.0
RATE_OFF, RATE_MIN, RATE_MAX = 0.02, 0.05, 20.0


def size(seconds):
    return math.log(seconds / SIZE_MIN) / math.log(SIZE_MAX / SIZE_MIN)


def line(seconds):
    return math.log(seconds / LINE_MIN) / math.log(LINE_MAX / LINE_MIN)


def spiral(ratio):
    e = math.log2(ratio)
    return -math.sqrt(-e) if e < 0 else math.sqrt(e)


def fade(db):
    return -math.sqrt(db / -60.0) if db < 0 else math.sqrt(db / 6.0)


def rate(hz):
    if hz <= 0:
        return 0.0
    return RATE_OFF + (1 - RATE_OFF) * math.log(hz / RATE_MIN) / math.log(RATE_MAX / RATE_MIN)


DEFAULTS = {
    SIZE: size(0.25), SPIRAL: 0.0, TAPE: 1.0, FADE: fade(-1.5), RATE: rate(0.5),
    PITCH: 0.0, TONE: 0.0, ANCHOR: 0.0, JITTER: 0.0, REACH: 0.0, LINE: line(8),
    SHAPE: 0.0, SOFT: 0.25, DIRECTION: FORWARD, SPREAD: 0.5, MIX: 0.5,
    BIRTH: 0.0, HOLD: 0.0, SKIPS: 0.0, LEVEL: 0.0,
}

PRESETS = [
    # Inward on tape: each repeat 0.94 as long and faster, rising into a
    # pitch that vanishes after about seven seconds.
    ("zip", {
        SIZE: size(0.4), SPIRAL: spiral(0.94), TAPE: 1.0, FADE: fade(0.0), RATE: rate(0.4),
        SOFT: 0.3, SPREAD: 0.6,
    }, {}),
    # Inward by cutting: a roll closing in on the end of its window, into a
    # buzz at the source's own pitch.
    ("roll", {
        SIZE: size(0.4), SPIRAL: spiral(0.94), TAPE: 0.0, ANCHOR: 1.0, FADE: fade(0.0),
        RATE: rate(0.4), SOFT: 0.1,
    }, {}),
    # Outward on tape: slower, lower and darker every turn.
    ("unwind", {
        SIZE: size(0.15), SPIRAL: spiral(1.13), TAPE: 1.0, FADE: fade(-1.0), RATE: rate(0.3),
        TONE: -0.7, SOFT: 0.5, SPREAD: 0.8,
    }, {}),
    # A tape stop: each lap 1.56 times longer and as much slower.
    ("brake", {
        SIZE: size(0.3), SPIRAL: spiral(1.56), TAPE: 1.0, FADE: fade(-2.0), RATE: rate(0.25),
        SOFT: 0.4, SPREAD: 0.4,
    }, {}),
    # Short soft laps, twelve new circles a second, scattered over the last
    # couple of seconds: a granular cloud drifting slowly inward.
    ("cloud", {
        SIZE: size(0.07), SPIRAL: spiral(0.98), SOFT: 1.0, RATE: rate(12), REACH: 0.3,
        JITTER: 0.6, SPREAD: 1.0, FADE: fade(-6.0), LINE: line(6),
    }, {}),
    # Plucked laps trading sides, closing in, born on a 2 Hz grid with holes
    # in it.
    ("plucks", {
        SIZE: size(0.25), SHAPE: -0.6, DIRECTION: PINGPONG, SPIRAL: spiral(0.97),
        FADE: fade(-1.0), RATE: rate(2), SKIPS: 0.6, SPREAD: 0.8, TAPE: 0.5,
    }, {}),
    # Reversed laps that swell and stop, a fifth up, thinner every turn.
    ("swells", {
        SIZE: size(0.6), SHAPE: 0.6, SOFT: 0.6, DIRECTION: REVERSE, TONE: 0.8,
        FADE: fade(-2.0), RATE: rate(0.5), PITCH: 7.0, SPREAD: 0.7,
    }, {}),
    # Circles from anywhere on the last four seconds, in ping-pong: press
    # HOLD and they wander a frozen loop.
    ("scatter", {
        LINE: line(4), REACH: 1.0, SIZE: size(0.3), RATE: rate(1.5), JITTER: 0.4,
        SOFT: 0.5, SPIRAL: spiral(0.95), FADE: fade(-2.0), DIRECTION: PINGPONG, SPREAD: 1.0,
        MIX: 0.6,
    }, {}),
]


def build(knobs):
    values = dict(DEFAULTS)
    values.update(knobs)
    return [round(float(values[i]), 6) for i in range(NPARAM)]


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    version = json.load(open(os.path.join(repo, "plugin.json")))["version"]
    out = os.path.join(repo, "presets", "spira")
    os.makedirs(out, exist_ok=True)
    for n, (name, knobs, menu) in enumerate(PRESETS, start=1):
        data = {"keepBirth": False}
        data.update(menu)
        preset = {
            "plugin": "forsitan",
            "model": "spira",
            "version": version,
            "params": [{"value": v, "id": i} for i, v in enumerate(build(knobs))],
            "data": data,
        }
        path = os.path.join(out, f"{n}_{name}.vcvm")
        with open(path, "w") as f:
            json.dump(preset, f, indent=2)
            f.write("\n")
        print(f"wrote {os.path.relpath(path, repo)}")


if __name__ == "__main__":
    main()
