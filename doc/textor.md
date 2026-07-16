# textor

**One-knob loop weaver: two seconds of sound, endlessly rewoven, never
the same twice.**

*textor* is Latin for "weaver". The module is a clone of the **Fieldtone
Weaver Modular**, the "happy accidents" sampler: it captures two seconds
of audio and reweaves it into a hypnotic loop. Every movement of the
weave knob generates a completely new loop. Nothing saves, nothing
recalls, there is no way back. The original's algorithm is unpublished;
this engine is designed from its documented behavior.

The captured sample is scattered over a 16-step loop as three woven
elements, each with its own level knob and gate output:

| element | character |
|---------|-----------|
| **warp** | long, dense foundation strands |
| **weft** | medium strands crossing it |
| **fleck** | sparse, short, bright accents |

Each strand is a fragment of the buffer with its own start point,
length, pitch shift, direction and stereo position, fixed for the life
of the loop — the loop repeats identically until the knob moves again.
The gates fire even with an empty buffer, so textor doubles as a random
rhythm generator with no sample loaded at all.

## The weave knob

Like the hardware, the big knob has three zones:

| zone | action |
|------|--------|
| full ccw | **reset** — erases the sample and stops the loom |
| low zone | **rec** — entering it starts a 2 s capture |
| the rest | **weave** — any movement reweaves a brand new loop |

True to the original's philosophy of impermanence, the sample is **not
saved with the patch** — only the weave itself (its seed) survives a
reload; record something new into it and the old loop structure returns
with new cloth.

## Controls

| control | function |
|---------|----------|
| **weave** | the knob (see above) |
| **warp / weft / fleck** | element levels |
| **mode** | switch: **texture** (long overlapping windowed strands, a morphing pad) or **rhythm** (short percussive fragments) |
| **pitch** | switch: **root** (fragments shifted by musical intervals — octaves, fifths, fourths — sympathetic to the sample's own key) or **random** (continuous random shifts) |
| **rec** | button: start a 2 s capture (red LED while recording). The loop keeps playing; when the capture completes the new sound replaces the old *inside the same weave* |

Flipping a switch re-renders the current weave in the new mode, so you
can audition the same loop as pad and as beat.

## Inputs and outputs

| jack | function |
|------|----------|
| **in** | audio to capture |
| **rec** | trigger: start a capture — clock it to keep replacing the sample while the loop plays |
| **weave** | trigger: reweave a new loop, a remote nudge of the knob |
| **clk** | clock: paces the 16 steps externally (one step per pulse); unpatched, steps run at 125 ms |
| **g1 / g2 / g3** | 2 ms trigger per element onset — the loop as a rhythm generator for the rest of the rack |
| **l / r** | stereo output, each strand at its own position in the field |

## Tips

- Hum your key note into a mic and capture it in **texture / root**
  mode: instant non-monotonous pad, sympathetic to your key.
- **rhythm / random** with a spoken phrase makes funky beats of
  unexpected content; send **g1** to a kick for reinforcement.
- Clock **rec** every few bars while feeding it a live source: the weave
  stays, the cloth keeps renewing — a texture that follows the music.
- Trigger **weave** from a slow random gate to let the module wander by
  itself.
- All levels down except **fleck** in texture mode: sparse, glassy
  punctuation.

## Attribution

Behavioral clone inspired by the Fieldtone Weaver Modular. The engine is
original code, designed from public descriptions; not affiliated with
Fieldtone.
