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

## Spectral ideas (2026-08-06)

Second brainstorm, from a discussion of clone targets (Reaktor Skrewell,
Blukac Endless Processor, Ewan Bristow's spectral plugins, three Chase Bliss
pedals). Sorted best to worst *within this section*; the S-numbering is
independent of the list above.

**Novelty was NOT checked against the VCV Library for any of these.** Do that
before starting, the way the July list was checked.

Everything from S1 to S6 shares one FFT/phase-vocoder core, so they are not
independent bets: build the core once, then each module is a different
transform between the same analysis and resynthesis stages. Forsitan has no
spectral module today, every one of the 25 is time domain.

### S0. the spectral core (prerequisite, not a module)

Header-only in `src/spectral/`, the way `src/imber/` is shared by imber and
sylla. Analysis, transform, resynthesis, with only the middle stage varying.

- `dsp::RealFFT` is in the Rack SDK (pffft underneath), so no new dependency.
- Rack calls `process()` per sample, so the core owns its own hop buffering.
  2048 window / 512 hop at 48k is ~94 transforms a second, CPU is not the
  problem.
- Needs: windowing + overlap-add, spectral peak picking, f0 detection
  (harmonic product spectrum is nearly free once magnitudes exist).
- **Latency stacks.** 2048 window is ~43ms; four such modules in series is
  ~170ms. Expose the core's latency, keep one window-size convention across
  all spectral modules so the total is predictable, and offer a window-size
  menu to trade latency for resolution. Precedent: caligo's diffuser carries
  ~70ms inherent and that was accepted.
- Open architectural question, settle before the API hardens: several small
  single-purpose modules (the forsitan norm for utilities, and what makes the
  S3 splitter worth having) versus one large instrument with a selectable
  transform (the draen / interea engine-bank pattern this codebase already
  knows). Leaning small.

### S1. paulstretch drone

Overlapping windows, discard and randomise the phase, resynthesise advancing
the hop far slower than it was consumed. Anything becomes an enormous smeared
drone at 10x to 100x.

- Highest payoff of the batch. The collection is already drone-heavy (draen,
  vorax, guttur, textor) and this is the one drone technique none of them can
  reach.
- Algorithm is a paragraph of prose from Nasca and genuinely simple.
- **Licensing**: original Paulstretch is GPLv2, forsitan is GPLv3, so
  reimplement from the description and do not lift code. Easy here.
- Design question to settle first: at stretch rate zero this *is* the S2
  freeze. Freeze holds one moment forever, paulstretch plays the whole thing
  50x slower, but it is the same machinery with the hop ratio as the only
  difference. S1 and S2 may be one module with a rate knob whose bottom end
  is infinity. Decide before drawing two panels.
- Name candidates: aevum (eternity), protraho, traho. Avoid tendo, too close
  to tundo.

### S2. blukac endless processor clone

Dual-channel infinite sustainer: capture the buffer, resynthesise it into a
clickless endless stream, stack five layers per channel. Memory 100ms to 3s.

- No code, but the [manual](https://blukac.com/files/Endless_Processor_Manual.pdf)
  and the [SOS review](https://www.soundonsound.com/reviews/blukac-endless-processor)
  pin down the entire feature set, and the method is stated as spectral
  resynthesis rather than looping. You are matching described behaviour, not
  a voicing.
- Tightest scope of any clone target considered.
- **Overlap watch**: perge already freezes, tabes and textor already loop,
  vestigia is already memory. The differentiator is that this is a *spectral*
  freeze with layer stacking. Be deliberate about that on the panel or it
  reads as perge's freeze in 8HP.
- See S1: possibly the same module.

### S3. cribrum — tonal / noise separation

Sinusoidal modelling (Serra and Smith, SMS): track stable spectral peaks,
resynthesise them as the tonal part, subtract, the residual is the noise.
Two outputs.

- The infrastructure play. Modest alone, but it **multiplies every other
  spectral module**: stretch the tonal part and leave the transients, freeze
  the noise floor and let the pitch play through.
- Nothing in the Rack library appears to do this. Tool-nobody-has, which is
  what forsitan does well.
- This is the strongest argument for the small-modules fork in S0.

### S4. spectral scale quantiser

Detect every partial, snap each to the nearest note of a chosen root and
scale, resynthesise. Speech, noise and cymbals come out harmonised.

- Best effort-to-payoff ratio here. Cheap once peak picking exists, and
  musically legible in a way most spectral effects are not.
- **imber and sylla already have the root/scale infrastructure** in
  `src/imber/`: free code and a consistent UX across three modules.
- Name candidates: consono, concino.

### S5. frequency stretching (Ewan Bristow inspired)

Stretched tuning as a spectral transform: detect f0, express each partial as
harmonic number `n = f / f0`, warp through a function that fixes `n = 1`
(`f_n = n * f0 * sqrt(1 + B * n²)`, piano inharmonicity, or `f_n = f0 * n^s`),
resynthesise. Pitch is preserved, timbre goes inharmonic and bell-like. That
fixed fundamental is the whole "fundamental-aware" part.

- Originally ranked first as a *port*, demoted when it turned out the .pd
  patch is not extractable from the plugdata build. Reimplementation from the
  described technique, same footing as vespae (DAFx paper, no code) and bulla.
- Still the simplest consumer of the core (pure transform, no state machine),
  which makes it a good shakedown module for S0 even though it ranks below
  S1-S4 musically.
- The pitch tracker is what will need tuning time, not the warp.
- Degrades gracefully on unpitched input: as f0 goes unstable, blend toward
  plain frequency warping.
- Worth trying first, ~10 minutes: ask Bristow directly (individual dev, gives
  plugins away, works entirely in an open-source ecosystem), and `strings` the
  binary for `#X obj` / `#N canvas` in case the plugdata wrapper embedded the
  patch as a text resource rather than compiling through hvcc.

### S6. spectral blur — a reverb different in kind

Give every bin its own decay time and let magnitudes smear forward in time:
frequency-dependent infinite tails.

- Justified only because it is a different animal from what exists: antrum is
  an FDN, caligo is a nested allpass. A third *time-domain* reverb would not
  be worth building; a frequency-domain one is.
- Per-bin delay offsets fall out of the same structure, so "highs arrive late"
  and randomised per-bin smear are modes, not separate modules.
- Name candidates: vapor, diffundo. Not nebula (Qu-bit Nebulae collision), and
  note caligo already means fog in this collection.

### S7. skrewell-inspired chaos (no spectral core needed)

Pulse oscillator bank, cross FM/AM, several resonant 2-pole filters, feedback,
and one FLOW macro sweeping all of it at once. Squarely the rete / bulla /
guttur family.

- **Take the idea, do not attempt a clone.** Someone already tried porting it
  to Max/gen and hit a wall: Reaktor's built-in filters are closed and appear
  to saturate in an undocumented way, and the chaos lives *inside* that
  nonlinearity inside a feedback loop, so small filter differences give
  completely different output. Their words: "butterflies, hurricanes, and
  stuff". Ship it as inspired-by, the bulla precedent.
  https://cycling74.com/forums/problem-rebuilding-the-reaktors-skrewell-in-maxgen
- Shares nothing with S0-S6, so it can run in parallel as a break from FFT
  work.
- Not a self-contained sound generator in the Reaktor sense: it runs
  continuously with no gate/pitch input.

## Considered and dropped

- **Chase Bliss Mood mk2 / Lost + Found / Bad Mood**: worst case on both axes
  at once. Zero source (closed commercial DSP, no papers, no firmware, no
  teardowns reaching the algorithms) *and* enormous scope: Lost + Found is 12
  modes across two channels plus a compressor, 144 combinations; Bad Mood is 3
  delay modes plus 3 reverb modes (radio alone has 5 sub-stations) plus a
  global Glue stage plus cross-channel interaction. That is five modules, not
  one, and what you would land is "a stereo multi-effect with a lot of modes",
  which is what perge already is. Bad Mood does not even ship until late
  October 2026, so there is nothing in the wild to A/B against. Also a
  different posture from the other ports: dronecaster and Audrey II were MIT
  firmware, Greyhole is Faust, antrum came from published papers, whereas
  these are current products actively being sold.
  The one salvageable part is *structure*, not algorithms: two channels aware
  of each other, either side able to record, modulate or be routed through the
  other, with the main knob a crossfade between two engines rather than a
  wet/dry. That topology is portable and needs no cloning.
- **An FFT version of quadrare**: quadrare already is "patch into the
  transform domain, with keep/quant lossy stages, residual out and
  per-coefficient outs". Redoing that in the Fourier basis is a near-duplicate
  of the concept whatever the basis says. Same reason to skip spectral
  codec-degradation / perceptual-masking ideas: quadrare owns lossy-transform
  territory in this plugin.
- **Phase vocoder pitch shifter**: done to death, several already in the
  library.
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
