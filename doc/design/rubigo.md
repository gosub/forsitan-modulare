# rubigo - design doc

Status: built 2026-09-28, audition pending. The module follows this doc;
`doc/rubigo.md` is the manual. Where building it changed a decision, the doc
says what was decided and why (see "Decided while building" at the end).

A digital percussion voice played by its own generative sequencer. One
oscillator and a noise source (or external audio) go through a driven LP/HP
filter, a digital distortion and a clipping volume stage, each with a
decay-only envelope. Two random generators decide which steps fire and what
value each step carries, and a STEPS knob locks their output into a loop,
then discards and regenerates it piece by piece.

**rubigo** is Latin for "rust". The hardware's distortion switch reads
RUST / CORROSION, and the sound is proudly the decay of a digital signal.


## Lineage, and the rule for building it

rubigo is a clone of Body Synths' **Metal Fetishist** (Xavi Badosa, 2024),
firmware **v2.0** (2025). The firmware is closed, so everything below is
written from these:

- *Metal Fetishist Reference Manual, firmware v2.0*
  (bodysynths.com/resources/mf/manual)
- *Settings Menu Quick-Reference*
  (s3.amazonaws.com/files.bodysynths.com/mf/BodySynths_MetalFetishist_SettingsReference.pdf)
- *Preset Book #1* (bodysynths.com/resources/mf/presetbook1): twelve
  knob-position drawings, no sequences ("sequences cannot be saved").
- The Preset Book's companion video (youtube.com/watch?v=qyCb7s0LCc0),
  which plays the twelve settings on the hardware: the reference every knob
  law was measured against.
- DaisySP (github.com/electro-smith/DaisySP, MIT), the DSP library of the
  hardware's Daisy platform, whose blocks match the manual's descriptions.

Where the manual is silent the choice is ours and marked **inferred**.
`doc/rubigo.md` credits the Metal Fetishist and Body Synths by name.

The whole instrument is cloned, not just the sequencer. On its own the
sequencer is a Turing Machine with a probability gate, which Rack already
has in quantity; what is particular to this machine is one random value per
step landing on internal destinations (a decay time, a crusher, the noise
mix) that no outside voice exposes. Its jacks still make it usable as a
generator for anything else (clock, trigger and step-mod outs), and the
external input makes it a sequenced processor for any source.


## What the hardware does

### Voice

Signal path: **mixer -> filter -> distortion -> volume**.

- **Oscillator**: SAW or SQUARE on a switch. PITCH sets 30 Hz to 1500 Hz.
  Every modulation *adds* to the knob, so the knob is the lowest pitch and
  the result can go well above 1500 Hz.
- **Pitch envelope**: decay-only, ~1 ms attack, exponential fall back to
  the knob. DECAY up to 3 s, AMOUNT up to +600 Hz.
- **NOISE**: a crossfader, oscillator at minimum, white noise at maximum.
  A cable in the external input replaces the noise.
- **Filter**: LP or HP on a switch, CUTOFF and RESONANCE. A fixed drive
  stage sits in front, so nothing passes clean even fully open. Resonance
  self-oscillates: with the sources muted, the manual plays melodies on the
  resonance alone.
- **Cutoff envelope**: decay-only like the pitch one, DECAY up to 3 s,
  AMOUNT. It opens the filter in LP and closes it in HP.
- **Distortion**: CORROSION is a digital overdrive; RUST is the same
  overdrive followed by a downsampler whose rate falls as the knob turns
  up. One knob sets the amount.
- **Volume**: VOLUME is the maximum level and adds asymmetric clipping as
  it rises. The volume envelope is decay-only, up to 3 s, and nothing is
  heard unless it has been triggered.
- **TRIGGER** button, trigger input and sequencer all fire the three
  envelopes together.

### Sequencer

- **Play/stop** switch. **TEMPO** runs an internal clock from 0.4 Hz to
  80 Hz (audio rate). A signal at CLOCK takes over at once, and TEMPO
  becomes a divider / multiplier: /8 /4 /2 x1 x2 x4 x8. When the external
  clock stops, the internal one re-engages after "a few seconds".
- **RANDOM SKIPS**: the percentage of steps that do not fire. 0% fires
  every step. Nudging the knob moves where the triggers fall inside a
  locked loop, so each step holds a random number and the knob is the
  threshold it is compared against, live.
- **RANDOM STEP MOD**: a new random value each step, scaled by the knob and
  sent to PITCH, NOISE or CUTOFF on a three-way switch. For pitch it is
  microtonal: PITCH is the lowest note, the knob sets the highest.
- **STEPS**: OFF lets both generators run free. Turning it up locks their
  values into a loop of 2, 4, 8, 10, 16 or 32 steps. Turning it down
  discards the end of the loop, so going back up regenerates only that
  part. Back to OFF throws everything away.
- Two LEDs: trigger (a step fired) and clock (the sequencer advanced).

### Jacks

The hardware's jacks run 0-5 V, PITCH / NOISE / CUTOFF -5..+5 V.

- **TRIGGER out** (every time the voice fires), **TRIGGER in** (fires the
  voice, independent of the sequencer).
- **CLOCK out** (every step), **CLOCK in** (advances one step in play).
- **STEPMOD out**: the current step value, after the amount knob.
- **PITCH in**, 1 V/oct, summed with the knob and the modulation.
- **NOISE in**, **CUTOFF in**, summed with their knobs. CUTOFF in follows
  the Mod assign menu below.
- **Audio in** (replaces the noise), **audio out**.

### Settings menus (v2.0)

- **Effects**: replaces the distortion. The RUST / CORROSION switch picks
  one of two modes and the knob sets one parameter.
  - Distortion (default).
  - 2nd oscillator, mixed at 50%: CORROSION ranges -1 oct..unison, RUST
    unison..+1 oct.
  - Phaser, flanger, chorus: CORROSION subtle, RUST intense, knob = LFO
    speed.
  - Plus a dry/wet, reached by turning the knob with TRIGGER held,
    fully wet by default on most effects.
- **Mod assign**: the CUTOFF step-mod position and the CUTOFF input drive
  another target instead. The knob becomes the minimum and the modulation
  the maximum: volume decay, pitch decay amount, cutoff decay amount,
  effect parameter. For **volume** it is the other way round: the knob is
  the maximum and the modulation the minimum.
- **Options**:
  - alternative step lengths OFF 3 5 7 12 18 24;
  - restart the sequence on play;
  - do not auto-start when the external clock goes away.
- MIDI channel: not applicable here.


## rubigo

### Controls

The hardware's, one for one:

| control | range (the laws are the measured ones, see "The voice") |
|---|---|
| PITCH | 30..1500 Hz, exponential |
| wave switch | SAW / SQUARE |
| pitch DECAY, AMOUNT | 1 ms..3 s quadratic; 0..+600 Hz quadratic |
| NOISE | 0..1 crossfade |
| CUTOFF, RESONANCE | 20 Hz..16 kHz quadratic; 0..self-oscillation |
| LP / HP switch | |
| cutoff DECAY, AMOUNT | 1 ms..3 s quadratic; 0..+5 kHz linear |
| VOLUME, volume DECAY | 0..1 with clipping; 1 ms..3 s quadratic |
| RUST / CORROSION switch, amount knob | |
| MIX trimpot | the hidden dry/wet, on the panel (default 100%) |
| STEPS | big knob, 7 detents: OFF 2 4 8 10 16 32 (or the alternative set) |
| RANDOM SKIPS | 0..100%, linear |
| RANDOM STEP MOD, destination switch | 0..1 quadratic; PITCH / NOISE / CUTOFF |
| TEMPO | 0.4 + 79.6 k^2.4 Hz; seven zones /8..x8 under external clock |
| play / stop switch, TRIGGER button | |

The dry/wet becomes a trimpot rather than a hold-and-turn gesture, which
has no Rack equivalent. It is one value for every effect rather than one
per effect, so the "wet by default" is one default.

### Jacks

The hardware's ten, at Rack levels:

- TRIGGER, CLOCK outs: 10 V, 1 ms. STEPMOD out: 0..10 V.
- TRIGGER, CLOCK ins: Schmitt at 0.1 / 1 V (as nodi and gradus).
- PITCH in: 1 V/oct. NOISE, CUTOFF ins: +/-5 V covers the full knob travel
  either way (the hardware's range, kept).
- AUDIO in, AUDIO out: +/-5 V.

Three additions, the "faithful plus a few" choice of 2026-09-28:

- **SKIPS CV** and **STEP MOD CV**: 0..10 V adds the full knob travel,
  clamped. The two generators are what one performs, and on the hardware
  they have no jacks.
- **RESET**: a trigger sends the sequence back to its first step at the
  next clock (inferred: the next clock, not at once, so a reset and a clock
  arriving together do not double-step). It is the jack form of the
  "restart on play" option.

MIDI is left out: Rack's MIDI-CV already feeds PITCH and TRIGGER, and the
hardware plays MIDI notes only in stop, which is what those two jacks do.

### Context menu

The hardware's Settings Menu: Effects (5 items), Mod assign (6 items),
Alternative step lengths, Restart on play, Disable auto-start. All saved in
the patch.

### The sequencer, in full (inferred where marked)

A table of 32 slots, each holding two uniform random numbers: `skip` and
`mod`, plus a "valid" flag.

- **OFF**: every step draws a fresh pair and writes it into a ring of the
  last 32 steps played.
- **OFF -> L**: the last L steps played become the loop, in the order they
  were heard. This is what "lock these random values" means to the ear:
  the loop is what was just heard. (**Inferred**: the manual could equally
  mean the next L steps.)
- **L -> L' > L**: slots L..L'-1 were discarded, or never filled, and draw
  fresh values the first time they are reached.
- **L -> L' < L**: slots L'..L-1 are discarded. Going back up refills them
  with new values: "selectively replace parts of our sequence".
- Position wraps modulo the current length. Shortening past the playhead
  wraps it at once.
- A step fires if `skip >= SKIPS`, so SKIPS = 0 fires everything and
  sweeping it moves triggers in and out one slot at a time.
- The step's mod value is `mod * law(STEP MOD)`, read live, so turning the
  amount rescales a locked loop without changing its shape.
- **A skipped step holds the previous step's mod value.** The manual never
  says so directly, but its CLOCK -> TRIGGER self-patch depends on it: the
  voice then fires on every step, and the manual says skips turn into "step
  repetitions" and that SKIPS decides "when the modulation changes vs. when
  it repeats". That only works if a skipped step leaves the value alone.
  STEPMOD out follows the held value too.

Step-mod targets, scaled as measured (see "The voice"):

- PITCH: STEP MOD sets the top note on PITCH's own scale, so the step adds
  up to `pitchHz(STEP MOD) - 30 Hz`: the manual's "PITCH defines the
  lowest, RANDOM STEP MOD the highest". Microtonal by construction.
- NOISE: adds `mod * law(STEP MOD)` to the crossfade.
- CUTOFF: adds `mod * law(STEP MOD) * 5 kHz`, up in LP, down in HP, as the
  envelope.

### Clock (inferred where marked)

- Internal: a phase accumulator at TEMPO, restarted by play.
- External: TEMPO's travel is cut into seven equal zones. Division counts
  edges. Multiplication measures the interval between the last two edges
  and places N-1 steps inside it, re-phased on every edge (**inferred**).
- Auto-return: two seconds without an edge, or four periods of the last
  interval, whichever is longer (**inferred**: "a few seconds"). Disabled
  by the menu option.
- CLOCK out fires on every step, fired or skipped. TRIGGER out fires only
  when the voice fires, from any source.

### The voice: DaisySP's blocks, and laws measured on the hardware

The first build used generic DSP and knob laws guessed from the manual's
ranges, and it sounded nothing like the hardware: an exponential decay law
put the Preset Book kick's volume decay (0.43) at 29 ms, a click. Two
things fixed it.

**The blocks.** The Metal Fetishist runs on an Electrosmith Daisy, and each
block the manual describes matches one in DaisySP, the MIT library that
platform ships with: `Svf`, whose fixed drive scales with the resonance
("a fixed Drive stage that affects the response of the resonance");
`Overdrive`, whose post-gain makes it louder as it drives ("increases the
perceived volume"); `Decimator` for the downsampler; `AdEnv`, which
retriggers from its current value; polyBLEP `Oscillator`; `Phaser`,
`Flanger`, `Chorus`. `src/rubigo/daisy.hpp` ports them, with DaisySP's
notice. That the firmware uses them is inference.

**The laws.** The Preset Book's companion video
(youtube.com/watch?v=qyCb7s0LCc0, one chapter per setting) plays the
book's twelve settings on the hardware, and the knob positions were read
off the book's pages. Rendering the same settings through rubigo and
comparing spectrograms, envelopes and step rates gave:

| law | measured | chosen |
|---|---|---|
| TEMPO | 4.1 Hz at 0.27, 6.2 Hz at 0.355, 2.4 Hz at 0.2 | 0.4 + 79.6 k^2.4 Hz (keeps the manual's ends) |
| DECAYs | quadratic time fits with one curve per envelope | fmap EXP, 1 ms..3 s |
| volume curve | tau 0.14 s at DECAY 0.36, 0.87 s at 0.99 | AdEnv curve -3.5 |
| pitch curve | kick at 125 Hz after 50 ms, 60 Hz after 100 ms | -6 |
| cutoff curve | resonant kick's HP sweep, bassline's closing | -10 |
| pitch AMOUNT | kick starts near 260..350 Hz at 0.81 | quadratic x 600 Hz |
| cutoff envelope | acid peaks near 5 kHz (CUTOFF 0.28, AMOUNT 0.81); bassline near 2 kHz (0.01, 0.33) | adds AMOUNT x 5 kHz, linear |
| HP direction | the resonant kick sweeps up after each hit | envelope and step pull the cutoff down in HP |
| CUTOFF | kick & noise's noise is heard at 0.28 | fmap EXP 20 Hz..16 kHz |
| SKIPS | time-bomb at 0.66 fires one step in three | linear |
| STEP MOD | acid's notes stay under ~200 Hz at 0.42 | quadratic; on PITCH the top note on PITCH's scale |
| RUST | hold rate 6.95 kHz at 0.8, 1.93 kHz at 0.98 | Decimator factor 0.5 k^3 |
| CORROSION | explosions at 0.66 stays dark | Overdrive drive 0.25..0.5 |
| PITCH | 55.6 Hz at 0.2 | exponential 30..1500 Hz |

The book's knob drawings sit on tick marks and the video's player may
touch a knob after the chapter starts, so each figure is good to maybe
10-20%; where two laws fit, the one keeping the manual's ranges won.
**electric failure** does not match: the hardware's HP at CUTOFF 0.28
passes low tones that rubigo's removes, and no single change fits it
without breaking the others. It is left as a known difference.

Other voice details:

- `f = PITCH * 2^Vpitch + env + step`: V/oct scales the knob, and the Hz
  modulations add on top, so a kick keeps its sweep from a keyboard. A hit
  out of silence restarts the oscillator's phase, so every kick has the
  same attack; one landing on a sounding note does not, which would click.
- The Svf's damping reaches zero at full resonance, a lossless loop that
  rings only if something rings it; the hardware's rings with the input
  dead. Over the last 5% of RES the damping goes slightly negative (0.2 at
  the top) and the drive (2, of DaisySP's 0..10) sets the level: about
  0.8 V rms at VOLUME 0.5.
- VOLUME: the VCA, then an asymmetric soft clip driven up to x2.5.
- Effects: phaser 4 poles / depth 0.5 / feedback 0.2 (subtle) or 8 / 0.9 /
  0.7 (intense); flanger delay 0.3 / depth 0.5 / feedback 0.3 or 0.75 /
  0.9 / 0.85; chorus two engines at LFO x1 and x1.37. The knob sets the LFO
  from 0.05 to 8 Hz. These presets are ours; the manual names only
  "subtle" and "intense".
- No oversampling: the hardware is a 48 kHz Daisy and RUST is aliasing on
  purpose.

### Panel

About 20 HP. It keeps the hardware's grouping, which a UX designer laid
out:

- top: effect switch and knob (with MIX), STEPS big and central, volume
  DECAY / VOLUME at the right;
- middle: step-mod switch and knob, the two LEDs, SKIPS, TEMPO, play/stop;
- bottom: wave switch, PITCH, NOISE, LP/HP, CUTOFF, RESONANCE, and the two
  decay/amount pairs, with the TRIGGER button between them;
- jacks in a row along the bottom above the logo rather than in the
  hardware's side block, inputs left, outputs on badges right.

The final layout is done in the panel editor and checked with
`panel_audit.py`.

## Tests

- `test/smoke_rubigo`: silence without a trigger; one trigger decays to
  silence within DECAY; SKIPS 0 fires every step, 100% fires none; a
  locked loop repeats bit-exactly for three passes; shortening and
  lengthening regenerates only the tail; external clock x2 and /2 step
  counts; auto-return after the timeout; RESET lands on step one; no NaN
  under every switch and menu combination at extreme knobs; full
  RESONANCE with the sources muted stays bounded.
- `test/rubigo_probe`: the sequencer, the clock, envelope times, PITCH, the
  filter's self-oscillation, the RUST rate, the reasoned random, a fuzz.
- Audition in `test/audition/rubigo.md`, with the manual's own "try this"
  recipes as the items: kick from the pitch envelope, the resonant melody,
  the drone, the processor, the audio-rate clock self-patch.

## Presets, and the reasoned random

Preset Book #1's twelve settings are the manufacturer's content and are not
copied: no preset reproduces one of them. They were read (knob angles
measured off the page images) for the **principles** they share, and both
our factory presets and a "reasoned random" are built on those.

What the twelve agree on:

- Always playing. STEPS is OFF, 8 or 16; never 2, 4, 10 or 32.
- PITCH is low, 0..0.2 of its travel. Pitch comes from the envelope or the
  step mod, not the knob.
- The pitch envelope is either **off** (DECAY and AMOUNT at zero) or
  **kick-shaped** (DECAY 0.2..0.35, AMOUNT 0.5..0.8). Nothing in between.
- The cutoff envelope is also off or on as a pair; when on, DECAY and
  AMOUNT both sit in the upper half more often than not.
- NOISE is 0, 0.5 or 1. Never a trace of noise.
- RESONANCE is zero unless the filter is the voice (HP, or an acid line),
  and then it is 0.5..1.
- In LP, CUTOFF is low (up to 0.3) unless the sound is noise; HP sits at
  mid travel.
- VOLUME is 0.66..0.8: higher with CORROSION, lower with RUST.
- The effect amount is 0 or at least half; RUST always at least half.
- TEMPO 0.2..0.36 for rhythms. Fast TEMPO with a long volume DECAY is the
  drone and wall recipe.
- SKIPS is zero for continuous material and 0.3..0.8 for rhythms.
- STEP MOD 0.27..1, never a hint. CUTOFF as destination goes with RUST.
- Three of the twelve are self-patched: CLOCK -> TRIGGER, CLOCK -> NOISE,
  and CLOCK -> TRIGGER with STEPMOD -> CUTOFF.

The **reasoned random** is a context menu entry, and also what Rack's own
Randomize (Ctrl-R) does on this module. The uniform randomize lands mostly
on silences and mush: half the knobs have one or two useful regions and
the rest of their travel is the space between them. It first picks an
archetype, then draws every control from that archetype's ranges and
couplings:

| archetype | from the book's |
|---|---|
| KICK | pitch envelope on, low cutoff, short volume decay, skips |
| BASS | pitch step mod, LP with some resonance, 8 or 16 steps |
| NOISE | noise at 0.5..1, cutoff or noise step mod, RUST |
| RESONANT | HP or LP near self-oscillation, cutoff step mod, cutoff envelope |
| DRONE | fast tempo, long volume decay, no skips, STEPS OFF |

The menu offers "any" and each archetype by name. It changes the knobs and
switches and leaves the context menu and the locked sequence alone:
re-rolling the sound over a loop you like is the point.

The factory presets in `presets/rubigo/` are written by
`tools/presets/gen_rubigo_presets.py` from the same archetypes, with our
own names and values.


## Decided while building

- **The loop is saved in the patch** (`sequence` in the module's JSON: the
  32 slots, the length, the position, the held value). The hardware cannot
  keep a sequence, but in Rack a patch that loses its loop on reload would
  be worse than the difference. A preset does not carry a loop, so loading
  one keeps the current loop.
- **A load is not a switch to RUN.** The first sample after the module is
  added or loaded plays a step if the sequencer runs, but does not apply
  "restart on run", and with a clock patched it waits for the clock instead
  of playing off the grid. Found by the JSON round-trip check in
  `smoke_rubigo`, which saw the reloaded loop start one or two steps early.
- **The default TEMPO (0.2) is in the /4 zone** under an external clock,
  as the hardware's would be. The default is chosen for the internal clock
  (2 Hz, a four-on-the-floor kick at 120 BPM), and the tooltip says "/4
  external" when a clock is patched.
- **MIX is on the panel** as a trimpot, one value for every effect,
  default 100%. For the 2nd oscillator it is the second oscillator's level,
  full = the manual's 50/50.
- **The TRIGGER button is a lit bezel** in the accent yellow; its light is
  the hardware's trigger LED. The clock LED sits at TEMPO's corner.
- **The panel** is 20 HP: four rows of knobs and switches on six columns,
  nine inputs in one row, four output badges either side of the logo. The
  two-way switches' positions are labelled above and below; the
  destination switch **pit / nse / cut** has its three in a small column on
  its right, one level with each position, as on spira.
- **Measured**: of 200 uniform draws of every control, 31 are inaudible
  (peak under 0.5 V over 4 s); of 200 reasoned randoms, none
  (`rubigo_probe random`). CPU is 105 to 155 ns a sample, 0.5 to 0.75% of a
  core, depending on the effect (`rubigo_probe cpu`).

