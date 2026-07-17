# textor

**One-knob loop weaver: two seconds of sound, endlessly rewoven, never
the same twice.**

*textor* is Latin for "weaver". The module is a clone of the **Fieldtone
Weaver Modular**, the "happy accidents" sampler: it captures two seconds
of audio and reweaves it into a hypnotic loop. Every movement of the
weave knob generates a completely new loop. Nothing saves, nothing
recalls, there is no way back. The original's algorithm is unpublished;
this engine is designed from its documented behavior and from signal
analysis of demo recordings (onset/tempo autocorrelation, loop-repeat
similarity, envelope, stereo and pitch statistics).

## The weave

Every weave rolls its own character:

- **its own tempo** — a step period from ~45 to ~300 ms, so consecutive
  rolls can be lazy or frantic; the ~16-step loop spans about 0.7–4.8 s.
- **three elements as woven strands** — each element gets one or more
  periodic strands, firing every N steps like a randomly-dialed clock
  divider, each strand forever replaying its own fragment of the buffer
  with fixed pitch, direction, length and position in the field.
- **a delayed spacey element** — one element per roll carries decaying
  delay repeats (~0.1–0.45 s, moderate feedback).
- **evolution** — the loop repeats recognizably but never exactly:
  per-fire timing jitter, probabilistic fires, and strands that
  occasionally re-pick their fragment, so the weave slowly drifts.
- **soft edges** — fragments rise and fall on asymmetric raised-cosine
  windows (attacks mostly in the 50–400 ms range); rhythm mode is
  choppier, texture mode smears.
- **a nearly mono field** — fragments sit close to center with a slow,
  gentle spatial drift per element, like the hardware's subtle
  spatialization (not per-grain ping-pong).
- **semitone-quantized pitch** — even in random mode, shifts land on
  whole semitones (measured on the hardware); root mode draws from
  sympathetic intervals (octaves, fifths, fourths).

The elements, each with a level knob and a gate output (gates fire even
with an empty buffer, so textor doubles as a random rhythm generator):

| element | character |
|---------|-----------|
| **warp** | long, sparse foundation strands (divisions 4–16) |
| **weft** | medium strands crossing it (divisions 1–4) |
| **fleck** | shorter, brighter accents an octave up (divisions 1–4, flightier) |

## The weave knob

Like the hardware, the big knob has three zones:

| zone | action |
|------|--------|
| full ccw | **reset** — erases the sample and stops the loom |
| low zone | **rec** — entering it starts a 2 s capture |
| the rest | **weave** — any movement reweaves a brand new loop |

When a capture completes on a silent loom, playback starts by itself
with a fresh weave, like the hardware.

True to the original's philosophy of impermanence, the sample is **not
saved with the patch** — only the weave itself (its seed) survives a
reload; record something new into it and the old loop structure returns
with new cloth.

## Controls

| control | function |
|---------|----------|
| **weave** | the knob (see above) |
| **warp / weft / fleck** | element levels (they also scale that element's delay tail) |
| **mode** | switch: **texture** (long overlapping smears, a morphing pad) or **rhythm** (short percussive fragments) |
| **pitch** | switch: **root** (sympathetic intervals) or **random** (random semitones) |
| **rec** | button: start a 2 s capture (red LED while recording). The loop keeps playing; when the capture completes the new sound replaces the old *inside the same weave* |

Flipping a switch re-renders the current weave in the new mode, so you
can audition the same loop as pad and as beat.

## Inputs and outputs

| jack | function |
|------|----------|
| **in** | audio to capture |
| **rec** | trigger: start a capture — clock it to keep replacing the sample while the loop plays |
| **weave** | rerolls on any voltage *change* (>0.5 V): a trigger works, and a stepped random voltage rerolls on every new step, like CV-ing the hardware's knob |
| **clk** | clock: paces the loom's steps externally (one step per pulse), keeping the random rhythms — and the gate outputs' clock-divider patterns — in time with the system |
| **g1 / g2 / g3** | 2 ms trigger per element fire (delay repeats don't fire gates) |
| **l / r** | stereo output |

## Context menu

- **Restart loop on every weave** — the hardware behavior: each reroll
  restarts the fresh loop immediately, so sweeping the knob sputters a
  rapid cascade of pattern beginnings (the characteristic Weaver scrub
  sound). Off by default: weaves then swap seamlessly in place, keeping
  the step position and letting running fragments ring out.

## Tips

- Hum your key note into a mic and capture it in **texture / root**
  mode: instant non-monotonous pad, sympathetic to your key.
- **rhythm / random** with a spoken phrase makes funky beats of
  unexpected content; send **g1** to a kick for reinforcement.
- Clock **rec** every few bars while feeding it a live source: the weave
  stays, the cloth keeps renewing — a texture that follows the music.
- A stepped random voltage into **weave** rerolls on every step; a slow
  LFO rerolls every time it drifts half a volt.
- Solo an element: all levels down except one. The delay lives on a
  different element each roll — hunt for the spacey one.

## Attribution

Behavioral clone inspired by the Fieldtone Weaver Modular (design: Hugh
Jones). The engine is original code, designed from public descriptions
and audio analysis of published demos; not affiliated with Fieldtone.
