# olim - design doc

Status: built, 2026-09-25. The module follows this doc; `doc/olim.md` is
the manual.

An eight-head stereo delay over one long buffer: a TIME knob places the
farthest head, SPREAD distributes the other seven between now and then, nine
sliders mix the dry signal and the eight heads, and FEEDBACK runs from sound
on sound into a howl. It is stage 3 of [the three-stage chain](olim-chain.md),
built alone and made whole: the hardware's panel, not the chain's compressed
version of it.

**olim** is Latin for "once upon a time", and also for "someday": the same
word points both directions, which is what the heads are.


## Lineage, and the rule for building it

olim is a clone of Olivia Artz Modular's **Time Machine** (22 HP, stereo,
2024), with its VCA expander (2026) built in. The firmware is public
(`github.com/oamodular/time-machine`) under CC BY-NC-SA 4.0, which cannot be
linked into a GPL-3 plugin.

The first draft of this doc kept the firmware closed, as radix kept the
Radical22's. For olim the user decided otherwise: **the firmware is the
source of truth for behaviour**, and was read to settle the open questions
the manual left. What that allows and what it does not:

- the behaviour, the signal flow and the numbers that set it (a 200 ms
  crossfade, a limiter at full scale, a knob's dead zone) are taken as they
  are, because being a clone means matching them;
- the code is not: `src/olim/olim.hpp` is written fresh from this doc, in
  this repo's idiom, with no line, name or structure carried over, and the
  firmware's sources are not copied into the repo;
- `doc/olim.md` credits the Time Machine by name and says the firmware was
  studied.

Everything below marked **firmware** was read there; everything else is ours.


## What the hardware does

From the manual (`oamodular.org/docs/tm`):

- **t** sets the farthest delay, [almost] 0 to 8 s; **t/2v** halves it per
  +1 V, -5 to +5 V, for 0.0001 s to 2 min 30 s overall.
- **Spread**: noon spaces the eight heads evenly, left bunches them toward
  now, right toward t.
- **Clock**: t is quantised to it; with spread at noon so are all eight
  heads.
- **Feedback**: an arc marks the sound-on-sound zone; beyond it, feedback
  "similar to guitar-amp feedback but evil and digital".
- **Nine faders**: dry and eight heads. **VCA expander**: nine linear VCAs
  on them, unity at +5 V, clamped to 0..5 V, the fader becoming an
  attenuator.
- Stereo in and out, +-5 V. A light per fader.

From the firmware, which fills in how:

### Signal flow, per channel (firmware)

The two channels are two independent copies with the same settings; nothing
crosses between them. Full scale 1.0 is 5 V. Per sample:

```
buf[w]  = in
wet     = sum of the eight heads, each already times its gain
wet     = wet - verySlowDC(wet)                  (0.08 Hz)
wet     = compress(wet, key = in + wet)          (0 dB, 5:1, 20 ms / 200 ms)
buf[w]  = -limit_fb(in + wet * fb * norm)        (written inverted)
out     = limit_out(wet + in * dry)
w       = w + 1
```

- **norm** = `1 / max(1, sum of head gains)`, smoothed over about 0.2 s. It
  is in the **feedback path only**: the output is the plain sum, so raising
  more sliders makes it louder, but the loop gain at fb = 1 stays at most 1.
- **Inverted feedback**: the buffer holds the negated signal, so the first
  echo is inverted and each pass flips the sign again.
- **limit** is a gain limiter at full scale: gain `1 / max(1, |x|)`, instant
  attack, release at 16 per second. One in the loop, one on the output.
- **compress** is a feed-forward compressor keyed on `in + wet`, threshold
  0 dB (full scale), ratio 5, attack 20 ms, release 200 ms, no makeup: it
  only acts above full scale, and it is what holds the howl.
- A head at delay 0 reads `buf[w]` after `in` is written and before the
  feedback overwrites it: TIME at zero plays the input, not inverted.

### Heads (firmware)

- Delay in whole samples, truncated; no interpolation.
- Each head has two positions A and B, and gains A and B, and crossfades
  linearly from A to B at **5 Hz** (200 ms). When a fade ends it starts the
  next at once, with B becoming A and the current target becoming B. The
  heads are therefore **always** fading, every 200 ms, even standing still;
  a knob move lands within 200 ms and sounds as a crossfade to the new
  place, never as a pitch bend. Slider moves are carried the same way, in
  200 ms linear steps.
- **Blur**: with feedback above 1, each fade's rate is drawn at random from
  `5 +- (fb - 1)` Hz, so at fb = 3 the fades run anywhere from 3 to 7 Hz.
  Every head in every channel draws its own: the stereo image decorrelates
  as feedback rises.
- Each head's light is its own level before the slider (a follower on
  `|head|`, about 20 ms). The dry fader's light is the input's level.

### Controls (firmware)

- **TIME**, free: `T = 8 s * knob^2 / 2^(5 * cv)`, cv being volts / 5,
  clamped to +-1, and T clamped to the buffer. Quadratic, not exponential,
  and it reaches 0.
- **TIME**, clocked: the knob becomes 12 steps and the CV 10 steps over its
  +-5 V (2 steps per volt), each a factor of two, with hysteresis of a third
  of a step so it does not chatter at the edges. At noon T is one clock
  period, from 1/64 to 64 periods, halved until it fits the buffer.
- **Clock**: the period is the running mean of the last two edge intervals
  (`(old + new) / 2`); a clock that has not ticked for 2 s is gone, and TIME
  goes back to free.
- **SPREAD**: `x = i / 8` for head i = 1..8, `s` the knob with its CV in
  0..1, `k = 1 + 2.5 * |2s - 1|`:
  `w = x^k` below noon (bunched toward now), `1 - (1 - x)^k` above (toward
  T), `x` at noon. `w8 = 1` always: the last head is T.
- **FEEDBACK**: `fb = clamp(2 * dz(knob) + cv, 0, 3)`. `dz` is a dead zone
  at noon applied twice, so knob 0.405 to 0.595 gives exactly fb = 1: that
  is the **arc**, and the sound-on-sound zone is the dead zone.
- **SPREAD** has the same double dead zone, so noon is exactly even.
- **Sliders**: linear, times the VCA (0..5 V to 0..1, clamped).
- **Dry** is smoothed (about 20 ms), feedback lightly (2 ms).
- Buffer 150 s at 48 kHz, fixed.


## Engine

`src/olim/olim.hpp`, header-only, `namespace olim`, no Rack dependency, so
the probe links it alone. It takes the controls already read (knob positions,
CV in volts, slider and VCA values, a clock gate) and does the mapping above
itself, so the probe exercises the same laws the module plays.

**Where it has to differ from the hardware**, because the host is not a
Daisy at 48 kHz:

- **Sample rate.** Every rate above is in hertz or seconds and is converted
  at the host rate: the firmware's per-sample smoothing constants were
  written for 48 kHz and are restated as time constants.
- **Memory.** 150 s at 48 kHz stereo is 57.6 MB, at 96 kHz 115 MB, at
  192 kHz 230 MB. The buffer is sized for the host rate, and a context-menu
  **Memory** of 20 / 60 / 150 s (default 150, the hardware's) caps it. Long
  TIMEs clamp to it as the hardware clamps to 150 s. Allocation happens off
  the audio thread (`imber_worker::startDetached`), with `calloc` so pages
  are committed only as the write head reaches them.
- **Per block vs per sample.** The firmware reads its controls once per
  7-sample block; olim sets the head targets every 8 samples, which is the
  same thing at Rack's rates.
- **Calibration, knob noise gates, ADC dead bands at the ends**: hardware
  concerns with no Rack equivalent, left out.

There is nothing else to choose. The earlier draft's open questions (grain
length, normalisation, limiter, spread law, feedback ceiling, what the lights
show) all have firmware answers above.


## Controls and jacks

| param | range | notes |
|---|---|---|
| TIME | 0..1, displays seconds (or clock multiple) | quadratic, 0 to 8 s |
| SPREAD | 0..1, noon even | double dead zone |
| FEEDBACK | 0..1, displays loop gain 0..2 | arc at 0.405..0.595 |
| DRY | 0..1 | slider, lit by the input level |
| HEAD 1..8 | 0..1 | slider, lit by its head's level |

| input | range |
|---|---|
| IN L, IN R | audio, R normalled from L |
| TIME CV | -5 .. +5 V, +1 V halves T (2 steps per volt clocked) |
| SPREAD CV | +-5 V adds +-1 to the knob |
| FEEDBACK CV | +-5 V adds +-1 to the loop gain |
| CLOCK | rising edges; gone after 2 s without one |
| DRY VCA, HEAD 1..8 VCA | 0 .. +5 V linear, unity at 5 V, clamped; unpatched is unity |

Outputs: OUT L, OUT R (limited to +-5 V).

Context menu: **Memory** 20 / 60 / 150 s.


## Panel

The standard forsitan grammar, labels and badges. Nine slider columns,
DRY first on the left, then heads 1 to 8 with their times rising left to
right, each slider over its VCA jack: about **24 HP** (121.92 mm), two
more than the hardware for the nine VCA jacks it keeps on a separate board.
Above the sliders TIME, SPREAD and FEEDBACK as big knobs, each with its CV
jack; FEEDBACK has the arc drawn over its dead zone. Along the bottom IN L,
IN R, CLOCK, OUT L, OUT R, then the logo.


## Tests

- `test/olim_probe`:
  - `heads` - the eight positions across SPREAD, TIME free and clocked;
    checks `w8 == 1`, the dead zones, and the power-of-two steps.
  - `loop` - level per pass against FEEDBACK and slider sets: at fb = 1
    (the arc) one head and eight heads both hold, below it they decay,
    above it the compressor catches the growth under full scale.
  - `clicks` - the #22 measurement (max second difference over peak on a
    220 Hz sine) under TIME and SPREAD sweeps, knob and CV.
  - `cpu`, and `wav <dir>` for listening.
- `test/smoke_olim`: construction at each Memory setting, NaN and silence
  checks, the VCA normalling, the clamp to the buffer.
- `test/audition/olim.md`, about a dozen items.


## Build order

Each step is a commit.

1. `src/olim/olim.hpp` and `test/olim_probe`.
2. `src/olim.cpp` on a placeholder panel, context menu, `test/smoke_olim`.
3. The panel, `panel_audit.py`.
4. `test/audition/olim.md`.
5. `doc/olim.md`, readme row, `plugin.json` entry, glossary entry, CLAUDE.md
   table row. A new module, so a minor version.
