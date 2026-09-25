# the chain (was olim) - design doc

Status: design, nothing built. Written 2026-08-30.

The name olim has gone to stage 3, built alone as its own module: see
[olim.md](olim.md). Below, "olim" means the three-stage chain.

On hold as a whole: stage 1 is being built first as a standalone module,
see [radix.md](radix.md), which supersedes this file where they differ.

A three-stage instrument on one panel: a chaotic 8-bit source, a filter, and
a multi-head delay, normalled in series with every junction broken out, and a
stepped chaos bus tying them together.

The shape is Maneco Labs': their Grone Manecolin is 33 HP of chaotic digital
oscillators feeding a Rungler, an MS-20 filter modulated by that Rungler, a
reverb, and saturation, with patch points that break the normalling so each
piece is usable alone. olim is that arrangement with our own three pieces.

**olim** is Latin for "once upon a time", and also for "someday": the same
word points both directions, which is what the delay stage is.


## Lineage, and the rule for building it

Two of the three stages are in the family of hardware whose firmware is
licensed CC BY-NC-SA 4.0, which cannot be linked into a GPL-3 plugin:

- Stage 1 is in the family of Dirty Electronics' **Radical22**.
- Stage 3 is in the family of OAM's **Time Machine**.
- Stage 2 is viginti's own KORG35, already in this repo.

Neither is a port. Both are built from technique, which is not anyone's to
license: phase-accumulator DDS, deliberate integer overflow, XOR grit on the
low bits, text bytes as a wavetable, an engine clock slower than the audio
rate, multiple read heads over one buffer, a spread law across them, inverted
limited feedback. The bytebeat family this belongs to is public and
documented since 2011, eleven years before Radical22.

**The rule while building: implement from this document.** Do not have
`main.c` or `dsp.h` open, do not transcribe an expression, do not check the
output against theirs sample for sample. What must not appear anywhere is the
eight Radical22 increment lines as a set, its `myarray` bytes, or its 4x8
program map.

Credit goes in `doc/olim.md` the way bulla credits Hordijk and scrupea
credits Skrewell: "in the family of", naming the influence, which is normal
and carries no licence.

The plugin stays GPL-3-or-later. No second plugin, no permission emails.


## Signal flow

```
  [IN] ────────> RADIX ──> [SRC OUT]
                   |              \
                   |         [FLT IN] ──> FILTER ──> [FLT OUT]
                   |                         ^              \
                   |                      cutoff        [DLY IN] ──> DELAY
                   |                         |                          |
                   +──> CHAOS BUS ───────────+                          |
                   |         |                                          v
                   |    [CV OUT]                              SATURATE ──> [OUT L/R]
                   |                                                       |
                   +<─────────────────── [FB IN] <───────────────────------+
```

Each stage's input jack breaks the normal above it, so the module is four
instruments: radix alone is an 8-bit chaos oscillator, the filter alone is a
second viginti, the delay alone is the time machine, and the chain is olim.


## Stage 1: RADIX

An integer machine running on **its own clock**, not Rack's, with a
zero-order hold into the host rate. This is the same arrangement tundo uses
for its engine, and it is the single biggest expansion over the hardware:
Radical22's character comes from a free-running PWM clock, and putting that
clock on a knob and into the audio band opens the whole space.

State is a 32-bit accumulator `acc`, a 32-bit `rate`, and three free-running
counters incrementing by 1, 3 and 6 (different periods, so they beat).

Per engine tick:

1. draw a modulator byte `m` from **SRC**
2. compute the increment from `rate` and `m` per **LAW**
3. `acc += inc`, wrapping, deliberately
4. `sample = TABLE[(acc >> 24) & 0xFF]`
5. quantise to **BITS**
6. XOR **GRIT** into the low bits
7. hold until the next tick

The generalisation is that the modulator source and the way it is applied are
**independent switches**, where the hardware hardcodes a list of pairs. Five
by five is 25 increment behaviours against its eight, and the read stage
multiplies again.

**SRC** - where the modulator byte comes from:

| position | source | character |
|---|---|---|
| `PARAM` | the PARAM knob, static | plain, the knob is a number |
| `TABLE` | `TABLE[param]` | the knob sweeps the table, FM-like |
| `SELF`  | `TABLE[acc >> 24]` | phase feedback, the accumulator drives itself, chaos |
| `COUNT` | one of the free counters | drifting, beating, never quite repeating |
| `IN`    | the audio/CV input | radix becomes a processor, not just a source |

**LAW** - how it is applied:

| position | increment | character |
|---|---|---|
| `ADD`   | `rate + m` | gentle, stays near pitch |
| `MUL`   | `rate * m` | wide, overflows freely |
| `SHIFT` | `rate << (m >> 5)` | octave jumps, brutal |
| `XOR`   | `rate ^= m` then `inc = rate` | the rate corrupts itself and never recovers |
| `MOD`   | `acc = acc % (acc + m)` then `+= rate` | non-monotonic, heavily aliased |

**TABLE** - the read stage:

`SINE`, `SAW` (identity, no table), `PULSE`, `NOISE` (a fixed seeded random
table), `BITS` (the accumulator's own bits, no table at all), `TEXT`.

`TEXT` is the one worth generalising hardest. The hardware has one fixed
table baked from one fixed string. Ours generates the 256 bytes from a seed,
with the string settable from the context menu, so every patch can carry its
own wavetable made out of its own words.

**Controls:** RATE (V/oct trackable, so it can be played, which the hardware
cannot), PARAM, CLOCK, BITS, GRIT, plus the three switches.
**Jacks:** V/OCT, PARAM CV, CLOCK CV, IN, SRC OUT.

CLOCK runs 100 Hz to 96 kHz, exponential. BITS runs 16 down to 1. GRIT is the
XOR-into-the-low-bits trick as a continuous amount rather than a fixed quirk.


## Stage 2: filter

`#include "viginti_dsp.hpp"` and instantiate `viginti::Korg35Filter`. It is
already a namespaced header, already measured against the paper authors'
renders, and needs no refactor at all. The MS-20 lineage is also literally
what Maneco use in the Grone.

Its resonance distorting and collapsing with level is the right behaviour
under a square-ish 8-bit source, which is why it beats a clean ladder here.

**Controls:** CUTOFF, RES, DEPTH (chaos bus into cutoff).
**Jacks:** FLT IN, CUTOFF CV, FLT OUT.

The alternative is vespae, whose LP-to-HP mix pot buys a whole character
sweep on one knob, which matters on a panel this crowded. Decided in favour
of viginti for the lineage; revisit if the panel runs out of room.


## Stage 3: the delay

Eight read heads over one buffer per channel, which is Time Machine's
architecture and the part worth having.

- **TIME** places the farthest head. With a clock at the CLOCK input, it
  quantises to power-of-two multiples of that clock instead.
- **SKEW** distributes the other seven between the head and TIME, from
  bunched near the present, through linear, to bunched at the far end.
- **SHAPE** replaces the hardware's eight sliders with one control over the
  amplitude envelope across the heads: flat, decaying, rising, alternating,
  or re-randomised on each change. This is the compression that makes the
  stage fit on a shared panel, and it is the one place the design gives
  something up.
- **FEEDBACK** runs past unity, inverted and limited. Past 1.0 the excess
  becomes **blur**: the head crossfade rate is randomised rather than fixed.
- Head repositioning is a slow crossfade, order 0.2 s. **This is not a
  detail to optimise away** - it is where the granular smear comes from, and
  it is most of why the hardware sounds like it does.

Mono in, stereo out: the chain ahead of it is mono, and offsetting the head
positions slightly per channel gives width for free rather than needing a
stereo path all the way back to radix.

**Controls:** TIME, SKEW, SHAPE, FEEDBACK, MIX.
**Jacks:** DLY IN, TIME CV, SKEW CV, FB CV, CLOCK, OUT L, OUT R.


## The chaos bus

This is the Rungler's job in the Manecolin, and olim gets it for free.

bulla already has two runglers, but they are inline in `struct Bulla` and
bulla is released; refactoring shipped code to share three lines is not worth
the risk. olim derives its own from the radix accumulator instead, which is
better anyway: the modulator falls out of the source rather than sitting
beside it.

Three bits off `acc`, weighted newest-heaviest into a 3-bit DAC, smoothed the
way bulla smooths its own. Normalled to filter cutoff through DEPTH, and
broken out at **CV OUT** so it can modulate anything else in the rack.


## Feedback and output

**FB IN** returns the output into radix's modulator input, so the whole chain
closes. With SRC on `IN` this makes the delay's contents drive the
oscillator's increment, which is the howling case and the reason the module
exists.

Saturation before the output, as the Grone has, because the delay's inverted
feedback runs past unity and needs somewhere to go.


## Panel

Thirteen knobs, three switches, sixteen jacks, one CV out, plus level LEDs.
Three vertical stage columns of about 10 HP each: **30 HP (152.4 mm)**, close
to Manecolin's 33. imber is 36 and quadrare is 32, so this is not new ground
for the plugin.

Black, white and yellow, no OCR-A prose inside a stage field: the three
fields and the position of things carry the meaning, in the spirit of
Radical22's own artwork panel. The forsitan logo stays.

Two frictions to settle before drawing anything:

- The stage boundaries and the break jacks **must** be findable. A module
  whose whole premise is breakable normalling, with unmarked jacks, is a
  module nobody breaks the normalling on. The compromise on the table is
  label-free within a field, marked at the boundaries and at every jack, with
  the existing yellow-badge output grammar doing the naming.
- `panel_audit.py` requires a label on every jack and control at >= 2.0 mm
  cap height. A deliberately unlabelled panel fights the tool. Either the
  audit gets a per-module exemption or the panel keeps minimal labels; do not
  discover this after the SVG is drawn.


## Open questions

1. **Buffer length.** Time Machine holds 150 s. At 48 kHz stereo float that
   is 57.6 MB resident, which is a lot for a Rack module. Proposal: 60 s by
   default with a context-menu choice up to 150.
2. **Oversampling.** viginti oversamples with a menu; radix is aliased by
   design and must not be cleaned up. Proposal: oversample the filter only,
   fixed, no menu.
3. **Filter choice**, if the panel runs out of room (see stage 2).
4. **How label-free the panel really is** (see above).
5. Whether RATE tracking V/oct should be exact or deliberately loose. Exact
   makes it playable; loose keeps it feral. Probably a menu option.


## Build order

Each step is a commit, and the module is auditionable before the next starts.

1. `src/olim/radix.hpp` - the engine alone, mono, own clock, with
   `test/olim_probe` measuring spectra per LAW/SRC/TABLE cell. Listen to it
   before anything else is built; if the engine is not interesting on its
   own, nothing downstream will save it.
2. The filter stage, reusing `viginti_dsp.hpp`.
3. The delay stage, with the head crossfade and the spread law.
4. The chaos bus, normalling, and the break jacks.
5. Panel, then `panel_audit.py`.
6. `test/audition/olim.md`, a dozen items, per the audition discipline.
7. `doc/olim.md`, readme row, `plugin.json` entry, glossary entry.

Steps 1 to 3 are independent enough to be judged separately, which is the
point of the break jacks: if the delay is the only good stage, that is a
finding, not a failure.
