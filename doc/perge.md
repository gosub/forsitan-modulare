# perge

**Stereo dynamic sampler and multi-effect: repeats that listen to how you
play.**

*perge* is Latin for "carry on!, keep going!" (imperative of *pergere*).
The module is a from-scratch homage to the AC noises / BunkerNoise
**CONTINUA** pedal — whose name is the Italian for the same exhortation —
rebuilt from its public documentation: a
sampler that doesn't just play your sound back, but reacts to how hard and
how fast you play while you're still playing, then transforms the repeats
through a multi-effect section.

## How the repeats work

The module listens to the input with an envelope follower. When the level
crosses **thrs** (threshold), a capture starts, and it runs until the input
falls quiet again (up to 2 seconds). The captured sample becomes the newest
of three sample slots, and repeats of it are spawned on a tempo grid:

- **tempo** sets the repeat rate (100 ms – 2 s, CV addable), or patch a clock into
  **clock** and the repeats follow it (the right-click *Clock multiplier*
  menu scales the incoming clock by 1/4 – x4; it has no effect on the
  tempo knob).
- **sens** sets how strongly your dynamics matter: low = even, consistent
  repeats regardless of touch; high = loud hits produce loud repeats, soft
  playing nearly disappears.
- **atk** / **rel** shape each repeat's envelope, from tight echo-like
  slices to soft atmospheric swells.
- **sustain** sets how long the repeat stream persists (per-repeat decay).
  Past ~90% of the knob the engine **freezes**: the captured audio is held
  indefinitely, capture stops, and every other control keeps working on the
  frozen material. The **freeze** button (latching) and gate do the same.
- **pitch** is inactive at noon. Clockwise, repeats are *randomly* shifted
  up — first an octave fades in, then a fifth above that; counterclockwise
  mirrors downward. Each repeat rolls its own dice.
- **glt/dim** is inactive at noon. Counterclockwise (**glitch**): sudden
  unpredictable tempo accelerations fragment the flow. Clockwise
  (**dimension**): the two previous captures come back as extra layers,
  each locked to the grid but on its own rhythmic pattern (every 2nd and
  every 3rd tick) — up to three samples coexist.
- The **mode** switch (bottom row, between the freeze and tilt jacks) plays
  repeats **standard**, **reverse**, or **tail** (only the swelling tail of
  each repeat).
- *Cap grain to tempo interval* (right-click, on by default) limits each
  repeat to one tempo period, keeping repeats short and discrete. Turn it
  off to let each repeat replay the whole captured note, which overlap into
  a denser, more sustained wash.

## The multi-effect section

The wet bus runs through four bipolar effects, all inactive at noon:

| knob | counterclockwise | clockwise |
|------|------------------|-----------|
| **lofi/crs** | lo-fi: pitch-LFO wobble plus progressive darkening | crush: sample-rate reduction, harsher as you turn |
| **rvrb/smr** | deep dark reverb | smear: allpass diffusion that melts transients together |
| **lp/hp** | lowpass (darker, warmer) | highpass (thinner, sharper) |

- **mod** sets the lo-fi/crush modulation depth and speed.
- **dcay** sets the reverb/smear tail length.
- **sprd** sets the stereo width of the repeats (each repeat gets its own
  random pan position).
- **infx** only matters with *Alternative routing* (right-click menu): it
  sets how much dry signal is routed through the multi-effect section, the
  rest staying clean.
- **mix** balances dry and repeats, dry-full to wet-full.

## tilt

**tilt** (momentary button or gate) is the chaos switch: while held, pitch
(a few semitones), lo-fi, crush, reverb and smear are randomly re-modulated
a few times a second — scattered, drifting warble that snaps back when
released.

## Patching

| jack | function |
|------|----------|
| **in l / in r** | stereo input; right is normalled to left (the LED by *in l* flashes while capturing) |
| **tempo / pitch / sust / glit / filt** | CV for tempo, pitch, sustain, glitch/dimension and filter (added to the knobs) |
| **clock** | external tempo; measured between rising edges, released ~10 s after the clock stops |
| **frz** | freeze gate |
| **tilt** | tilt gate (momentary) |
| **out l / out r** | stereo output; with only *out l* patched the two channels are summed |

## Tips

- Feed it sparse, dynamic playing with **sens** high: the module answers
  loud phrases and ignores the quiet ones, like a duet partner.
- **sustain** just below the freeze zone plus a slow clock makes drifting,
  Basinski-adjacent beds; add a little **lofi** and **dcay** high.
- **glitch** at 3 o'clock equivalent (CCW!) with **tail** mode turns
  percussive input into stuttering reversed ghosts.
- Patch a random gate into **tilt** for periodic bursts of chaos.
- With synth pads that never fall below the threshold, the capture rolls
  over every 2 s and the repeats degenerate into arbitrary chunks of the
  pad. Raise **thrs** until only deliberate swells cross it and captures
  become discrete phrases again.

## Notes

- The sample buffer lives at the engine sample rate; changing the sample
  rate (or the right-click "Clear buffer") empties it. Buffer contents are
  not saved with the patch.
- The repeats engine is a voice pool (16 voices); extremely fast clocks
  with long releases steal the oldest voices first.
