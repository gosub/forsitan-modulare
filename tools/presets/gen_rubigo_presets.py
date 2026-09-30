#!/usr/bin/env python3
"""Write rubigo's factory presets.

    python3 tools/presets/gen_rubigo_presets.py

Each preset is one of the hardware manual's "try this" recipes, or one of
the archetypes doc/design/rubigo.md reads out of the Preset Book's
principles, written with our own values and names. None of them is a
Preset Book setting: those are Body Synths' content.

Knobs are 0..1 as on the panel. A preset states every knob and switch, and
the three menu settings that change the sound (effect, Mod assign, step
lengths), so loading one never inherits them from what was loaded before.
The locked loop is not part of a preset: it stays as it was.
"""

import json
import os

# param ids, from src/rubigo.cpp
(PITCH, WAVE, PITCH_DECAY, PITCH_AMOUNT, NOISE, CUTOFF, RES, FILTER,
 CUTOFF_DECAY, CUTOFF_AMOUNT, VOLUME, VOLUME_DECAY, EFFECT, RUST, MIX,
 STEPS, SKIPS, STEPMOD, DEST, TEMPO, RUN, TRIGGER) = range(22)
NPARAM = 22

SQUARE, SAW = 0.0, 1.0
LP, HP = 0.0, 1.0
CORROSION, RUSTED = 0.0, 1.0
TO_CUTOFF, TO_NOISE, TO_PITCH = 0.0, 1.0, 2.0      # the switch counts from the bottom
# STEPS detents: OFF 2 4 8 10 16 32, or OFF 3 5 7 12 18 24 with alt lengths
OFF, S2, S4, S8, S10, S16, S32 = range(7)
# menu
DISTORTION, OSC2, PHASER, FLANGER, CHORUS = range(5)
A_CUTOFF, A_VOL_DECAY, A_PITCH_AMOUNT, A_CUTOFF_AMOUNT, A_VOLUME, A_EFFECT = range(6)

DEFAULTS = {
    PITCH: 0.05, WAVE: SQUARE, PITCH_DECAY: 0.25, PITCH_AMOUNT: 0.0,
    NOISE: 0.0, CUTOFF: 0.35, RES: 0.0, FILTER: LP,
    CUTOFF_DECAY: 0.25, CUTOFF_AMOUNT: 0.0, VOLUME: 0.75, VOLUME_DECAY: 0.42,
    EFFECT: 0.0, RUST: CORROSION, MIX: 1.0,
    STEPS: OFF, SKIPS: 0.0, STEPMOD: 0.0, DEST: TO_PITCH, TEMPO: 0.3,
    RUN: 1.0, TRIGGER: 0.0,
}

PRESETS = [
    # The manual's kick: PITCH at the bottom, a short pitch decay with a
    # large amount, square. A little step mod on the pitch keeps it moving.
    ("kick", {
        PITCH: 0.0, PITCH_DECAY: 0.24, PITCH_AMOUNT: 0.72, CUTOFF: 0.3, RES: 0.05,
        VOLUME: 0.78, VOLUME_DECAY: 0.4, EFFECT: 0.0,
        STEPS: S16, SKIPS: 0.35, STEPMOD: 0.15, DEST: TO_PITCH, TEMPO: 0.31,
    }, {}),
    # "A changing sequence of kick and noise steps": NOISE at zero, the step
    # mod at full on NOISE.
    ("clatter", {
        PITCH: 0.05, PITCH_DECAY: 0.22, PITCH_AMOUNT: 0.7, CUTOFF: 0.55,
        VOLUME_DECAY: 0.38, EFFECT: 0.2,
        STEPS: S8, SKIPS: 0.2, STEPMOD: 1.0, DEST: TO_NOISE, TEMPO: 0.33,
    }, {}),
    # The resonant melody: cutoff and its envelope at zero, resonance full,
    # the step mod half way on CUTOFF.
    ("melody", {
        WAVE: SAW, PITCH: 0.1, CUTOFF: 0.0, RES: 1.0, VOLUME: 0.7, VOLUME_DECAY: 0.5,
        EFFECT: 0.3, STEPS: S16, SKIPS: 0.25, STEPMOD: 0.5, DEST: TO_CUTOFF, TEMPO: 0.34,
    }, {}),
    # "You just got a drone synth": volume decay and TEMPO full, no skips.
    ("drone", {
        WAVE: SAW, PITCH: 0.3321, CUTOFF: 0.0, RES: 0.6, VOLUME: 0.6,   # 110 Hz, 20 Hz
        VOLUME_DECAY: 1.0, EFFECT: 0.7, RUST: RUSTED,
        STEPS: OFF, SKIPS: 0.0, STEPMOD: 0.2, DEST: TO_CUTOFF, TEMPO: 1.0,
    }, {}),
    # Bass: resonant low-pass, a plucked cutoff envelope, pitch step mod.
    ("acid", {
        WAVE: SAW, PITCH: 0.1, CUTOFF: 0.2, RES: 0.85, CUTOFF_DECAY: 0.38,
        CUTOFF_AMOUNT: 0.7, VOLUME: 0.76, VOLUME_DECAY: 0.55, EFFECT: 0.15,
        STEPS: S16, SKIPS: 0.4, STEPMOD: 0.4, DEST: TO_PITCH, TEMPO: 0.37,
    }, {}),
    # Noise through a high-pass and a heavy downsampler, the cutoff stepped.
    ("static", {
        NOISE: 1.0, CUTOFF: 0.5, FILTER: HP, RES: 0.1, VOLUME: 0.66,
        VOLUME_DECAY: 0.3, EFFECT: 0.85, RUST: RUSTED,
        STEPS: S8, SKIPS: 0.5, STEPMOD: 0.6, DEST: TO_CUTOFF, TEMPO: 0.35,
    }, {}),
    # "The instrument behaves like a sound effect device": TEMPO, volume
    # decay and NOISE full. Patch something into IN; unpatched it is the
    # noise itself, swept by the steps and an intense phaser.
    ("wireless", {
        NOISE: 1.0, CUTOFF: 0.5, RES: 0.4, VOLUME: 0.72, VOLUME_DECAY: 1.0,
        EFFECT: 0.3, RUST: RUSTED,
        STEPS: OFF, SKIPS: 0.0, STEPMOD: 0.5, DEST: TO_CUTOFF, TEMPO: 1.0,
    }, {"effect": PHASER}),
    # The manual's click: a very short cutoff decay at full amount, with
    # resonance. Fast, sparse and long-looped.
    ("ticks", {
        PITCH: 0.3, CUTOFF: 0.25, RES: 0.6, CUTOFF_DECAY: 0.05, CUTOFF_AMOUNT: 1.0,
        NOISE: 0.2, VOLUME_DECAY: 0.15, VOLUME: 0.8, EFFECT: 0.5,
        STEPS: S32, SKIPS: 0.5, STEPMOD: 0.3, DEST: TO_PITCH, TEMPO: 0.45,
    }, {}),
    # Mod assign on volume decay: every step its own length, through a
    # subtle chorus.
    ("swell", {
        WAVE: SAW, PITCH: 0.12, PITCH_DECAY: 0.3, PITCH_AMOUNT: 0.4, CUTOFF: 0.3,
        RES: 0.4, VOLUME: 0.72, VOLUME_DECAY: 0.35, EFFECT: 0.2,
        STEPS: S16, SKIPS: 0.25, STEPMOD: 0.9, DEST: TO_CUTOFF, TEMPO: 0.22,
    }, {"effect": CHORUS, "assign": A_VOL_DECAY}),
    # The second oscillator an octave down under a stepped bass line, from
    # 97 Hz: lower, the octave under it falls below hearing.
    ("sub", {
        WAVE: SAW, PITCH: 0.3, CUTOFF: 0.3, RES: 0.5, CUTOFF_DECAY: 0.3,
        CUTOFF_AMOUNT: 0.4, VOLUME: 0.74, VOLUME_DECAY: 0.5, EFFECT: 0.0,
        STEPS: S16, SKIPS: 0.3, STEPMOD: 0.3, DEST: TO_PITCH, TEMPO: 0.36,
    }, {"effect": OSC2}),
    # A seven-step loop from the alternative lengths, flanged.
    ("septet", {
        PITCH: 0.02, PITCH_DECAY: 0.2, PITCH_AMOUNT: 0.65, NOISE: 0.5, CUTOFF: 0.45,
        VOLUME: 0.76, VOLUME_DECAY: 0.35, EFFECT: 0.4, RUST: RUSTED,
        STEPS: S8, SKIPS: 0.3, STEPMOD: 0.9, DEST: TO_NOISE, TEMPO: 0.35,
    }, {"effect": FLANGER, "altLengths": True}),
]


def build(knobs):
    values = dict(DEFAULTS)
    values.update(knobs)
    return [float(values[i]) for i in range(NPARAM)]


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    version = json.load(open(os.path.join(repo, "plugin.json")))["version"]
    out = os.path.join(repo, "presets", "rubigo")
    os.makedirs(out, exist_ok=True)
    for n, (name, knobs, menu) in enumerate(PRESETS, start=1):
        data = {"effect": DISTORTION, "assign": A_CUTOFF, "altLengths": False}
        data.update(menu)
        preset = {
            "plugin": "forsitan",
            "model": "rubigo",
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
