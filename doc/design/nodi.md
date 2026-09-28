# nodi - design doc

Status: built 2026-09-26, audition passed 2026-09-28 (all 19 items; the
three Decide items kept as built: the 2 ms Auto rule's handover at 62.5 Hz
is inaudible, so is the four-sample overlap in a sixteen-step chain, and
the threshold lights read as a picture of X). The module follows this doc; `doc/nodi.md` is
the manual. Where building it changed a decision, the doc says what was
decided and why.

A continuous-time sequencer: eight thresholds turn the motion of a voltage X
into the activation of one of eight stages, and each stage puts out its own
voltage. With a ramp at X it is a step sequencer whose sliders set *when*
each step happens; with an LFO, an envelope or a random voltage it is an
event generator that follows that signal; with a keyboard's V/oct it is a
quantizer or a harmoniser; with an audio-rate ramp it is a graphic VCO; with
audio at X it is a waveshaper.

**nodi** is Latin for "knots", plural of *nodus*. The hardware's manual
opens on Hegel's "nodal lines of measure relations" (*Knotenlinie*): a
quantity changes smoothly until, at a node, it turns into a different
quality. Eight thresholds are eight such nodes.


## Lineage, and the rule for building it

nodi is a clone of New Systems Instruments' **Discrete Map** (34 HP, 2024),
with its **A / B / C Expander** (10 HP) built in. The hardware has no
firmware: it is comparators, logic and analog sliders. So there is nothing
to read but the manual (*Discrete Map and A/B/C Expander, Manual Revision
1.0*, nsinstruments.com/modules/dmap.html), and everything below is written
from it. Where the manual is silent the choice is ours and marked
**inferred**.

`doc/nodi.md` credits the Discrete Map and NSI by name.


## What the hardware does

Three sections, joined by one normalled cable.

### Clock section

A ramp from -5 V to +5 V, out on RAMP and normalled to X.

- **RATE**: SLOW is 4 minutes per cycle to 9 Hz, FAST is 4 Hz to 12 kHz.
- **V/O** tracks about 7 octaves; **RATE CV** with its attenuator modulates
  the rate *linearly* (linear FM at audio rate).
- **SYNC**: a rising edge (1 V -> 4 V) resets the ramp to -5 V at once.
- **SYNC/N** counts edges and resets on the N-th, N = 1..16 on a knob. A
  SYNC edge also clears the count.
- **LOOP / ONCE**: in ONCE the ramp stops *just above* +5 V and waits for a
  sync. Switched back to LOOP, a waiting ramp restarts at once (no second
  EOC: it fired when the ramp stopped).
- **EOC** fires a trigger when the ramp resets or reaches the end of a
  cycle.
- On reset the ramp **overshoots below -5 V** for a moment, so a rising
  threshold sitting at the bottom always fires. There is no overshoot at the
  top, hence a small dead zone there.

### Event section

- **Eight threshold sliders**, each with a **RISE / OFF / FALL** switch. A
  RISE stage activates when X rises past its threshold, a FALL stage when X
  falls past it; the other direction does nothing. One stage is active at a
  time, the last one tripped, and it stays active until another trips.
- **POSIT. / LENGTH**. In POSIT. each slider places its threshold directly,
  anywhere in the space, in no particular order. In LENGTH the first
  threshold sits at the bottom and each slider is the *relative* length of
  the interval above its threshold, normalised so the eight add up to the
  whole space. An OFF stage keeps its length; a slider at zero removes the
  stage.
- **ABOVE / BELOW** set the top and bottom of that space, +5 V / -5 V when
  unpatched. On the hardware they are the two ends of the slider ladder,
  so ABOVE of one module into BELOW of the next joins their sliders into one
  ladder of sixteen.
- **EXT**: a gate there cancels the active stage (none is active, f(X)
  drops to 0 V) and produces a gate. It is ignored for 12.5 us after the
  module's own stage activations, so two modules can cancel each other
  through GATE -> EXT without cancelling themselves.
- An **X indicator**, a column of LEDs from BELOW to ABOVE.

### Map section

- **Eight f(X) sliders**, one per stage, each with a stage LED.
- **RANGE**: -5..+5 V, 0..5 V, or 0..2.5 V.
- **f(X) out** = the active stage's voltage + **+Y** (a precision adder,
  for transposing or for summing modules). 0 V + Y when no stage is active.
- **GATE** out: 5 V on every stage change or EXT, for **GATE LEN.** 6 us to
  2.8 s. A new gate before the old one ends restarts the timer, so the gate
  carries over.

### A / B / C Expander

- Eight **group switches** put each stage in group A, B or C.
- **THRESHOLD A / B / C** inputs, each with an attenuator, *add* to the
  thresholds of every stage in their group, in volts, in either mode. Steps
  move past each other; a step pushed past the end of the ramp never fires.
- **GATE A / B / C** outputs: the gate, for stages of that group only.
- A **sequential switch**: COM is connected to A, B or C while a stage of
  that group is active, and to nothing when none is. It is bidirectional.
  At audio rate this is a *temporal mixer*.


## Engine

`src/nodi/nodi.hpp`, header-only, `namespace nodi`, no Rack dependency, so
the probe links it alone.

### Crossings as events, not samples

The comparators see continuous time, so the engine must too. Per sample it
walks X's **path** from the last sample to this one and finds every
threshold crossing on it, with its fractional time:

- **External X**: one straight segment between the two samples.
- **Internal ramp**: the exact path, which can be up to three segments in
  one sample: up to the top, the reset jump down to the overshoot, and up
  again. The reset is a *falling* sweep through the whole space, so every
  FALL stage fires on it in top-to-bottom order and the lowest wins. This is
  why the manual's step sequencer sets stage 1 to FALL.
- **Moving thresholds** (sliders, THRESHOLD CV) cross X as well: what is
  tested is the sign of `X - threshold`, interpolated across the sample, so
  a threshold swept under a still X trips its stage exactly as it would on
  the hardware.

Crossings are ordered by time; coincident ones in the order of travel
(rising: lowest threshold first). The last one decides the active stage.
Every activation produces a gate, **including a stage re-activating itself**
(**inferred**: "whenever a stage is activated"; it makes a single RISE stage
a comparator with a gate out).

A stage fires **exactly at its threshold**, then re-arms for that direction
only after X has been **5 mV** past it the other way (**inferred**, and our
only hysteresis): a digital comparator on a noisy signal would otherwise
fire every sample. A first draft put the trip points 2.5 mV either side of
the threshold instead, which delayed every step by that much travel: 60 ms a
step on a 4-minute ramp. And a crossing that lands **5 mV or more past** the
threshold fires even unarmed: without that, X parked inside the band and
then sent across by a sample-and-hold was missed (3 wrong notes in 2000 on
the quantizer setup, 0 in 20000 after).

An external X that has not moved since the module started has crossed
nothing, so no stage is active until it does, as on the hardware.

### Where it has to differ from the hardware

- **The ends of the space.** Thresholds are kept 10 mV (twice the
  hysteresis) inside BELOW..ABOVE, before the THR CV is added. In LENGTH
  stage 1 sits on the bottom rail, and a Rack signal reaching exactly
  -5 V (the Fundamental LFO, any VCO) touched it without crossing, so
  stage 1 never played from anything but the internal ramp (found in the
  audition, item 2.2). The cost is 1/1000 of the span at each end: a RISE
  stage at the bottom fires that far into the ramp rather than on the
  reset, and in a sixteen-step chain the handover step is 48 samples
  longer, in a 48000-sample cycle.
- **The ramp's overshoot** is a tiny fixed undershoot of the reset jump,
  so every FALL threshold fires on the reset. ONCE parks just above +5 V,
  as the hardware does. There is **no dead zone at the top**: a threshold
  at the top fires. The hardware's dead zone is an analog imperfection,
  not a behaviour.
- **EXT lockout.** Every Rack cable is one sample of delay and every nodi
  output one more (see Aliasing), so the GATE -> EXT round trip between two
  modules is four samples, not a few microseconds. The lockout is therefore
  **8 samples** rather than 12.5 us, which keeps the 16-step patch working.
  The same delays leave both modules active for **4 samples** at each
  handover, which `nodi_probe ext` measures and the audition asks about.
- **Gate length** runs from **one sample** (6 us is shorter than a sample)
  to 2.8 s.
- **Bidirectional switch.** A Rack jack is an input or an output. nodi's
  switch is **A / B / C in, COM out**: the sequential switch and the
  temporal mixer. The reverse, one signal to three places, is the three
  GATE outs into three VCAs.
- **Joined ladders.** ABOVE and BELOW are inputs here, so ABOVE -> BELOW
  between two nodi does not join their sliders proportionally. POSIT.
  splitting (-5..0 V on one, 0..+5 V on the other) works by patching the
  voltages. A true join, by placing two nodi side by side, is possible
  through Rack's expander mechanism and is **left for later**.
- **Analog imperfection** (slider mismatch, the top dead zone): not
  modelled.

### Aliasing

At audio rate f(X) is a stepped wave, RAMP a saw and COM a hard switch:
each would alias badly as naive samples. The crossing times are already
known to a fraction of a sample, so each step can be rounded at its exact
time.

The draft said minBLEP. What was built is a two-sample **polyBLEP**, as
tundo, scrupea and aether use: it keeps the header free of Rack, where
Rack's MinBlepGenerator lives, and `nodi_probe alias` shows it takes 17 to
35 dB off the alias energy. A polyBLEP corrects the sample before a step
too, so **every output leaves one sample late**, gates included, and a gate
and its voltage still arrive together.

A rounded step is not exactly a stage's voltage for a sample, which is
wrong where a sample-and-hold clocked by GATE is looking. So the rule is:
**a step is rounded when it comes within 2 ms of the last one on that
output** (500 Hz and up), and is a plain step otherwise, decided **once per
sample**: a jump across three thresholds is three steps in one sample, and
rounding the second and third for following the first smeared a
quantizer's output. A sequence stays exact; an oscillator is clean. A
context-menu **Anti-aliasing** item offers Auto (that rule, the default) /
Off / On.

### Polyphony

**X is polyphonic** (**not** in the hardware): each channel keeps its own
active stage, so a chord at X comes out mapped note by note, which makes the
quantizer / harmoniser use polyphonic. f(X), GATE, GATE A/B/C and COM follow
X's channel count; +Y, EXT, ABOVE, BELOW and THRESHOLD A/B/C are read per
channel (a mono cable applies to all). The internal clock stays one ramp.
The stage lights show channel 1.


## Controls and jacks

| param | range | notes |
|---|---|---|
| RATE | SLOW 1/240..9 Hz, FAST 4 Hz..12 kHz | exponential, displays Hz or s |
| FM | 0..1 | attenuator on RATE CV |
| N | 1..16 | SYNC/N count, snapped |
| LOOP / ONCE | switch | |
| SLOW / FAST | switch | |
| GATE LEN | 1 sample .. 2.8 s | exponential |
| RANGE | +-5 / +5 / +2.5 V | 3-way switch |
| POSIT / LENGTH | switch | |
| f(X) 1..8 | 0..1 | lit sliders, lit when their stage is active |
| X 1..8 | 0..1 | threshold sliders |
| DIR 1..8 | RISE / OFF / FALL | 3-way switches |
| GROUP 1..8 | A / B / C | 3-way switches |
| THR A / B / C | 0..1 | attenuators |

| input | notes |
|---|---|
| V/O | 1 V/oct on the rate |
| FM | linear rate modulation, +5 V at full attenuator doubles the rate; the rate stops at 0, it does not go through zero |
| SYNC, SYNC/N | rising edges, Schmitt 1 V / 4 V |
| X | poly, normalled to RAMP |
| ABOVE, BELOW | top and bottom of the threshold space, +5 / -5 V unpatched |
| EXT | rising edge cancels the stage |
| +Y | added to f(X) |
| THR A / B / C | volts added to the group's thresholds, through the attenuator |
| A / B / C | the switch's inputs |

Outputs: RAMP, EOC, f(X), GATE, GATE A / B / C, COM.

The shapes live in `src/nodi/shapes.hpp`, beside the engine.

Context menu: **Anti-aliasing** Auto / Off / On, and five submenus that set
many controls at once. Each is **one undo step**, as olim's heads menus are,
and the shapes live in the engine header so the probe can check them.

- **Setups**: the manual's quick-start patches as whole-module settings
  (variable-length and variable-position sequencer, quantizer, graphic VCO,
  bitcrusher, temporal mixer): mode, clock, range, both slider banks and
  both switch banks.
- **f(X)**: presets are scales and shapes, landing on exact semitones in
  the current RANGE (chromatic, major, minor, the pentatonics, a triad and
  a seventh arpeggio, octaves, up and down ramps, all at zero, random in
  a scale); transforms are reverse, rotate left / right, mirror, transpose
  a semitone / an octave up and down, snap to semitones, shuffle and
  mutate.
- **Thresholds**: presets read in the current mode (even, swing, 3-3-2,
  accelerando, ritardando, random; in POSIT. "even" is the diagonal);
  transforms are reverse, rotate, mutate, and **LENGTH <-> POSIT.**
  conversion, which flips the switch and rewrites the sliders so every
  threshold stays where it was (from LENGTH always; from POSIT. only the
  stages that are in ascending order). A stage below one before it gets
  zero length *onto the next stage in order*: at coincident thresholds the
  higher stage wins on the way up, so it is the out-of-order stage that is
  passed over. The first version raised it onto the stage before instead,
  which silenced that one.
- **Directions**: all RISE, all FALL, the sequencer (1 FALL, rest RISE),
  the bitcrusher (four RISE, four FALL), alternate, reverse, rotate.
- **Groups**: all A, A B C cycling, pairs, thirds, random, rotate.

Defaults are the manual's first quick start: LENGTH, SLOW, LOOP, RANGE
+2.5 V, every length at half, stage 1 FALL and the rest RISE. Out of the box
it is an eight-step sequencer.


## Panel

The hardware is 34 + 10 = 44 HP. nodi aims at **24 HP** (121.92 mm).

The eight stages are columns in the middle, 8.5 mm apart (a slider is
6.72 mm wide; that leaves the 1.5 mm clearance), so the block is 66 mm wide.
Top to bottom in each column: the f(X) slider, lit by its stage; the stage
number; the threshold slider; the RISE / OFF / FALL switch; the A / B / C
switch. The switch rows are legended once, at the left of the block (rise /
fall, a / c at the ends of the throw).

The **X indicator** is not a separate widget: the threshold sliders are lit,
faintly while X is above their threshold and fully for 50 ms when they
fire. With the ramp at X they fill up like a bar graph and empty at the
reset, which is the hardware's LED column drawn where the thresholds are.

That leaves about 28 mm on each side and a band under the block, for 23
jacks, 7 knobs, 4 switches and a button:

- **left**, the clock: RATE, SLOW / FAST, FM attenuator and jack, V/O,
  SYNC, N and /N, LOOP / ONCE, TRIG (a button OR'd with SYNC), then EOC
  and RAMP out level with the switch inputs;
- **right**, POS / LEN, X, EXT, HI, LO, +Y and RANGE; then the outputs in
  badges, GATE A / B / C, f(X) and GATE, with DUR (the gate length) in the
  sixth place beside them, the bottom pair level with the switch inputs;
- **the band under the stages**: THR A / B / C, each an attenuator and a
  jack, then the switch, A and B left of the logo, C and COM right of it.

It fits at **24 HP** with `panel_audit.py` clean; the labels are short
(OCR-A is 2.24 mm a character at the house size), so ABOVE and BELOW read
**hi** and **lo**, and the gate length **dur**. The bottom row's badges are
13.3 mm tall rather than 14, to clear the screws.


## Tests

- `test/nodi_probe`:
  - `cross` - activation times against a ramp of known rate: every stage
    within a sample of where its slider puts it, in both modes; the reset
    order (FALL stages top to bottom, a RISE at the bottom after them); a
    threshold swept under a still X.
  - `length` - thresholds from lengths: sum to the space, OFF keeps its
    length, zero removes the stage, ABOVE / BELOW rescale.
  - `ext` - cancel, the gate it makes, the lockout, and two engines
    cross-patched with one-sample cables as a 16-step sequencer.
  - `alias` - the graphic VCO at 1, 3 and 8 kHz: aliased energy with
    minBLEP against without; and the Auto rule leaving a slow sequence's
    steps exact.
  - `cpu`, and `wav <dir>` for listening.
- `test/smoke_nodi`: construction, NaN and silence, poly channel counts,
  the RAMP -> X normal, the defaults being a running sequencer.
- `test/audition/nodi.md`, 19 items.

CPU (`nodi_probe cpu`): 62 ns a sample mono, 0.30% of a 48 kHz sample; 690
ns at 16 channels, 3.3%. About 40 ns of it is per channel.


## Build order

Each step is a commit.

1. `src/nodi/nodi.hpp` and `test/nodi_probe`.
2. `src/nodi.cpp` on a placeholder panel, context menu, `test/smoke_nodi`.
3. The panel, `panel_audit.py`.
4. Setups, presets and transforms in the context menu.
5. `test/audition/nodi.md`.
6. `doc/nodi.md`, readme row, `plugin.json` entry, glossary entry, CLAUDE.md
   table row. A new module, so a minor version.
