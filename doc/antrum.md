# antrum

**Feedback delay network reverb, from a coffin to the heavens, all of it modulatable.**

*antrum* is Latin for "cave, grotto". The module is built after the **Make
Noise / SoundHack Erbe-Verb**, Tom Erbe's reverb for Eurorack: not a room
you park a signal in, but a resonant space you *play*, where every
parameter takes voltage and the size control runs continuously from
unrealistically small to unrealistically large without ever changing
algorithm.

The topology follows Erbe's ICMC 2015 paper, *Building the Erbe-Verb:
Extending the Feedback Delay Network Reverb for Modular Synthesizer Use*:

```
in ─> pre-delay (forward or reversed) ─┐
                                       v
     ┌──────────── 4 delay lines ──────┴───────────┐
     │  read plain, sine-modulated, or as grains   │
     │             v                               │
     │       Chebyshev saturation, driven by the   │
     │         network's own energy                │
     │             v                               │
     │       unitary (Hadamard) matrix ─> DC block │
     │             v                               │
     │       absorb lowpass ─> allpass diffuser    │
     │             v                               │
     │           x decay ──────────────────────────┘
     └─────────────────────────────────────────────┘
                   v
        + early reflection taps
                   v
             tilt ─> dry/wet mix ─> l, r
```

A four-line feedback delay network with a unitary matrix is lossless: at
a feedback gain of exactly 1.0 it sustains forever, below it decays
exponentially, and above it (the **decay** knob goes to 120%) it pumps
energy in until the saturation stages catch it. That is why **decay** and
**absorb** interact so strongly — the saturation adds harmonics and the
absorption filters take them away, and where the two balance is where the
infinite reverbs live.

The four delay times are kept mutually prime and all scale together from
the single **size** control, so sweeping size is a mass of coordinated
doppler shifts rather than a switch between presets. Slowly it sounds
like the walls moving; quickly it is percussive; at audio rate it is FM.

## Controls

Every knob has its own attenuverter and CV input directly below it.

| knob | function |
|------|----------|
| **size** | 1–500 ms of network delay: coffin (full CCW), room (noon), plate, hall, heaven (full CW). Also scales the early reflections |
| **pre-delay** | 7–500 ms before the first reflections, independent of size. Under a clock (see **clk**) it becomes a ratio of the clock instead |
| **decay** | feedback gain, 0–120%. Past ~100% the tail stops decaying and starts feeding on itself |
| **absorb** | diffusion *and* damping in one knob: full CCW = no diffusion, no damping; ~10:00 = full diffusion, no damping; full CW = full diffusion, full damping (a dark, dead space) |
| **depth** | bipolar modulation depth *and type*. Noon is no modulation. CCW is **cyclic**: a multiphase sine vibrato in the four delay lines, from subtle chorusing to extreme doppler swirl. CW is **ergodic**: raised-cosine grains scattering the room dimensions at random, granular at high depth. The last stretch CW fades in **shimmer**, an octave-up voice folded back into the network |
| **speed** | 0.5–256 Hz, the rate of whichever modulation **depth** selected (LFO rate for cyclic, grain rate for ergodic). Under a clock it is a ratio of the clock. With depth at noon it does nothing |
| **tilt** | bipolar output tone: CCW cuts highs and boosts lows heavily, CW the opposite, flat at noon. It sits after the network, so it never touches the feedback |
| **mix** | equal-power dry/wet blend |
| **rev** | button, toggles reverse: the pre-delay buffer plays backwards, with a short crossfade so it does not click. The **pre-delay** control sets the reverse buffer length |

## Inputs and outputs

| jack | function |
|------|----------|
| **in l / in r** | stereo audio in; **in r** is normalled to **in l** for mono use |
| **clk** | clock input. While patched, **pre-delay** and **speed** snap to ratios of the clock (1/12, 1/8, 1/6, 1/4, 1/3, 1/2, 2/3, 1/1, 3/2, 2, 3, 4, 6, 8, 12 — 1/1 at noon). CCW of noon is longer/slower, CW is shorter/faster |
| **gate** | momentary reverse: high engages the reverse buffer, releasing it returns to forward |
| **cv** (out) | the network's own energy as 0–10 V, from an envelope follower on the reverb. Patch it back into **decay** through an inverting attenuverter and the reverb reins itself in like a compressor; patch it to **size** for a space that swells with what you play |
| **l / r** | stereo output |

Every CV input is ±5 V for the full range of its parameter, scaled by its
attenuverter.

## Tips

- The starting point for a normal reverb is size at noon, pre-delay low,
  decay around 60%, absorb at noon, depth at noon, mix around 11:00.
  From the manual of the original: *coffin* size full CCW / decay 9:00,
  *room* both at noon, *plate* size 1:00 / pre-delay full CCW, *hall*
  size 3:00 / pre-delay 11:00, *heaven* everything full CW.
- **size** is the parameter to modulate. Slow ramps morph one space into
  another; fast ones turn the reverb into a resonator. It is the one knob
  worth an attenuverter and an envelope before anything else.
- Push **decay** to full and ride **absorb**: the reverb sustains forever
  while absorb decides how bright and how long "forever" is.
- Reverse plus a long pre-delay plus full wet gives the classic reverse
  reverb, and it syncs to the patch through **clk**.
- Depth full CW with a slow **speed** and infinite decay is the ghost
  choir: shimmer feeding a network that never lets go.

## Differences from the hardware

- Stereo in (the hardware is mono in, stereo out), and every parameter
  gets an attenuverter, not just size, depth, decay and tilt.
- Clock-synced pre-delay reaches 4 s here; the hardware reaches 5.46 s.
- No input level control: patch a VCA or use the CV inputs.
- The internal modulation depth for ergodic grains scales with **size**
  (up to half the room), which the reference implementation caps at a
  fixed few milliseconds. Cyclic vibrato keeps the small fixed depth.

## Attribution

The Erbe-Verb was designed by Tom Erbe (SoundHack) with hardware by Make
Noise (Tony Rolando, Matthew Sherwood). This module is an independent
implementation from Erbe's published paper and the module's manual, and
is not affiliated with Make Noise or SoundHack. The block layout, the
delay-time ratios, the early reflection taps and the analog tilt filter
model follow **dm-Reverb** by Dave Mollen
([davemollen/dm-Reverb](https://github.com/davemollen/dm-Reverb)),
GPL-3.0, the same license as this plugin.
