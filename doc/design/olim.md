# olim - design doc

Status: design, nothing built. Written 2026-09-25.

An eight-head stereo delay over one long buffer: a TIME knob places the
farthest head, SPREAD distributes the other seven between now and then, nine
sliders mix the dry signal and the eight heads, and FEEDBACK runs from sound
on sound into a howl. It is stage 3 of [the three-stage chain](olim-chain.md),
built alone and made whole: the hardware's panel, not the chain's compressed
version of it.

**olim** is Latin for "once upon a time", and also for "someday": the same
word points both directions, which is what the heads are.


## Lineage, and the rule for building it

olim is in the family of Olivia Artz Modular's **Time Machine** (22 HP,
stereo, 2024), and of its VCA expander (2026). The firmware is public and
licensed CC BY-NC-SA 4.0, which cannot be linked into a GPL-3 plugin, so the
rule from [olim-chain.md](olim-chain.md#lineage-and-the-rule-for-building-it)
applies unchanged: **implement from this document**. Do not open the
firmware's sources or its schematic, do not transcribe a constant, do not
compare output with it sample for sample. The technique is not anyone's to
license: a circular buffer, read heads at fractions of a delay time, a
distribution law across them, clock quantisation, crossfaded head jumps,
feedback through a limiter.

Everything the hardware is known to do comes from its public manual
(`oamodular.org/docs/tm`) and its product pages; the rest is inference, and
is marked as inference below. Credit in `doc/olim.md` is "in the family of",
as bulla credits Hordijk.


## What the hardware does, from the manual

- **t** sets the farthest delay, [almost] 0 to 8 s, exponential.
- **t/2v** is exponential CV on it, +1 V halves the time, -1 V doubles it,
  -5 to +5 V, for 0.0001 s to 2 min 30 s overall.
- **Spread**: at noon the eight heads are evenly spaced; left bunches them
  toward now, right toward t. CV -5 to +5 V.
- **Clock**: with a clock patched, t is always quantised to it, and with
  spread at noon so are all eight heads. Spread moves the seven inner heads
  off the beat; the last one stays.
- **Feedback**: an arc on the panel marks the zone where sound "will neither
  disappear nor swell out of control", the sound-on-sound zone. Beyond it,
  "feedback and chaos similar to guitar-amp feedback but evil and digital".
  CV -5 to +5 V.
- **Nine faders**: the dry signal and the eight heads.
- **VCA expander**: nine inputs, linear VCAs on the nine faders, unity at
  +5 V, clamped below 0 and above +5 V. Patched, a fader becomes an
  attenuator on its VCA. The manual mentions a "subtle volume normalization"
  that the clamping works together with.
- Stereo in, stereo out, 24-bit audio at +-5 V. Firmware 1.1.2 removed
  dropouts and clicks when moving spread and t.
- A row of lights, one per head.


## Engine

`src/olim/olim.hpp`, header-only, `namespace olim`, no Rack dependency past
`dsp` helpers, so the probe links it without a module.

### Buffer

One float buffer per channel, written every sample with `in + feedback`.
Length is a context-menu choice, **Memory: 20 s / 60 s / 150 s**, default
150 s, the hardware's figure: 57.6 MB at 48 kHz stereo, 115 MB at 96 kHz.
Allocated with `calloc`, whose pages are only committed as the write head
reaches them, and reallocated on a sample-rate or menu change off the audio
thread (`imber_worker::startDetached`, as imber builds its bank). A
delay that asks for more than the buffer holds clamps to it.

### Head positions

`T = TIME knob * 2^(-t/2v)`, clamped to [2 samples, buffer]. The knob runs
3.2 ms to 8 s exponentially: 3.2 ms is 0.0001 s times 2^5, so the manual's
overall range falls out of the knob and the CV range together.

Head `i` of 8 sits at `d_i = T * w_i`, with `x = i / 8` and

```
w_i = x ^ p,    p = 4 ^ (-spread)      spread in [-1, 1]
```

At noon `p = 1` and the heads are even. Negative spread gives `p > 1` and
bunches them toward now; positive gives `p < 1` and bunches them toward T.
`w_8 = 1` whatever `p` is, so the last head is always T, as the manual has
it. The base 4 is a first guess, to set by ear.

**Clock (inference).** With a clock patched, T snaps to the clock period `P`
times a power of two, `T = P * 2^round(log2(T / P))`. A power of two, not an
integer multiple, because it is the only reading under which the manual's
"all eight quantised when evenly spaced" holds: with T = 8P the heads land on
every beat, with T = P on every eighth of one. The period is measured
between rising edges and taken as the mean of the last few, as vates does,
so a jittery clock does not shake the heads.

### Moving a head (inference, and the character)

A head never slides. It holds its position until the target moves, then
**crossfades** from the old position to the new one over a grain of length
`G`, and only picks up the next target once the fade is done. Turning TIME
therefore does not pitch-shift; it re-grabs the buffer in overlapping grains,
which is where "granulizing like grey goo" and the smear between heads have
to come from. Two read pointers per head, equal-power fade.

`G` is the first number to settle by ear, and the one with a known conflict:

- long (order 100 to 200 ms) makes the smear, and hides the jump;
- short makes t/2v at short delays act like the comb, flanger and
  Karplus-Strong the reviews describe, because a comb tuned by t/2v at 1 V
  per octave has to follow its CV, not step every 200 ms.

Proposal: `G = clamp(T, 2 ms, 150 ms)` per head, so a head reaches for a new
position about once per its own round trip. Measured, not argued: the probe
reports what a t/2v sweep does to the comb pitch at each candidate.

Reads are cubic (Hermite): at T of a few samples the heads are a tuned comb
and linear interpolation audibly damps it.

### Mix and feedback

```
wet   = sum_i  g_i * head_i  /  N(g)
out   = g_dry * in + wet
write = in + limit(fb * wet)
```

`g_i` is slider i times its VCA, `g_dry` the dry slider times its VCA.

**N(g) is the normalisation** (inference): without it, loop gain is
`fb * sum(g_i)` and the sound-on-sound zone would move every time a slider
does. With `N = max(1, sum(g_i))` the zone stays put: loop gain at a single
head is `fb`, and with several correlated heads never above it. Whether
`sum` or a power sum (`sqrt(sum(g_i^2))`, which assumes decorrelated heads)
holds the zone better is a probe measurement, not a guess. It is also what
makes a VCA patched into a head change the timbre and not just the level.

**FEEDBACK** runs 0 to 1.5 in loop gain. The **arc** on the panel covers the
range where the probe measures the loop as neither decaying nor growing
within a few dB over a minute, expected around 0.95 to 1.0.

**limit** is a soft limiter in the loop, `x / (1 + |x| / L)`-like, at
L = 5 V, plus a DC blocker at a few hertz: past unity the loop must go
somewhere, and a symmetric soft limit is what turns runaway into sustain.
"Evil and digital" says the hardware does something harder; a hard clip is
the fallback if the soft one sounds polite.

Stereo is two identical engines sharing positions: L in feeds L buffer.
R in is normalled from L in. Nothing crosses between channels.

### Clicks

Every jump is a crossfade, the sliders and FEEDBACK are smoothed (one pole,
about 10 ms), and the Memory change fades to silence first. The #22 method
applies: max second difference over peak on a 220 Hz sine through a moving
TIME or SPREAD, clean near 0.001, limit 0.01.


## Controls and jacks

| param | range | notes |
|---|---|---|
| TIME | 3.2 ms - 8 s, exponential | display in seconds |
| SPREAD | -1 .. +1, noon even | |
| FEEDBACK | 0 - 1.5 loop gain | arc on the panel |
| DRY | 0 - 1 | slider |
| HEAD 1..8 | 0 - 1 | slider, lit by its head's level |

| input | range |
|---|---|
| IN L, IN R | audio, R normalled from L |
| TIME CV | -5 .. +5 V, +1 V halves T |
| SPREAD CV | +-5 V spans the knob |
| FEEDBACK CV | +-5 V spans the knob |
| CLOCK | rising edges |
| DRY VCA, HEAD 1..8 VCA | 0 .. +5 V linear, unity at 5 V, clamped; normalled to +5 V |

Outputs: OUT L, OUT R.

Four knob-like params, eight sliders plus dry, sixteen inputs counting the
nine VCAs, two outputs, nine slider lights, a clock LED, two output LEDs.
Every enum is appended to, never reordered, once released.

Context menu: Memory (20 / 60 / 150 s), and anything the probe earns.


## Panel

The standard forsitan grammar, labels and badges. Nine slider columns,
DRY first on the left, then heads 1 to 8 with their times rising left to
right, each slider over its VCA jack: about **24 HP** (121.92 mm), two
more than the hardware for the nine VCA jacks it keeps on a separate board.
Above the sliders TIME, SPREAD and FEEDBACK as big knobs, each with its CV
jack; FEEDBACK has the arc drawn round it. Along the bottom IN L, IN R,
CLOCK, OUT L, OUT R, then the logo.

`VCVLightSlider` is 6.7 by 25.9 mm; the column pitch is whatever 24 HP
leaves after the screws, and `panel_audit.py` decides.


## Tests

- `test/olim_probe`:
  - `heads` - the eight positions against SPREAD and TIME, clock or not;
    checks `w_8 == 1` and the power-of-two snap.
  - `loop` - loop gain against FEEDBACK and slider settings, level after
    60 s; gives the arc its ends and picks N(g).
  - `clicks` - the #22 measurement under TIME and SPREAD sweeps, CV and
    knob.
  - `comb` - the pitch a short T gives, and how well it follows t/2v at
    each grain length `G`.
- `test/smoke_olim`: construction at each Memory setting, NaN and silence
  checks, the VCA normalling, the clamp at 150 s.
- `test/audition/olim.md`, about a dozen items.


## Open questions

1. `G`, the grain length, and whether it depends on T (see Moving a head).
2. `N(g)`: plain sum or power sum.
3. Soft or hard limit in the loop.
4. The spread base (4) and the feedback ceiling (1.5).
5. Whether the heads' lights show the head's level or its slider times its
   VCA (the second is what the expander's patching needs to see).


## Build order

Each step is a commit.

1. `src/olim/olim.hpp` and `test/olim_probe`: buffer, heads, spread law,
   clock snap, crossfading moves, mix, feedback, limit. Settle open
   questions 1 to 4 by measurement before any panel.
2. `src/olim.cpp` on a placeholder panel, context menu, `test/smoke_olim`.
3. The panel, `panel_audit.py`.
4. `test/audition/olim.md`.
5. `doc/olim.md`, readme row, `plugin.json` entry, glossary entry, CLAUDE.md
   table row. A new module, so a minor version.
