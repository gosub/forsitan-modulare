# rubigo - design doc

Status: design, 2026-09-28. Nothing built yet.

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
written from the manual:

- *Metal Fetishist Reference Manual, firmware v2.0*
  (bodysynths.com/resources/mf/manual)
- *Settings Menu Quick-Reference*
  (s3.amazonaws.com/files.bodysynths.com/mf/BodySynths_MetalFetishist_SettingsReference.pdf)
- *Preset Book #1* (bodysynths.com/resources/mf/presetbook1): twelve
  knob-position drawings, no sequences ("sequences cannot be saved").

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

| control | range |
|---|---|
| PITCH | 30..1500 Hz, exponential |
| wave switch | SAW / SQUARE |
| pitch DECAY, AMOUNT | 1 ms..3 s, 0..+600 Hz |
| NOISE | 0..1 crossfade |
| CUTOFF, RESONANCE | 20 Hz..20 kHz exponential; 0..self-oscillation |
| LP / HP switch | |
| cutoff DECAY, AMOUNT | 1 ms..3 s; 0..full range (inferred: +/- 8 octaves) |
| VOLUME, volume DECAY | 0..1 with clipping; 1 ms..3 s |
| RUST / CORROSION switch, amount knob | |
| MIX trimpot | the hidden dry/wet, on the panel (default 100%) |
| STEPS | big knob, 7 detents: OFF 2 4 8 10 16 32 (or the alternative set) |
| RANDOM SKIPS | 0..100% |
| RANDOM STEP MOD, destination switch | 0..1; PITCH / NOISE / CUTOFF |
| TEMPO | 0.4..80 Hz exponential; seven zones /8..x8 under external clock |
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
- The step's mod value is `mod * STEP MOD`, read live, so turning the
  amount rescales a locked loop without changing its shape.
- **A skipped step holds the previous step's mod value.** The manual never
  says so directly, but its CLOCK -> TRIGGER self-patch depends on it: the
  voice then fires on every step, and the manual says skips turn into "step
  repetitions" and that SKIPS decides "when the modulation changes vs. when
  it repeats". That only works if a skipped step leaves the value alone.
  STEPMOD out follows the held value too.

Step-mod targets, all **inferred** in scale:

- PITCH: adds `mod * amount * 1500 Hz`, linear in Hz, as everything else on
  the pitch is. Microtonal by construction.
- NOISE: adds `mod * amount` to the crossfade.
- CUTOFF: adds `mod * amount * 8` octaves (towards open in LP, towards
  closed in HP, as the envelope).

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

### Voice DSP (all inferred, the manual gives only the ranges)

- **Oscillator**: polyBLEP saw / square. `f = PITCH * 2^Vpitch + env +
  stepmod`: V/oct scales the knob, and the Hz modulations add on top, so a
  kick keeps its sweep when played from a keyboard.
- **Envelopes**: 1 ms linear attack, then `exp(-t / tau)` with tau such
  that the curve reaches -60 dB at the DECAY time. Retrigger restarts from
  the current value, not zero, so fast steps do not click.
- **Noise**: white, from the module's own xorshift.
- **Filter**: ZDF state-variable filter with a tanh stage before it
  (fixed drive, about +6 dB) and a soft limit inside the resonance loop,
  so full RESONANCE rings with no input and stays bounded.
- **CORROSION**: gain 1..40 into an asymmetric clipper, with level
  compensation part of the way ("increases the perceived volume").
- **RUST**: the same, then sample-and-hold from the engine rate down to
  about 400 Hz, exponential on the knob, with no anti-aliasing: the
  aliasing is the point.
- **Volume**: `VOLUME` drives an asymmetric soft clipper whose drive grows
  with the knob, then the envelope multiplies.
- **Effects**: phaser (6 allpass stages), flanger (0.5..5 ms with
  feedback), chorus (two taps 10..25 ms), each with a subtle and an intense
  preset of depth / feedback; the knob sets the LFO from 0.05 to 8 Hz. 2nd
  oscillator: the same waveform, at `2^(-1..0)` or `2^(0..1)` times the
  first.

Oversampling: none by default. The hardware is a 48 kHz Daisy with an
audible digital edge, and RUST throws away more than any oversampling
would keep. If the CORROSION stage measures ugly at 44.1 kHz without RUST,
a 2x menu option is the fallback, as on raucus.

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
- `test/rubigo_probe measure`: envelope times, pitch sweep, the filter's
  self-oscillation pitch against CUTOFF, the RUST rate against its knob.
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
