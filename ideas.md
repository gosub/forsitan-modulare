# forsitan modulare — module ideas

Brainstorm from 2026-07-10/11, sorted best to worst. Novelty was checked
against the VCV Library at the time of writing; re-check before starting
anything.

## 1. rete — feedback integrator network (IMPLEMENTED in v2.7.0)

Port of Nathan Ho's "feedback integrator networks" (FIN): N leaky
integrators into a fixed random N×N mixing matrix, then LeakDC highpass,
then clipper, fed back with a one-sample delay. Self-oscillating instrument
drifting between tonal and chaotic.

- Interface (per Ho's own advice): fixed random matrix with a RANDOMIZE
  button + trigger input (seed saved in patch JSON), 8 per-node gain
  knobs + CV inputs, leak amount, clip drive, excite input, stereo mix out,
  poly out of all 8 nodes (pavo does the Splay from his SC code).
- The SC version needs blockSize=1; Rack is single-sample natively, so the
  port is easier than the original. 8×8 is ~64 mults/sample, negligible CPU.
- Port block order verbatim first (integrator → matrix → HPF → clip), it
  came from trial and error. Leak/LeakDC coefficients are SR-dependent.
- Technique, not licensed code; credit the post, courtesy email (Nathan Ho
  = snappizz of the SC community).
- Source: https://nathan.ho.name/posts/feedback-integrator-networks/
- Nothing comparable in the library; closest are single-equation chaos
  oscillators (HetrickCV FBSineChaos, Nonlinear Circuits) and hand-patched
  no-input matrix mixing.

## 2. ululo — feedback guitar instrument (IMPLEMENTED in v2.7.0)

From Nathaniel Virgo's "Guitar feedback emulation" (https://sccode.org/1-U):
guitar held up to the amp. Delay (amp distance) + six comb filters (strings)
+ LPF/HPF + distortion, closed in a feedback loop.

- Tune the comb bank from poly V/oct: interea plays chords on feedback.
- Whammy CV, distance knob, drive. Small DSP, big character.
- Nothing like it in the library despite Rack's guitarist audience.
- sccode posts carry no license: ping the author before porting.

## 3. campanae — change-ringing sequencer

Permutation sequencer from English change ringing (Plain Hunt, Plain Bob,
Grandsire, Stedman). Pick bell count and method; rows emit pitch CV + gate
per bell or one poly pair. "Calls" (Bob/Single) as trigger inputs alter the
permutation path.

- 400-year-old algorithmic music tradition, maps 1:1 onto CV/gate.
- Pure combinatorics, cheap to build on existing sequencer plumbing.
- Nothing in the library (Grayscale Permutation is unrelated random seq).

## 4. tela — weaving-draft sequencer

Weaving drafts (threading sequence × treadling sequence through a tie-up
matrix) as a gate sequencer: shafts are output channels, treadling is the
clock dimension. Centuries of published drafts (twills, overshots, huck
lace) become a preset library.

- Panel could render the drawdown (cloth pattern) as it plays.
- Tiny data, rich non-obvious patterns; nothing weaving-shaped in the
  library. Latin bonus: tela is both loom and web.
- Would pair with campanae as a "pre-electronic pattern traditions" release.

## 5. tabes — disintegration looper (IMPLEMENTED in v2.7.0)

Tape loop that ages each pass: HF loss, wow, dropouts, grit, with a decay
rate knob and a splice control. Basinski's Disintegration Loops as a module.

- People build this by hand with long effect chains (MOD WIGGLER / KVR
  threads); library has plain loopers (Lilac, Voxglitch) but no decaying one.
- Widest audience appeal of the batch; pairs with draen for the drone crowd.
- PT2399 work supplies the lo-fi DSP vocabulary.

## 6. clepsydra — water-clock rhythm generator

Cascade of vessels: each fills at its own CV-controllable rate, tips when
full, fires a trigger, pours into the vessel below. Organic polyrhythms with
physical logic; per-vessel leak for patterns that never lock.

- Adjacent to cumuli (accumulate-and-overflow) but aimed at rhythm.
- Ancient-timekeeping counterpart to solarium. Nothing vessel-based in the
  library; existing "organic clocks" are just jitter-on-a-grid.

## 7. officina — Radiophonic Workshop swoosh box

From Nathaniel Virgo's "Radiophoni" (https://sccode.org/1-S): frequency
shifter inside a feedback loop, seeded by band-passed noise bursts. Endless
rising/falling BBC sci-fi effects.

- Controls: shift amount (bipolar), regeneration, band center, excite in.
- Freq shifters exist in the library; the packaged feedback instrument
  does not. Tiny module, good companion to ululo/rete in a feedback release.

## 8. necto — random cable patcher / patch mutator

Sibling to alea: adds or mutates cables instead of modules. limen already
taught this codebase the engine API for modules, ports, and cables.

- Type awareness is what makes it good, not a toy: classify ports
  (audio/CV/gate) by heuristics, only plausible connections, optional
  attenuation on what it patches.
- WhatTheRack spawns random modules (like alea); nothing in the library
  mutates wiring. alea + necto = generative-patch ecosystem story.

## 9. hydraulis — Roman water organ voice

Drone voice modeled on the hydraulis (oldest keyboard instrument): pipe
ranks on a shared unstable wind supply, valve chuffs, pressure sag when many
notes sound at once.

- The shared-wind coupling (more notes starve them all) is the musically
  interesting part. Sits next to draen; nothing similar in the library.

## 10. tempestas — weather-system modulation source

Coupled simulation of pressure, temperature, wind, cloud cover as slow
correlated CVs, plus event triggers (gust, rain starts, thunder).

- The point is correlation: outputs move like facets of one system, which
  no uncorrelated LFO/S&H bank gives you.
- Library "Random" tag is all uncorrelated sources. Barometer-needle panel
  widget would be very forsitan.

## 11. molecula — Molecular Music Box sequencer

The Molecular Music Box algorithm (grirgz's SC take:
https://sccode.org/1-4Wx): two note durations + a seed like "4E3"; notes
accumulate into loops of different lengths that phase Reich-style.

- Module: two duration knobs, seed, scale, poly CV/gate out.
- Simple rules, rich output, nothing in the library.

## 12. solarium — sundial / real-time modulation

CVs derived from wall clock and date: time of day, day length, sun elevation
for a configurable latitude, season, moon phase. Patches sound different at
dawn than at midnight.

- Very cheap to build; nothing in the library touches real-world time as a
  modulation source (as far as checked).

## 13. brevitas — SC-tweet engine bank

The draen move again: a curated bank of famous 140-character SuperCollider
pieces (SC Tweets collections: https://sccode.org/1-4RA,
https://sccode.org/1-5eN) as selectable engines with fading engine select.

- Architecture already exists (draen). Museum piece of SC culture.
- Ranked down for the licensing burden: many tweets, many authors, sccode
  posts mostly unlicensed. Needs per-author permission or a strict subset.

## 14. murmuratio — starling-flock modulation source

Boids as a polyphonic CV source: each poly channel is one bird (x/y or
heading/speed), plus flock density and centroid outputs.

- Correlated-but-individual motion; poly cousin of tempestas. Pairs with
  pavo (spread the flock across the stereo field).
- Sha#Bang! Photron uses boids but only to animate panel colors; the
  CV-source niche is open.

## 15. tessera — Wang-tile pattern sequencer

Wang tiles (edge-matching turns random placement into coherent structure)
mapped to pitch/gate/CV. Generative sequences that are neither looped nor
random, with audible local logic.

- Nobody in the library uses tiling algorithms; great name (mosaic tile,
  Roman watchword token).
- Most experimental of the batch; needs good sonification choices to not
  feel academic.

## 16. tali — knucklebone dice

Small companion to alea: random source with the historically documented
unequal face probabilities of Roman knucklebones (~4/10 flat sides, ~1/10
narrow). Throw four at once; sum as CV; named-throw triggers (Venus: all
different, Canis: all ones).

- Weekend module. "Weighted random with 2000-year-old weights" is a good
  forsitan joke; alea iacta est demands it.

## 17. epicyclus — Ptolemaic LFO

Deferent plus epicycles: stacked rotating circles, each with rate, radius,
direction; the traced point's x/y are two CV outs. Looping-but-complex
shapes with retrograde swerves; the panel visual explains itself.

- Caveat: under the hood it is additive sine LFOs with phase coupling, so
  the least novel DSP here; concept, geometry, and 2D output carry it.

## 18. lustro — scanned filter (scando sibling) (IMPLEMENTED in v2.7.0)

Reuse the scando mass-spring engine as the control surface of a resonant
filterbank: band frequencies/gains ride the moving string, external audio
gets "played" by the physics.

- Novel and on-brand (scanned synthesis is already the house niche), but
  speculative: needs prototyping to know if it sounds as good as it reads.

## 19. bulla — Blippoo Box (IMPLEMENTED in v2.7.0)

Port of Rob Hordijk's Blippoo Box (olaf's SC implementation:
https://sccode.org/1-5bB): two oscillators, rungler shift register, peak
filter in a twisted cross-modulation topology.

- Honest caveat: Benjolin clones already occupy adjacent territory in the
  library; the Blippoo itself was not found, but novelty is the weakest of
  the list. Chaos-instrument fans are completionists, though.

## Considered and dropped

- **Gendy / dynamic stochastic synthesis**: already in the library (docB
  Gendy, sb-StochKit Grandy).
- **Bytebeat / BetaBlocker VM voice**: bytebeat is crowded (Cella,
  Voxglitch, Bokontep). BetaBlocker's visible program counter and
  self-modifying memory would make a mesmerizing panel, but the "code as
  oscillator" niche is occupied.
- **DWG sitar** (sympathetic strings): Audible Instruments Resonator owns
  this in Rack.
- **Chaos attractors, Turing machines, probabilistic sequencers**:
  saturated corners of the library.
- **Generative compositions from sccode** (Eternal Elevator Ensemble, One
  chord requiem, tonal canons): beautiful pieces, but they resist
  knob-per-parameter modularization; instruments port, compositions don't.
