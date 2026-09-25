# radix - design doc

Status: design, nothing built. Written 2026-09-25.

A chaotic 8-bit source on its own panel: an integer machine on its own clock,
whose program is chosen by three stepped knobs with CV. It is stage 1 of
[the three-stage chain](olim-chain.md), built first and shipped alone; the
chain, if it ever comes, takes the engine header and adds nothing to it.

**radix** is Latin for "root": the first stage of the chain, and the thing
every sample grows from, one integer at a time.


## Lineage, and the rule for building it

radix is in the family of Dirty Electronics' **Radical22**, whose firmware is
CC BY-NC-SA 4.0 and cannot be linked into a GPL-3 plugin. The rule from
[olim-chain.md](olim-chain.md#lineage-and-the-rule-for-building-it) applies
unchanged: **implement from this document**, never with `main.c` or `dsp.h`
open, and nothing of the hardware's increment lines, `myarray` bytes or program
map may appear. Credit in `doc/radix.md` is "in the family of", as bulla
credits Hordijk.

The panel takes its idea from Radical22's front, not its artwork: see Panel.


## Engine

As stage 1 of olim-chain.md (per-tick steps, the SRC / LAW / TABLE tables,
`TEXT` from a string set in the context menu). Four of those table entries
measured wrong once written down (ADD, MOD, SRC TABLE, the TEXT scaling), and
PARAM was dead under SELF and IN; `src/radix/radix.hpp` says what replaced each
and is the reference from here on. What changes for the standalone module:

- **SRC, LAW and TABLE are stepped knobs with CV, not switches.** A switch on
  a label-free panel shows nothing, and a knob with CV restores the
  hardware's best trick, sequencing the program, over 5 x 5 x 6 = 150
  programs instead of 64. CV adds to the knob, 1 V per step, and the sum is
  rounded and clamped, so a sequencer's semitone grid does not land on it and
  a slow ramp walks through the positions in order.
- **Program changes are not smoothed.** The accumulator carries across a
  change, so a jump is a discontinuity in the rule, not in the phase. That
  click is the instrument; do not fade it.
- **Pitch belongs to RATE, CLOCK is resolution.** The increment is scaled by
  the engine clock so that, with SRC at `PARAM` and LAW at `ADD`, the pitch
  follows RATE and V/OCT and CLOCK changes only how coarsely it is drawn. On
  the hardware the clock moves the pitch; here that is the `Clock moves
  pitch` context-menu option, off by default. Other LAWs are chaotic and
  track nothing, which is not a fault.
- **No oversampling, no band-limiting, ever.** The aliasing is the sound.
- **CV OUT** is the stepped chaos CV from the chain's chaos bus: three bits
  off the accumulator into a 3-bit DAC, smoothed as bulla smooths its rungler.
  It lives here because it falls out of the accumulator.
- **IN** is both the audio input for SRC = `IN` and the feedback return:
  OUT (or CV OUT) patched back into IN is the chain's FB IN, with a cable.
- Mono. Output is +-5 V.

Open, to settle by ear during step 1:

- whether V/OCT tracking wants a `Loose tracking` menu option (olim-chain.md
  open question 5)
- whether BITS and GRIT deserve CV; not on the list below, and adding them
  later appends to the enums, so the default is to wait


## Controls

| knob | range | CV |
|---|---|---|
| RATE | pitch, V/oct | V/OCT |
| PARAM | 0-255 | PARAM CV |
| CLOCK | 100 Hz - 96 kHz, exponential | CLOCK CV |
| BITS | 16 down to 1 | - |
| GRIT | 0-100% | - |
| SRC | 5 steps | SRC CV |
| LAW | 5 steps | LAW CV |
| TABLE | 6 steps | TABLE CV |

Eight knobs; jacks V/OCT, PARAM CV, CLOCK CV, SRC CV, LAW CV, TABLE CV, IN,
OUT, CV OUT: seven in, two out. Every one of them has a full `configParam` /
`configInput` / `configOutput` name and, for the stepped knobs, a
`configSwitch` label per position: on a label-free panel the hover tooltip
is the only legend, so it has to be complete.

Context menu: the `TEXT` string, `Clock moves pitch`, and anything step 1
adds.


## Panel

**12 HP (60.96 mm)**, the Radical22's width.

**Label-free.** No labels, no output badges with text, no title. The
forsitan logo stays, at its usual `width/2`, y = 122.5, and is the only
fixed, upright element. The module browser and the hover tooltips carry the
names.

**Tilted.** The controls sit on a lattice rotated 12 degrees off the panel
axis, as the Radical22's jacks rise along a diagonal. Built: three blocks,
each a row of knobs with their CV jacks directly under them, zigzagging down
the panel - RATE / PARAM / CLOCK at the top left, SRC / LAW / TABLE in the
middle right, BITS and GRIT over IN, OUT and CV OUT at the bottom left. The
two-rows-of-jacks proposal lost to this because a knob beside its own CV is
the only grouping a label-free panel has left.

**Findable.** The panel is label-free, not unreadable:

- every control and jack sits on a plain disc cut out of the artwork, dark
  for inputs and knobs
- the two outputs sit on **yellow** discs, the output-badge grammar with the
  text taken out, so what comes out is visible at a glance
- the stepped knobs get tick marks on their disc, one per position, so the
  step count reads without a label

**Artwork.** Full-bleed, generated, black / white / yellow on the `#1a1a1a`
ground. It comes from its own generative process, not from the engine: a
branching growth (space colonisation, or cracks propagating from seeds)
drawn as filled shapes, in the spirit of the Radical22's branches without
copying the photo. Requirements on the generator:

- `tools/panels/gen_radix_panel.py`, deterministic from a seed in the file,
  so the panel regenerates identically; nothing in `res/` made by hand
- **NanoSVG-safe**: filled paths only, no filters, gradients, masks or
  clip paths, and merged into a few compound paths rather than thousands of
  small ones, with a node budget measured against the load time of the
  heaviest existing panel
- keeps out of the screws and the logo, and grows **under** the controls,
  with the opaque discs drawn over it. Keeping it out of the discs by
  construction was the plan and was tried first: a branch cannot find its
  way into the narrow gaps inside a block of knobs and jacks, so every block
  sat in a black blob several times its own size. Falling density near the
  controls went for the same reason.

As built: gold limbs, a white tangle over them and black cracks through
both, each layer one compound path under the nonzero rule, 220 KB against
imber's 279. It renders in Rack as drawn (`gen_screenshots.py radix`).

**panel_audit.** It does not require a label on every control (it checks
overlaps and label clearances only), so the label-free panel passes as is.
`notitle` on `@layout:begin` drops the title from the audit, and
`svg=tools/panels/gen_radix_panel.py` makes a Save in the panel editor rerun
the generator instead of drawing the standard panel over the art. The tilt
needs nothing: positions are stored as rotated centres.


## Build order

Each step is a commit, and the module is listenable before the next starts.

1. `src/radix/radix.hpp` - the engine alone, own clock, ZOH, all 150
   programs, with `test/radix_probe measure` reporting per-program level, DC
   and spectral centroid, and flagging silent or stuck programs. Listen
   before going on: if the engine is not interesting alone, stop here.
2. `src/radix.cpp` with a placeholder panel: knobs, CVs, the stepped-knob
   rounding, CV OUT, context menu. `test/smoke_radix`.
3. `tools/panels/gen_radix_panel.py`, the tilted layout, the artwork;
   `panel_audit.py` changes if needed.
4. `test/audition/radix.md`, a dozen items.
5. `doc/radix.md`, readme row, `plugin.json` entry, glossary entry,
   CLAUDE.md table row. radix is a new module, so it goes out in a minor
   version.
