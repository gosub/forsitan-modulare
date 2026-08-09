# forsitan modulare — module ideas

Brainstorm from 2026-07-10/11, sorted best to worst. Novelty was checked
against the VCV Library at the time of writing; re-check before starting
anything.

Entries leave this file when they ship, and the numbering closes up behind
them: it is a rank, not a name. rete, ululo, tabes, lustro and bulla were
all on this list and are all in v2.7.0 — see `CHANGELOG.md` for what
actually got built.

Entries also leave when they are built and *rejected*, which is not the same
thing: those are marked in place rather than deleted, and the verdict is in
[doc/experiments.md](doc/experiments.md). Read it before reviving anything
that carries one of those markers.

## 1. campanae — change-ringing sequencer

Permutation sequencer from English change ringing (Plain Hunt, Plain Bob,
Grandsire, Stedman). Pick bell count and method; rows emit pitch CV + gate
per bell or one poly pair. "Calls" (Bob/Single) as trigger inputs alter the
permutation path.

- 400-year-old algorithmic music tradition, maps 1:1 onto CV/gate.
- Pure combinatorics, cheap to build on existing sequencer plumbing.
- Nothing in the library (Grayscale Permutation is unrelated random seq).

## 2. tela — weaving-draft sequencer

Weaving drafts (threading sequence × treadling sequence through a tie-up
matrix) as a gate sequencer: shafts are output channels, treadling is the
clock dimension. Centuries of published drafts (twills, overshots, huck
lace) become a preset library.

- Panel could render the drawdown (cloth pattern) as it plays.
- Tiny data, rich non-obvious patterns; nothing weaving-shaped in the
  library. Latin bonus: tela is both loom and web.
- Would pair with campanae as a "pre-electronic pattern traditions" release.

## 3. clepsydra — water-clock rhythm generator

Cascade of vessels: each fills at its own CV-controllable rate, tips when
full, fires a trigger, pours into the vessel below. Organic polyrhythms with
physical logic; per-vessel leak for patterns that never lock.

- Adjacent to cumuli (accumulate-and-overflow) but aimed at rhythm.
- Ancient-timekeeping counterpart to solarium. Nothing vessel-based in the
  library; existing "organic clocks" are just jitter-on-a-grid.

## 4. officina — Radiophonic Workshop swoosh box

From Nathaniel Virgo's "Radiophoni" (https://sccode.org/1-S): frequency
shifter inside a feedback loop, seeded by band-passed noise bursts. Endless
rising/falling BBC sci-fi effects.

- Controls: shift amount (bipolar), regeneration, band center, excite in.
- Freq shifters exist in the library; the packaged feedback instrument
  does not. Tiny module, good companion to ululo/rete in a feedback release.

## 5. necto — random cable patcher / patch mutator

Sibling to alea: adds or mutates cables instead of modules. limen already
taught this codebase the engine API for modules, ports, and cables.

- Type awareness is what makes it good, not a toy: classify ports
  (audio/CV/gate) by heuristics, only plausible connections, optional
  attenuation on what it patches.
- WhatTheRack spawns random modules (like alea); nothing in the library
  mutates wiring. alea + necto = generative-patch ecosystem story.

## 6. hydraulis — Roman water organ voice

Drone voice modeled on the hydraulis (oldest keyboard instrument): pipe
ranks on a shared unstable wind supply, valve chuffs, pressure sag when many
notes sound at once.

- The shared-wind coupling (more notes starve them all) is the musically
  interesting part. Sits next to draen; nothing similar in the library.

## 7. tempestas — weather-system modulation source

Coupled simulation of pressure, temperature, wind, cloud cover as slow
correlated CVs, plus event triggers (gust, rain starts, thunder).

- The point is correlation: outputs move like facets of one system, which
  no uncorrelated LFO/S&H bank gives you.
- Library "Random" tag is all uncorrelated sources. Barometer-needle panel
  widget would be very forsitan.

## 8. molecula — Molecular Music Box sequencer

The Molecular Music Box algorithm (grirgz's SC take:
https://sccode.org/1-4Wx): two note durations + a seed like "4E3"; notes
accumulate into loops of different lengths that phase Reich-style.

- Module: two duration knobs, seed, scale, poly CV/gate out.
- Simple rules, rich output, nothing in the library.

## 9. solarium — sundial / real-time modulation

CVs derived from wall clock and date: time of day, day length, sun elevation
for a configurable latitude, season, moon phase. Patches sound different at
dawn than at midnight.

- Very cheap to build; nothing in the library touches real-world time as a
  modulation source (as far as checked).

## 10. brevitas — SC-tweet engine bank

The draen move again: a curated bank of famous 140-character SuperCollider
pieces (SC Tweets collections: https://sccode.org/1-4RA,
https://sccode.org/1-5eN) as selectable engines with fading engine select.

- Architecture already exists (draen). Museum piece of SC culture.
- Ranked down for the licensing burden: many tweets, many authors, sccode
  posts mostly unlicensed. Needs per-author permission or a strict subset.

## 11. murmuratio — starling-flock modulation source

Boids as a polyphonic CV source: each poly channel is one bird (x/y or
heading/speed), plus flock density and centroid outputs.

- Correlated-but-individual motion; poly cousin of tempestas. Pairs with
  pavo (spread the flock across the stereo field).
- Sha#Bang! Photron uses boids but only to animate panel colors; the
  CV-source niche is open.

## 12. tessera — Wang-tile pattern sequencer

Wang tiles (edge-matching turns random placement into coherent structure)
mapped to pitch/gate/CV. Generative sequences that are neither looped nor
random, with audible local logic.

- Nobody in the library uses tiling algorithms; great name (mosaic tile,
  Roman watchword token).
- Most experimental of the batch; needs good sonification choices to not
  feel academic.

## 13. tali — knucklebone dice

Small companion to alea: random source with the historically documented
unequal face probabilities of Roman knucklebones (~4/10 flat sides, ~1/10
narrow). Throw four at once; sum as CV; named-throw triggers (Venus: all
different, Canis: all ones).

- Weekend module. "Weighted random with 2000-year-old weights" is a good
  forsitan joke; alea iacta est demands it.

## 14. epicyclus — Ptolemaic LFO

Deferent plus epicycles: stacked rotating circles, each with rate, radius,
direction; the traced point's x/y are two CV outs. Looping-but-complex
shapes with retrograde swerves; the panel visual explains itself.

- Caveat: under the hood it is additive sine LFOs with phase coupling, so
  the least novel DSP here; concept, geometry, and 2D output carry it.

## Spectral ideas (2026-08-06)

Second brainstorm, from a discussion of clone targets (Reaktor Skrewell,
Blukac Endless Processor, Ewan Bristow's spectral plugins, three Chase Bliss
pedals). Sorted best to worst *within this section*; the S-numbering is
independent of the list above.

Everything from S1 to S6 shares one FFT/phase-vocoder core, so they are not
independent bets: build the core once, then each module is a different
transform between the same analysis and resynthesis stages. Forsitan has no
spectral module today, every one of the 25 is time domain.

**Novelty checked against the VCV Library 2026-08-06.** Per-entry verdicts
below. The S-numbers are stable IDs, not the ranking; after the check the
order is **S1, S3, S4, S5, then S2, S6, S7 demoted**.

The incumbent to know about is **Frequency Domain**, a 9-module plugin
entirely devoted to FFT and resynthesis work: Morphology (spectral morpher),
Delayed Reaction (spectral delay), Freudian Slip (resynthesising sampler),
Harmonic Convergence (resynthesis engine), plus granular and wavetable
modules. The spectral corner is not empty, it has a specialist. Also relevant
across several entries: **Tonecarver Blur** (open source,
github.com/Tonecarver/tcRackModules), **Chaotic Instruments FFTF**, **DanT
Kapow**, and the Clouds ports (Audible Instruments Texture Synthesizer,
Sanguine Mutants Nebulae / Etesia / Fluctus).

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
- **Library check: CLEAR, and the best-evidenced of the batch.** `paulstretch`
  returns 0 modules. `stretch` returns exactly one, KRT "T", an unrelated
  pitched-time-stretch sync delay. Better than an absence: there is
  *demonstrated demand with no native answer*. The community thread "Using
  Paulxstretch in VCV Rack" has people trying to load the PaulXStretch VST
  through Host-fx (and failing to get Host to see it), and the standing advice
  elsewhere is "use Clouds in time-stretcher mode", which is a granular
  approximation, not the phase-randomised FFT technique.
  https://community.vcvrack.com/t/using-paulxstretch-in-vcv-rack/16146

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
- **Library check: OCCUPIED. Demoted from second place, do not build as
  specified.** Chaotic Instruments **FFTF** is "FFT spectral freeze: captures
  the incoming spectrum and sustains it as a frozen pad", which is this
  module's core function almost verbatim. Tonecarver **Blur** has a Freeze
  control on top of its frame buffer. The Clouds ports cover the granular
  freeze angle. And perge already freezes in-house. That is three external
  modules plus self-competition.
  What survives the check is *only* the part FFTF does not do: **five
  stackable layers per channel**, dual channel, where each new capture sustains
  on top of the still-sounding earlier ones. That is the actual Blukac idea and
  it is genuinely absent from the library. But it is a thin differentiator to
  hang a module on. Build it only if the layering *is* the design, not as a
  freeze module that also layers.

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
- **Library check: CLEAR, the cleanest gap found.** `partials` returns only
  docB OscA1, an additive *oscillator* (256 partials), which is synthesis and
  not analysis. `transient` returns transient *shaping* (Ambivalent
  Instruments GroupDelay) and transient *detection* (SignalFunctionSet Phase),
  nothing that decomposes a signal into sinusoids plus residual. No SMS
  anywhere in the library. Nearest neighbour is DanT **Kapow**, an additive
  resynthesis voice, but it works from offline analysis and is an instrument,
  not a real-time splitter.
- Caveat on the clean gap: nobody doing it may mean nobody wants it. Weaker
  evidence than S1's demonstrated demand, which is why S1 still ranks first.

### S4. spectral scale quantiser

Detect every partial, snap each to the nearest note of a chosen root and
scale, resynthesise. Speech, noise and cymbals come out harmonised.

- Best effort-to-payoff ratio here. Cheap once peak picking exists, and
  musically legible in a way most spectral effects are not.
- **imber and sylla already have the root/scale infrastructure** in
  `src/imber/`: free code and a consistent UX across three modules.
- Name candidates: consono, concino.
- **Library check: CLEAR.** `harmonizer` returns only CV-domain and
  voice-domain work: Ahornberg Harmonizer (harmonic/subharmonic CV generator),
  Chinenual Tintinnabulator (Arvo Pärt style, a quantizer), Shortwav Korupt
  (PLL harmonizer + fuzz). Closest is gregsbrain **xVox**, "a 4 voice
  quantizing pitch shifter", but that quantizes whole shifted voices, not
  individual partials. Snapping every partial of one signal to a scale is not
  done.

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
- **Library check: the transform is CLEAR, the neighbourhood is populated.**
  No module warps partials by harmonic number with a fixed fundamental.
  But Frequency Domain **Morphology** (spectral morpher) and **Harmonic
  Convergence** (resynthesis engine) sit right next door, as does **Freudian
  Slip** (resynthesising sampler) and DanT **Kapow**. Novel transform, crowded
  street. Weigh that against S5 already being the weakest musically of S1-S5.

### S6. spectral blur — a reverb different in kind

Give every bin its own decay time and let magnitudes smear forward in time:
frequency-dependent infinite tails.

- Justified only because it is a different animal from what exists: antrum is
  an FDN, caligo is a nested allpass. A third *time-domain* reverb would not
  be worth building; a frequency-domain one is.
- Per-bin delay offsets fall out of the same structure, so "highs arrive late"
  and randomised per-bin smear are modes, not separate modules.
- Name candidates: vapor, diffundo. Not nebula, which now collides twice:
  Qu-bit's hardware and Sanguine Mutants' Clouds port are both called Nebulae.
  Note caligo already means fog in this collection.
- **Library check: OCCUPIED. Drop, or narrow hard.** Tonecarver **Blur** is
  this module, and done well: a history buffer of up to 10s / 10000 FFT
  frames, a position read head interpolating between frames, frame-drop
  probability, freeze, and a "blur" control that picks bins randomly from
  frames either side of the playback position. It is built from Jean-François
  Charles's 2008 Computer Music Journal paper on spectral processing in
  Max/MSP + Jitter, which is the canonical source for this whole technique,
  and it is open source. Frequency Domain **Delayed Reaction** separately
  covers the per-bin delay idea.
  The S6 pitch (per-bin decay times, i.e. a frequency-domain reverb with
  per-bin RT60) is *technically* a different mechanism from a frame buffer,
  but the musical outcome overlaps heavily. Only worth pursuing if a prototype
  demonstrably does not sound like Blur, and the honest prior is that it will.
  Read the Charles paper before deciding anything here.

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
- **Library check: no clone exists, but the neighbourhood is saturated and we
  are already in it twice. Demoted.** `chaos` returns 36 modules. No Skrewell
  clone, and Reaktor users say there is nothing like it outside Reaktor
  (the suggested nearest thing is Metaphysical Function), so the specific gap
  is real. Against that: HetrickCV alone ships five chaos generators,
  and **Venom Vlippoo Box** is a Blippoo Box emulation competing directly with
  our own bulla. Forsitan already occupies this corner with bulla, rete and
  guttur. The binding constraint is self-competition, not the library.
  This was already the July list's finding ("chaos attractors: saturated
  corners of the library"), now reconfirmed.

## sccode sweep (2026-08-09)

Third brainstorm, this one systematic rather than associative: a crawl of every
"added code" post on sccode.org (1052 entries, the site's whole history), read
for things that would become a *module* rather than a piece. Numbered SC1 up,
best first, independent of the lists above.

Five sccode entries have already become forsitan modules (blippoo box → bulla,
guitar feedback → ululo, audrey II → vorax, Walsh-Hadamard → quadrare, scanned
synthesis → scando) and four more are already ranked above (Radiophoni → 4,
Molecular Music Box → 8, SC tweets → 10, Paulstretch → S1). Everything here is
new.

**Novelty checked against the VCV Library 2026-08-09**, by cloning
`github.com/VCVRack/library` (the manifest repo behind the library, current to
2026-08-03) and grepping all 4735 module names, descriptions and tags. That is
a much sharper instrument than the web search used for the earlier lists, so
the verdicts below are firmer, with one known limit: 217 modules ship an empty
description, so absence is strong evidence and not proof.

Licensing posture is the same everywhere in this section: sccode posts carry no
licence unless stated, so these are *techniques read off the code*, not code to
lift. Every one of them is a paragraph of algorithm, which is why they are here.

### SC1. waveset distortion

Wishart's waveset technique: cut the incoming signal at every second
zero crossing, and treat those variable-length fragments as the unit of
playback. Then repeat each waveset N times, omit them at some probability,
resample them to a fixed length, reverse them, shuffle a window of them.
Pitch and rhythm come apart in a way nothing time-domain-conventional does.

- Source: [Simple GUI for WavesetsEvent](https://sccode.org/1-5du), which
  exposes exactly the parameter set a panel wants (start, count, repeats,
  rate, omission probability, legato/overlap) over
  [musikinformatik/WavesetsEvent](https://github.com/musikinformatik/WavesetsEvent).
  The underlying idea is Wishart's *Audible Design*.
- Cheap: a zero-crossing detector, a ring buffer and an index. No FFT, no
  latency, works on live input, which is what makes it a Rack module rather
  than an offline CDP process.
- **Overlap watch**: perge repeats and glitches, tabes and textor loop. The
  differentiator is that the *segmentation is the signal's own*, not a clock's,
  so a repeat count of 3 raises pitch by nothing and stretches time by three.
  That has to be legible on the panel or it reads as another stutter box.
- **Library check: CLEAR.** No module mentions wavesets in this sense; the one
  hit for the word is CV funk Zephyr, whose "waveset families" are wavetable
  banks. Nothing does zero-crossing segmentation.
- Name candidates: sectio, incisum.

### SC2. eternal accelerando and glissando

The Risset rhythm: stack six copies of the same loop at octave-spaced rates,
each one sliding continuously up the rate scale, with a raised-cosine window
over log rate fading a layer in at the bottom as its neighbour leaves at the
top. The result speeds up forever without ever arriving. The pitch version is
the Shepard-Risset glissando, the same construction one dimension over.

- Sources: [Risset rhythm](https://sccode.org/1-511) by snappizz, whose
  SynthDef is fifteen lines and cites
  [Stowell's ICMC 2011 paper](http://c4dm.eecs.qmul.ac.uk/papers/2011/Stowell2011icmc.pdf),
  plus [Shepard-Risset glissando](https://sccode.org/1-5ee).
- The strong version is an *effect*, not a generator: capture live input into
  the buffer and drive the illusion with it. RATE (bipolar, accelerate or
  decelerate), BANDWIDTH (how convincing versus how wide), and a CV over the
  centre so the illusion can be steered.
- **Library check: CLEAR.** `risset` and `accelerando` return nothing.
  `shepard` returns exactly one module, Count Modula Shepard Generator, which
  emits 8 phased ramps as *control signals* for you to build a Shepard tone
  out of yourself. Nobody has done the rhythmic version, and nobody applies
  either to incoming audio.
- Name candidates: vertigo, gyrus.
- A wider framing worth considering before drawing the panel: a bank of
  auditory illusions (Risset rhythm, Shepard glissando, continuity illusion,
  combination tones from [Sound illusions](https://sccode.org/1-5hd)) as
  selectable engines, the draen pattern. `illusion` returns zero modules in the
  whole library. Against it: the engines share no machinery, so it would be a
  bag rather than an instrument.

### SC3. entrainment network

N pulse nodes, each with its own free rate, each listening to its neighbours
and pulling its period toward what it hears. One knob from zero coupling
(N independent metronomes drifting apart forever) to full coupling (all locked
in phase), with the interesting music living in the middle, where clusters
form, break and reform.

- Sources: LFSaw's [network of loosely connected nodes](https://sccode.org/1-4Tw)
  (each node measures the interval between its predecessor's events with a
  timer and re-emits at that rate; the chain settles after ~15 s), and
  [Poème Symphonique for 100 Metronomes](https://sccode.org/1-5ir), which is
  the zero-coupling end of the same dial and supplies the other half of the
  design: nodes that run down and stop, so the texture thins to a last
  surviving pulse.
- Outputs: a gate per node, a sum, and the order parameter (how synchronised
  the flock is, 0 to 1) as a CV. That last one is what makes it a modulation
  source and not just a clock bank.
- **Library check: CLEAR in the rhythm domain, occupied in the audio domain.**
  ZetaCarinae **Firefly** is a Kuramoto phase-coupled system, but it is five
  *wavetable oscillators* summed into a timbre, not gates. Moffenzeef **Swarm**
  is an asynchronous gate generator with no coupling at all. Nothing entrains.
- **Self-competition**: imber already has 8 players on drunk clocks. The
  distinction is that imber's clocks wander independently and these listen to
  each other. If that distinction cannot be heard, this is an imber mode.
- Name candidates: concentus, turba.

### SC4. residuum, Xenakis sieves

Sieve theory: build a set from unions, intersections and complements of
residue classes (every n-th unit offset by m), and read the result as either a
scale or a rhythm. Two or three moduli produce patterns that are periodic but
never obvious, and the same expression works on pitch and time.

- Source: [sieves](https://sccode.org/1-5fS) by eli.rosenkim, with the
  Exarchos and Jones paper linked from it. The core function is eight lines.
- Panel: three or four module/residue pairs, boolean operators between them,
  the resulting pattern drawn as a strip, outputs for gates and for quantised
  pitch. The interval succession is the pattern, so it wants a display.
- **Library check: CLEAR.** `sieve` returns nothing. Xenakis is present in the
  library only as GENDY (Coalescent GENDYN, docB Gendy), which the July list
  already ruled out for that reason; sieves are the other half of his toolkit
  and untouched.
- Name: cribrum is the literal translation but is already spoken for by S3, so
  residuum (the residue classes are the actual mechanism) or crates.

### SC5. L-system sequencer

Axiom plus rewrite rules, iterated, then read through a turtle interpretation:
`F` a note, `+` and `-` transpose, `[` and `]` push and pop a voice. Structure
that is neither looped nor random, with audible self-similarity at several
scales, from about twenty bytes of state.

- Sources: [Generating Graphics and Music From The Dragon
  Curve](https://sccode.org/1-5bp), and blueprint's series of forks
  ([Weed after P. Bourke](https://sccode.org/1-5gV),
  [l-systems Pbindef](https://sccode.org/1-5gY),
  [leaf](https://sccode.org/1-5hy)), which between them show the same engine
  driving different interpretations.
- Sits beside campanae (1) and tela (2) as a third pre-electronic pattern
  tradition, except this one is 1968 rather than 1668.
- The panel can draw the turtle path, which is the whole appeal of L-systems
  and would be a display no other sequencer has.
- **Library check: CLEAR.** `l-system` and `lindenmayer` both return zero.
  Note that cellular automata, the neighbouring idea, are the opposite:
  eleven modules, listed under dropped below.
- Name candidates: arbor, ramus.

### SC6. machina, procedural machines

> **BUILT AND REJECTED, 2026-08-09.** Ported from `SDTMotor` as planned,
> finished to panel and docs, and dropped on the sound. Tag `exp/sdt-machina`,
> notes in [doc/experiments.md](doc/experiments.md). Do not re-propose without
> reading those first: the honest caveat below turned out to be the whole
> story.

Andy Farnell's *Designing Sound* motor: a speed envelope drives a saw, the
rotor is bandpassed noise gated by the drive raised to a power, the stator is
a folded cosine of the same phase, and the lot goes through a resonant tube.
RPM as CV, load as CV, and the thing lugs, catches and stalls.

- **Build this from SDT, not from Farnell.** Superseded a day after it was
  written, see M0 below: the Sound Design Toolkit is GPLv3 C and ships
  `SDTMotor`, a combustion engine parameterised by RPM, throttle load, cycle
  type, cylinder count and size, compression ratio, ignition pulse width, cycle
  irregularity, backfire amount, and the lengths of the intake pipes, the
  extractors, the main exhaust, its expansion, the muffler chambers and the
  outlet. That is a whole engine where Farnell is one motor, and it is a port
  rather than a reimplementation. `SDTDCMotor` covers the small-motor case.
- Original sources, still worth reading for how little it takes: DSastre's
  ports of the *Designing Sound* Pd patches,
  [Motors](https://sccode.org/1-4RG), [Cars](https://sccode.org/1-4RH)
  (four-cylinder engine with slugging speed),
  [Electricity](https://sccode.org/1-4RF), [Insects](https://sccode.org/1-4QB).
- **Library check: CLEAR.** Nothing in the library makes engine, motor or
  machine sounds. The whole procedural-audio-for-sound-design corner of Rack is
  empty.
- Honest caveat: it is a sound-design voice, not a musical one, and forsitan
  has no precedent for that. It earns its place by being a rhythm source that
  is not a clock: an engine at 12 Hz is a pulse train with physics.
- Name: machina. Now downstream of M0, so rank it with the material sweep
  rather than against the other SC entries.

### SC7. self-similar sequences

Sloth canons: from a short seed, generate a sequence obeying `r(n) = r(a*n)`,
so the sequence contains a slowed copy of itself, which contains a slowed copy
of itself. Play the fast version against the slow one and the canon is exact at
every depth. Same family as Nørgård's infinity series.

- Sources: [sloth canons](https://sccode.org/1-5fU) and
  [monzos + sloth patch](https://sccode.org/1-5fV) by eli.rosenkim, plus
  [Psloth](https://sccode.org/1-5g3), a pattern class for it by tom.dugovic.
- Module: seed, ratio `a`, and three or four outputs reading the same sequence
  at 1x, 1/a, 1/a², so the canon is patchable rather than described.
- **Library check: CLEAR, with one unknown.** `infinity series`, `norgard` and
  `self-similar` all return nothing. LydD **Poppy-Fields** calls itself an
  "Infinite Sequencer Fractal Generator" but its plugin ships no documentation
  at all, so what it does is unverifiable; check it in Rack before starting.
- Small module, weekend scale, pairs naturally with SC4 and SC5 on one panel if
  none of the three justifies its own.

### SC8. relabi, the meandering beat

A metronome whose beats keep their average positions but whose individual beat
locations random-walk inside the bar, each beat's displacement compensated by
its neighbour so the bar length never drifts. Rhythm that is neither quantised
nor free.

- Sources: [mutronome](https://sccode.org/1-5fQ) by eli.rosenkim (tagged
  relabi, after Peter Blasser's term), and
  [Cumulative Pulses](https://sccode.org/1-53Y), which reaches similar
  territory by summing pulse trains.
- **Library check: CLEAR** for this mechanism. Humanize and swing modules
  exist everywhere, but they jitter each hit independently; the compensation
  (push beat 2 late, beat 3 comes early) is what makes it musical rather than
  sloppy.
- Ranked here because it is one knob's worth of idea. Most likely home is a
  mode inside SC3 rather than a panel of its own.

### SC9. UPIC arcs

Xenakis' UPIC: draw arcs on a page, each arc is a voice, x is time and y is
pitch, and the page is performed by sweeping a playhead across it. Draw
several and you have written a polyphonic gesture by hand.

- Source: [UPIC waveform editor](https://sccode.org/1-4VJ) by snappizz, a
  freehand editor toy in a GUI.
- **Library check: novel in framing, crowded in mechanism.** `upic` and
  `graphic score` return nothing, but mouse-drawable modules are a category:
  PdArray **Array**, mscHack **Wave morph Oscillator**, Bacon **ChipYourWave**,
  Eternal Eclipse **Saros** (drawable envelope screen with playhead), Patina
  **Memory Pad**. All of them draw one curve. The UPIC idea is *several arcs on
  one page swept together*, which none of them do.
- forsitan can build the display (imber's widget is the precedent), which is
  the only hard part.

### SC10. sonic focal depth

Ten layers running continuously, only one of them in focus at a time: the rest
are dimmed, blurred and pushed back, and one control moves the focal plane
through the stack. Depth of field for sound.

- Source: [sonic focal depth](https://sccode.org/1-4Q5) by Dan Stowell.
- Polyphonic input, a FOCUS CV, and a DEPTH knob setting how sharply focus
  falls off. Blur being lowpass plus reverb plus level is a design decision, not
  a given; that choice is the module.
- **Library check: CLEAR**, and no near neighbour found, but the concept is
  unproven: it may just be a crossfader with extra steps. Prototype in a patch
  before committing to a panel.

### SC11. Barry's Satan Maximizer

Steve Harris' LADSPA plugin: a compressor with an absurdly short window that
destroys transients and drags the noise floor up to the peaks. Two controls,
decay time and threshold, and everything sounds like it was recorded through a
wall.

- Source: [Barry's Satan Maximizer](https://sccode.org/1-51w), snappizz's
  replication.
- **Library check: CLEAR.** The only `maximizer` hit is a section of Unfiltered
  Audio Battalion's output stage. Nothing does this deliberately-broken
  dynamics processing.
- Weekend module, and the original is GPL, which for once means code could be
  read rather than only described. Adjacent to raucus, so it would want to look
  like a sibling.

### SC12. dissonator

Ring modulation aimed at psychoacoustics rather than timbre: place the
sidebands where they land inside the critical band of the input's partials, so
the output is maximally rough rather than maximally metallic. Roughness as a
tracked, controllable quantity.

- Source: [Dissonator](https://sccode.org/1-4Zv), julian.rohrhuber's remix of
  a dissonant ring modulator.
- **Library check: CLEAR.** `roughness`, `critical band` and `beating` all
  return nothing; ring modulators are everywhere but none of them track the
  input to place their sidebands.
- Weakest of the batch, and the least certain to be musical, but it is the only
  idea here that would need a real psychoacoustic model.

### Dropped in this sweep

Checked against the library and abandoned. Recorded so the same entries do not
get re-proposed.

- **The Muse** ([1-4Rg](https://sccode.org/1-4Rg), an emulator of Triadex's
  1972 sequencer): occupied twice over, by SignalFunctionSet **Muse**
  ("faithful Triadex Muse port", 40 sources, 31-bit XNOR register) and docB
  **TME**. This was the most attractive entry in the crawl until the check.
- **PadSynth** ([1-58B](https://sccode.org/1-58B), Nasca's algorithm):
  docB **Pad** and **Pad2** are both PadSynth. Note this is the same author as
  Paulstretch, so S1 has a neighbour in the library after all, just not for
  the stretching half.
- **SCGAZER** ([1-5db](https://sccode.org/1-5db), a recreation of the
  Møffenzeef Stargazer): Moffenzeef ships **Stargazer** in the library
  themselves, along with 25 other modules. Cloning a brand that is already
  present in the library is the worst version of a clone.
- **Game of Life and cellular automata** ([1-4YW](https://sccode.org/1-4YW),
  [1-4Xf](https://sccode.org/1-4Xf), [1-4WC](https://sccode.org/1-4WC)): eleven
  modules, including docB **C42** / **CCA** / **CCA2** / **Ant**, 23volts
  **Cells**, Ouroboros **Automata**, Voxglitch **Glitch Sequencer**,
  AlgoritmArte **CyclicCA**, Sparkette **Microcosm**, iggy.labs
  **more-ideas**, Ondas **Bittorio**, Sonus **Cellular Noise**.
- **Wine glass / glass armonica** ([1-51e](https://sccode.org/1-51e)):
  CV funk **Glass** is "a glass armonica physical model".
- **Bouncing ball triggers** ([1-51X](https://sccode.org/1-51X)): Voxglitch
  **Hazumi**, JW **Bouncy Balls**, Bidoo **ChUTE**. The one untouched corner is
  snappizz's [bouncy-ball delay](https://sccode.org/1-56v), a *delay* whose
  echoes accelerate as the ball settles, which is a different module from a
  bouncing-ball sequencer. Too thin to build alone; a good mode for a delay.
- **Vinyl crackle** ([1-4Sj](https://sccode.org/1-4Sj),
  [1-1H](https://sccode.org/1-1H)): HetrickCV **Crackle** is itself a
  SuperCollider port, and stoermelder **DIRT** covers the defect-modelling
  angle.
- **Doppler** ([1-5ef](https://sccode.org/1-5ef)): nozoïd
  **Nozori_84_DOPPLER** and NYSTHI **DOPPLAB**.
- **Integer-sequence sequencers** ([Collatz](https://sccode.org/1-4Vx),
  [Pisano](https://sccode.org/1-4Wb), [Kaprekar](https://sccode.org/1-4Wv)):
  CV funk **Collatz** and two Fibonacci clock dividers already occupy the
  "number theory as rhythm" idea, and the remaining sequences are
  interchangeable with those.
- **Morse code** ([1-4RQ](https://sccode.org/1-4RQ)): four modules
  (Tonecarver, mscHack, Moffenzeef, Daniel Davies).
- **Markov chains** ([1-5ca](https://sccode.org/1-5ca),
  [1-4Su](https://sccode.org/1-4Su)): ZetaCarinae **Rosenchance** and
  **GuildensTurn**.
- **DX7 emulation** ([1-57R](https://sccode.org/1-57R)): SignalFunctionSet
  **Operator** loads real `.syx` cartridges on the msfa engine.
- **Wiard noise ring** ([1-4SD](https://sccode.org/1-4SD)): shift-register
  randomness, which is bulla's runglers plus the saturated Turing corner.
- **Convolution reverb / reverse reverb** ([1-5hf](https://sccode.org/1-5hf),
  [1-5he](https://sccode.org/1-5he)): NYSTHI **CONVOLVZILLA**.
- **Boids** ([1-4RY](https://sccode.org/1-4RY)): already ranked as 11 above,
  and the sccode entry is a port of a Cycling '74 example used with permission,
  so it is not a source to work from.
- **THX Deep Note** ([1-5c6](https://sccode.org/1-5c6)): a beautiful
  reconstruction of a single gesture. One-trick, and the trick is 30 seconds
  long.
- **Compositions** (Ligeti aside, which contributed to SC3): the finding from
  the first list holds. Instruments port, compositions do not.

## material sweep (2026-08-09)

> **M0 to M3 WERE BUILT AND REJECTED, 2026-08-09**, together with SC6, in one
> pass on branch `sdt-machina`. The port worked, the modules were finished to
> panels, docs and tests, and all four were dropped on the sound. Tag
> `exp/sdt-machina`, notes in [doc/experiments.md](doc/experiments.md).
>
> The analysis below is left standing because it is still correct: the library
> gap is real, the interaction-not-objects premise held, and the port is
> reusable. What it did not predict is that the results are not worth
> listening to. M4 and the unranked stock below are untouched, but they sit on
> the same port and the same premise, so read the verdict before starting one.

Fourth brainstorm, from the question: what would everyday-object sounds be if
they were not modal synthesis? Paper crunching and tearing, mechanical
switches, friction, things breaking. Numbered M0 up; the numbering is
independent of the lists above.

The premise survived checking. **Rack's entire "Physical modeling" tag is
resonators**: Audible Instruments **Modal Synthesizer** (Elements), Prism
**Rainbow** (SMR), chowdsp **ChowModal**, Chaotic Instruments **Modal Filter**,
**PinkTrombone**, Sckitam **WaveguideDelay**, Vult **Rescomb**, Sapphire
**Elastika** (balls and springs), Free Surface **WaterTable**, Coalescent
**Haptik**. Every one of them is an object waiting to be struck, and the
striking is left to the patch. Grepping all 4735 modules (index as in the
sccode sweep, checked 2026-08-09) for friction, stick-slip, scrape, squeak,
creak, crumple, tear, rustle, fracture, shatter, contact and foley returns
**zero hits**. The only everyday-object module in the library is Ambivalent
Instruments **Rain**.

So the gap is not "paper is missing". The gap is that nobody in Rack models
**interaction**, only objects. Modal synthesis answers "what is ringing";
these sounds are almost entirely "how is energy arriving", and for crumpling
the resonator barely matters at all.

That is also what makes this one family rather than six unrelated toys. All of
it is a stochastic point process of micro-events, and the axis that separates
the materials is whether the events are independent or self-exciting:

- **crumpling**: independent events, power-law energy distribution, scale-free
  over decades (Houle and Sethna's acoustic-emission measurements on crumpling
  paper are the canonical reference; cited from memory, verify before relying
  on the exponent)
- **tearing**: a crack front propagating along a line, so events cluster and
  each one raises the odds of the next
- **breaking**: full avalanche, one event triggering a cascade of fragments
- **friction and scraping**: the same process with a restoring drive, walking
  from discrete creaks at low velocity to periodic squeal at high

One knob from independent to self-exciting walks paper rustle to tear to
shatter. One knob of drive velocity walks creak to squeal. Both are physical
parameters (criticality, drive), not fudges, which is the sort of spine
forsitan panels are good at exposing.

**The design rule for everything below**: not a foley box with named buttons
(PAPER, GLASS, SWITCH), which is a module you demo once. These are
**CV-played excitation sources**: force and velocity in, material and
criticality as the timbre axes, audio out plus a per-event trigger out. Then
paper rustle is a texture that answers a gesture, and the trigger out makes it
a rhythm source as well as a voice. That framing has to be in the design from
the start, not bolted on.

### M0. the SDT port (prerequisite, not a module)

> **BUILT, 2026-08-09.** The port exists and works, at tag `exp/sdt-machina`
> as `src/sdt/`. Pull it back rather than redoing it; four settings in it were
> forced by measurement and are listed in [doc/experiments.md](doc/experiments.md).


The [Sound Design Toolkit](https://soundobject.org/SDT/) (Delle Monache and
Rocchesso, out of the EU SOb / CLOSED / SkAT-VG projects,
[SoftwareX 2017](https://www.sciencedirect.com/science/article/pii/S2352711017300195),
source at [SkAT-VG/SDT](https://github.com/SkAT-VG/SDT)) is a hierarchical,
perceptually founded taxonomy of everyday sound events, implemented as a
cross-platform C core with Max and Pd externals on top.

- **It is GPL version 3 or later, in plain C, last pushed 2024.** That is
  directly compatible with forsitan and changes the whole posture of this
  section: a port (caligo, vorax) rather than a reimplementation from papers
  (vespae, bulla, antrum).
- The model list is almost exactly the brief: `SDTCrumpling`, `SDTBreaking`,
  `SDTScraping`, `SDTRolling`, `SDTBouncing`, `SDTFriction`, `SDTImpact`,
  plus `SDTExplosion`, `SDTBubble`, `SDTFluidFlow`, `SDTWindFlow`,
  `SDTWindCavity`, `SDTWindKarman`, `SDTMotor`, `SDTDCMotor` and the
  `SDTResonator` they attach to.
- The architecture is resonators times interactors: an interactor computes a
  contact force between two resonators, and impact and friction are the two
  interactor types. That maps onto Rack cleanly, and it is the same shape as
  `src/imber/`, a header-only DSP library shared by several modules.
- Before leaning on it: read `3rdparty/`, check what the models need at
  control rate versus audio rate, and settle attribution the way vorax did.
- Do not port the taxonomy. Port two or three models and build modules around
  them. The Max package is a research instrument with a hundred parameters;
  the forsitan version of any of these is six knobs and a decision about
  which ninety-four to fix.

### M1. stridor, friction and scraping

> **BUILT AND REJECTED, 2026-08-09.** 10HP, finished. Tag `exp/sdt-machina`,
> see [doc/experiments.md](doc/experiments.md).


Dry friction as a voice: a stick-slip relaxation oscillator driven by normal
force and sliding velocity, with surface roughness as a noise profile feeding
the contact. At low velocity it emits discrete creaks, as velocity rises the
slips lock into a periodic squeal, and the transition between the two is
continuous and playable.

- **The strongest single module of the batch.** It is continuously excitable,
  so it behaves like an instrument rather than an effect, and it self-oscillates
  at the squeal end, which is the forsitan house style (vespae's
  self-oscillation was kept deliberately).
- `SDTFriction` and `SDTScraping` are both in the toolkit; scraping is friction
  with a surface profile scrolling under the contact, so one engine covers
  rubbing, scraping, bowing-adjacent squeal and the creaking door.
- Controls: FORCE, VELOCITY (both CV), roughness, stiffness and dissipation of
  the contact, and the resonator it drives.
- **Library check: CLEAR.** Zero hits for friction, stick-slip, scrape, squeak
  or creak. The nearest thing is the bow exciter buried inside Elements, which
  is a preset inside a resonator module and not reachable as a source.
- Name candidates: stridor (creaking, screeching), attritus (rubbing, wearing
  away).

### M2. crepitus, crumpling and tearing and breaking

> **BUILT AND REJECTED, 2026-08-09.** 10HP, finished, with the Hawkes
> branching ratio as the crit knob. Tag `exp/sdt-machina`, see
> [doc/experiments.md](doc/experiments.md).


One point-process engine with a criticality knob. At the bottom, independent
buckling events with power-law energies: paper being squeezed. Raise it and
events start triggering their neighbours, so the texture organises into a
crack front travelling along a line: tearing. At the top, one event sets off
the whole cascade: something breaks.

- `SDTCrumpling` and `SDTBreaking` in the toolkit are two ends of this and are
  worth diffing before deciding whether they are one module or two.
- The distinctive sound of the batch, and the one furthest from anything Rack
  can currently make.
- Controls: DRIVE (how hard the material is being worked, CV), criticality,
  material (fragment size distribution and the resonance each event excites),
  plus the event trigger out, which turns paper into a rhythm generator.
- **Library check: CLEAR.** Zero hits for crumple, tear, rustle, fracture or
  shatter. HetrickCV **Crackle** is the SuperCollider `Crackle` UGen, a chaotic
  map, unrelated to acoustic emission.
- **Self-competition with imber**, which already generates procedural rain, and
  rain is also a point process. If crumpling turns out to sound like rain with
  a different filter, it is an imber engine and not a module. Test that by ear
  early, before any panel work.
- Name: crepitus (crackling, rustling, clattering).

### M3. ruina, the object under load

> **BUILT AND REJECTED, 2026-08-09.** 12HP, finished. Built alongside M2
> rather than instead of it, so the choice could be made by ear; the answer was
> neither. Tag `exp/sdt-machina`, see [doc/experiments.md](doc/experiments.md).


Same engine as M2 with a different front end, and the more forsitan of the two
framings. A CV loads the object; the module accumulates strain, creaks and
crackles as it goes, and decides for itself when the material fails. On failure
it emits the break and a trigger, then resets.

- This turns a smash button into a modulation source with hysteresis: the
  output is a texture, but the interesting signal is the trigger, whose timing
  the patch cannot fully predict and cannot force.
- Failure statistics are the design: a Weibull-ish distribution means the
  object usually survives a given load and occasionally does not, so repeated
  identical gestures give different lifetimes.
- **Build M2 or M3, not both.** They are one engine and two panels. M3 is the
  better module and the harder sell; M2 is the more obvious one and the easier
  demo.
- Name: ruina (collapse).

### M4. contact and bounce

Impacts, bouncing and rolling: `SDTImpact`, `SDTBouncing`, `SDTRolling`. A
mechanical switch is exactly this, an impact plus contact bounce with low
restitution, and so is a dropped object settling.

- Ranked last deliberately. As a module of its own it is a click generator, and
  clicks are cheap in Rack. Bouncing-ball triggers are also taken three times
  over (Voxglitch **Hazumi**, JW **Bouncy Balls**, Bidoo **ChUTE**).
- Worth having as the excitation *source* inside M1 and M2 rather than as a
  panel: what strikes the object should be as physical as the object.

### Further SDT stock, unranked

Available from the same port, none of it checked as carefully as the above:
`SDTExplosion`, three wind models (`SDTWindFlow`, `SDTWindCavity`,
`SDTWindKarman`, the last being vortex-shedding tones, the whistle of wind past
an edge), `SDTBubble` and `SDTFluidFlow`. The library is nearly empty here too:
the only liquid hit is TyrannosaurusRu **Droplets**, a sequencer inspired by a
leaky faucet rather than a synthesis model.

The wind models are the ones to keep in mind, because tempestas (7) is a
weather *modulation* source with no sound of its own, and these would give it
one.

SC6 (machina) also moves here: `SDTMotor` is a better engine than the Farnell
patch it was based on, and it arrives with the same port.

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
