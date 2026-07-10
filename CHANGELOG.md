# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/)
and this project adheres to [Semantic Versioning](http://semver.org/).

## [2.6.16] - 2026-07-10
### Added
  - test: `draen_sweep`, an offline octave sweep (27.5 Hz → 3.52 kHz) of both
    dræn engine banks reporting per-channel DC, AC RMS, peak and NaN counts
  - MMCCCXCIX: delta-sigma oversampling selectable from the context menu
    (1×/2×/4×/8×/16×, saved with the patch); the new default 8× sounds
    identical to the previous hardcoded 16× at half the CPU
### Changed
  - MMCCCXCIX: the delta-sigma loop and parameter handling were optimized
    (dead demod pole removed, cheaper TPDF dither, single-pass RAM bit
    exchange, coefficient math only runs while a knob/CV moves); combined
    with the 8× default the module's CPU use drops to roughly a third
### Fixed
  - dræn: a MinBLEP discontinuity landing within float rounding of a sample
    boundary read past the impulse table and permanently NaN'd the voice
    (the thx engine reliably died above ~1.7 kHz)
  - dræn: DC offsets in twelve dronecaster-bank engines (sunno reached -0.85
    at 27.5 Hz) removed with output DC blockers, leaving the SynthDef-faithful
    interiors untouched
  - dræn: the band-limited saw leaked DC proportional to its frequency (a
    minBLEP residual artifact), audible as offset on saw-heavy engines in
    both banks (wall, anthem, supersaw)
  - dræn: DC offsets across the hyf bank — feedback loops (mirror, wire,
    quill, rain, pulsework, hive), tracking lowpasses over noise (turbine,
    ember), waveshaping and near-zero PM sidebands (root, sputter, aster)
    now go through DC blockers; existing blockers relaxed to a 7.6 Hz cutoff
    so 27.5 Hz fundamentals keep their level
  - dræn: hyf engine levels no longer swing with pitch — engines whose
    loudness genuinely depends on hz (choir, breath, bowl, gong, tide, rain,
    mirror, pulsework, frost) get an octave-table makeup gain interpolated
    in log2(hz); quill's gain doubled (its old level was mostly the drift)

## [2.6.15] - 2026-07-09
### Added
  - limen: `get_module_info` protocol command — the metadata Rack shows in a
    module's right-click Info menu: model description, tags and links, plus
    the owning plugin's brand, version, license, author and URLs
### Changed
  - tools: the limen CLI clients (former `tools/cli/`) moved to their own
    repository, [gosub/limen-tools](https://github.com/gosub/limen-tools),
    renamed `limen` → `limen-cli`, with a new `info` command, Windows
    support, and CI-built binaries for Linux/Windows/macOS on its releases
  - tools: loose scripts organized into `typography/`, `panels/` and
    `patches/` subdirectories

## [2.6.14] - 2026-07-09
### Added
  - cumuli: super-slow mode in the right-click menu — both rates 100x slower
    (0.0001 V/s to 1 V/s, center default 0.01 V/s), knob tooltips rescale,
    saved with the patch
### Fixed
  - dræn: loading a patch with a saved engine now fades that engine in from
    silence, instead of playing engine 0 first and fading out of it

## [2.6.13] - 2026-07-09
### Added
  - dræn: a second engine bank — **hyf** (Old English for *hive*), 37 original
    drone instruments built on the same UGEN layer, selectable from the
    right-click "Engine bank" menu and saved with the patch; switching banks
    fades like an engine change
  - the hyf roster deliberately covers ground the dronecaster set doesn't:
    binaural beating (beam), Shepard tones (shepard), CZ phase distortion
    (phase), wavefolding (fold, corona), formant/vowel drones (choir, breath,
    eclipse), octave-up shimmer feedback (halo), bowed and plucked strings
    (wire, quill, rain), singing bowls and gongs (bowl, gong), and
    environmental textures (tide, ember, veldt, frost, turbine)
  - all 37 verified: level-matched to the first bank, no NaNs, feedback
    engines stable over 60 s at 40/440 Hz; the whole bank is light on CPU
    (every engine ≤ 1.4% of real time at 48 kHz)

## [2.6.12] - 2026-07-09
### Changed
  - dræn: DSP optimization pass, ~26% less CPU overall and the heaviest engine
    (hecker) cut from 8.8% to 2.7% of real time — filter and lag coefficients
    (biquad, Ringz, MoogFF, SVF, BAllPass, Lag/LagUD, Amplitude) now refresh on
    16-sample blocks instead of every sample (SC itself uses 64-sample control
    blocks, so this is finer-grained than the original), Env.perc advances its
    exponential shape incrementally, and hecker's pow/trig control mappings are
    evaluated at the same block rate
  - verified against the pre-optimization build: per-engine RMS unchanged
    within ±0.2%, all feedback engines stable over 60 s at 40/440 Hz

## [2.6.11] - 2026-07-09
### Added
  - dræn: the last four dronecaster engines, all @zebra — **unmemqua** (28-partial
    Klank torn by breathing filters), **uneablin** (cross-delayed PM sine ring
    with waveshape morphing), **unwealne** (wandering pulses octave-shifted
    through a 13-ratio bank), and **unreanth** (buffer-sequenced sine fades over
    ring-mod saws and a long pitch-smeared feedback delay) — **completing the
    full 37-engine dronecaster roster**
  - draen_ugens.hpp gains PitchShift (two-tap granular shifter), LagUD, and the
    distort / InsideOut / DiodeRingMod waveshapers; Ringz gains an SC-exact
    un-normalized mode (long partials ring louder, as in SC's Klank)

## [2.6.10] - 2026-07-09
### Added
  - dræn: two more dronecaster engines — **takita** (@sixolet, a self-clocked
    flip-flop drum language of resonant filtered clicks) and **twin pks**
    (oscillator-free tape-noise horror: compressed, band-passed, wow/fluttered,
    saturated and bit-crushed)
  - draen_ugens.hpp gains Phasor (resettable ramp) and Decimator (sample-rate /
    bit-depth reducer)

## [2.6.9] - 2026-07-09
### Added
  - dræn: three more dronecaster engines — **sunno** (Karplus-Strong doom
    guitars through crossover distortion and cascaded tanh stages), **nautilus**
    (@taubaland, Lorenz-driven sine-grain clouds), and **drumm**
    (@infinitedigits, crossfading bass/melodic layers in pumping Freeverb)
  - draen_ugens.hpp gains Pluck (Karplus-Strong), LorenzL (sub-stepped Euler with
    divergence guard), FBSineN, a high-shelf biquad, and crossover-distortion and
    sine-shaper waveshapers

## [2.6.8] - 2026-07-08
### Added
  - dræn: three more dronecaster engines — **eno** (@infinitedigits, Music for
    Airports: chorused chord saws, a Klank, and a comb-string "piano" walking
    Eno's note rows), **belong** (@infinitedigits, saws overdubbing a 16-beat
    tape loop, with a kick gated behind amp > 0.7), and **ruins** (@rplktr &
    @sixolet, self-clocked metallic FM hits in a long wow-and-flutter wash)
  - draen_ugens.hpp gains Decay2, Compander, a peaking-EQ and resonant-highpass
    biquad, and a GVerb approximation (8 damped combs, odd/even split to stereo)

## [2.6.7] - 2026-07-08
### Added
  - dræn: three more dronecaster engines — **gristle** (@infinitedigits, octave
    triangle-saws through a Greyhole cloud), **grove** (@sixolet, five self-gating
    pulsar-synthesis voices washed through Greyhole), and **shields**
    (@infinitedigits, double-combed saw pairs re-pitched off a slipping tape loop)
  - draen_ugens.hpp gains VarSaw, SetResetFF, Trig1, a curved Env.perc generator,
    a peak Limiter, the CombN-bank reverb block several engines share, and an
    approximation of Julian Parker's **Greyhole** (modulated allpass diffusers in
    a damped cross-fed stereo delay loop)

## [2.6.6] - 2026-07-08
### Added
  - dræn: four more dronecaster engines — **mt. zion** (@license, five S&H-wandering
    pulse harmonics), **mika** (@infinitedigits, allpass-retimed sine pings over a
    pulse+noise bass), **fieldsteel** (after Eli Fieldsteel's Tutorial 15, demand-
    picked band-passed saws with a resonant "marimba"), and **malone**
    (@infinitedigits, eight organ voices stepping a demand-sequenced chord table)
  - draen_ugens.hpp gains the demand-rate layer — Dseq, Drand, Dxrand and Dbrown
    generators polled on trigger edges — plus TExpRand, TDelay, CoinGate, an
    interpolated AllpassC and a midiratio helper

## [2.6.5] - 2026-07-08
### Added
  - dræn: two more dronecaster engines — **toshiya** (@infinitedigits, interval-
    jumping sines with a Klank resonator bank) and **magicicada** (@sixolet, a
    no-input-mixer feedback drone with crossfading delay banks)
  - draen_ugens.hpp gains Ringz (the resonator behind Klank), BrownNoise, a
    second-order BAllPass, a TPT state-variable filter (SVF), and an N-element
    SelectX crossfade

## [2.6.4] - 2026-07-08
### Added
  - dræn: three more dronecaster engines — **unrelacc** (@zebra, six Hénon-map
    chaotic oscillators in intervals), **dreamcrusher** (@infinitedigits, a
    no-input-mixer feedback drone), and **rehberg** (@infinitedigits, folded and
    DFM1-filtered tape-warble pulses drenched in Freeverb)
  - draen_ugens.hpp gains a faithful port of Jezar's public-domain **Freeverb**
    (8 damped combs → 4 allpasses per channel), plus HenonC, LFSaw, fold,
    Changed, Amplitude, OnePole, Balance2 and a DFM1 filter approximation

## [2.6.3] - 2026-07-08
### Added
  - dræn: four more dronecaster engines — **starlids** (@infinitedigits, PWM sub
    + 12 interval-stepping saws through a Moog ladder), **mt. lion** (@license,
    9 comb-resonated pulse voices driven by sample-and-held noise), **apparatus**
    (Josue Arias, clipped-triangle generators with mains hum and a crackle bed),
    and **eliane** (@sixolet, 7 sines phase-modulating in a feedback ring — an
    Éliane Radigue homage)
  - draen_ugens.hpp gains CombN, LFPulse, LFPar, Dust2, Crackle, and softclip /
    Rotate2 helpers; SC LocalIn/LocalOut is modelled as a one-sample feedback bus
### Fixed
  - draen_ugens.hpp: the per-voice RNG now avalanche-hashes its seed, so nearby
    seeds (s, s+7, …) decorrelate — xorshift alone gave correlated first outputs,
    which could e.g. clip all of Eliane's amplitude gates to zero (silence)
  - draen_ugens.hpp: combFeedback now handles negative decay times (negative
    feedback of equal magnitude), matching SC's comb behaviour

## [2.6.2] - 2026-07-08
### Added
  - dræn: two more dronecaster engines, both @infinitedigits — **coil**
    (12 Dust-triggered feedback-sine/noise voices through a long reverb) and
    **sachiko** (4 DPW-pulse voices modulated by slow triangle banks, into a
    global Moog ladder and reverb)
  - draen_ugens.hpp gains a reusable delay-line layer: interpolated delay lines,
    CombL/CombC feedback combs, Schroeder AllpassN, and a shared SchroederReverb
    (DelayN → 7×CombL → 4×AllpassN) — the reverb block copied across many
    dronecaster SynthDefs, now built once
  - draen_ugens.hpp also gains Impulse, Trig, TChoose, SinOscFB, a breakpoint
    EnvGen, an ASR attack env, a Moog ladder (MoogFF) and LeakDC
  - dræn: engine init now receives the sample rate, so delay-based engines size
    their buffers correctly and rebuild on sample-rate changes

## [2.6.1] - 2026-07-07
### Added
  - dræn: three more engines ported from dronecaster — **harm's way**
    (@moonblind, 16 amplitude-modulated harmonics), **thx** (@infinitedigits,
    the THX Deep Note sweep, with amp as the sweep position), and **hecker**
    (@infinitedigits, stereo banks of filtered white/pink noise)
  - dræn: per-engine makeup gain so engines are loudness-matched and switching
    between them no longer jumps levels
  - draen_ugens.hpp gains LFNoise2, WhiteNoise, PinkNoise, Dust, Latch, Lag
    (VarLag), BLowPass, and the Pan2 / SelectX / linexp / linlin / midicps
    helpers needed by the new engines

## [2.6.0] - 2026-07-07
### Added
  - dræn: drone synthesizer, a port of the dronecaster norns instrument
    (github.com/northern-information/dronecaster, GPL-3.0). A bank of drone
    engines is played from two controls, fundamental (hz) and level (amp), with
    a third selecting the engine; switching fades the current engine down and
    the next one up, mirroring dronecaster's SynthSocket
  - dræn: hz knob + CV (right-click switches the CV input between 1V/oct and
    linear 100 Hz/V), amp knob + CV, and an engine knob + CV with a runtime
    display of the selected engine's name
  - dræn: fade time selectable from the context menu (0.25 s .. 8 s)
  - dræn: initial engine roster — sine, square, triangle, supersaw — ported
    faithfully from the original SynthDefs, with author credits preserved
  - a reusable SuperCollider-UGEN DSP layer (src/draen_ugens.hpp) underpins the
    engines: SinOsc, LFTri, band-limited Saw/Pulse (via Rack's MinBLEP), the
    SC second-order filters over Rack's biquad, LFNoise0/1 and Splay — a
    vocabulary for future SC-to-C++ ports

## [2.5.1] - 2026-07-07
### Added
  - pellicula: "Shift all samples +8 / -8" context-menu actions advance or rewind
    every voice's sample selection by 8, wrapping at 64 — from the default 1-8 each
    click steps the whole module to the next contiguous bank, to audition a 64-sample
    kit eight sounds at a time

## [2.5.0] - 2026-07-07
### Added
  - pellicula: "exploded" 8-voice one-shot drum sampler in the spirit of the Erica
    Synths Pico DRUM sample-player engine, rebuilt clean-room from the published
    manual (12-bit / 44.1 kHz character, pitch, decay and level per voice)
  - pellicula: full control matrix — every voice has a knob and a CV input for
    sample-select, pitch, decay and level, plus a manual trig button and a trig input
  - pellicula: poly normalling — each input row has a poly jack (channel N drives
    voice N) alongside 8 mono jacks; a patched mono jack overrides its voice
  - pellicula: sample-select is 1V/oct semitone-quantized (0 V = the knob's sample)
  - pellicula: no samples are bundled — a global "kits folder" is set from the context
    menu, and each immediate subfolder is a selectable kit of up to 64 .wav files
    (mono/stereo, 8/16/24/32-bit int or float, any rate) ordered by filename; kits load
    on a background thread and swap in glitch-free, and the choice is saved with the patch
  - pellicula: per-voice outputs, an 8-channel poly output and a summed mix output
  - pellicula: per-voice choke groups and a 12-bit-playback toggle in the context menu
  - pellicula: 22HP matrix panel generated by tools/gen_pellicula_panel.py
### Fixed
  - panel-editor: resolve label characters through the font cmap so digits and
    punctuation bake correctly (previously only glyphs named by their character,
    i.e. letters, rendered)

## [2.4.0] - 2026-06-14
### Added
  - scando: scanned-synthesis oscillator (Verplank / Mathews / Shaw technique) — a
    fixed-end mass-spring string (the non-circular topology of Csound's scansyn /
    Qu-Bit Scanned) forms a slowly-evolving wavetable scanned at audio rate for a
    pitched, organically-shifting tone
  - scando: Mass, Stiffness, Damping, Centering and Hammer-shape controls reshape the
    string dynamics; Shape morphs the hammer between sine, saw, noise and dual-pulse
  - scando: Strength control drives the string continuously with the hammer shape for
    a self-sustaining tone; Update Rate sets the string's physics rate (~500 Hz–8 kHz)
  - scando: Fine tune (±7 semitones); Inject audio input with an In-Level attenuator
  - scando: EXCITE trigger hammers the string to the current shape (a pluck)
  - scando: 1V/oct pitch input and CV inputs for mass, stiffness, damping, centering,
    shape, strength and rate
  - scando: audio output with a self-levelling limiter, level LED, 16HP panel

## [2.3.1] - 2026-06-14
### Added
  - limen: `hello` command — protocol version and capability discovery
  - limen: `get_param` command — read back a single parameter's value and metadata
  - limen: window/view commands `set_fullscreen`, `zoom_to_modules`, and `quit`, for scripting patch screenshots
  - cli: Python limen client (`tools/cli/limen.py`) alongside the C client
  - cli: subcommands for the new protocol commands (`hello`, `param`, `fullscreen`, `zoom`, `quit`)
  - tools: `gen_patches.py` and a minimal `patches/limen.vcv` that launches Rack straight into a server-enabled, controllable state
  - tools: `gen_title_paths.py` and `measure_text.py` for generating OCR-A panel titles
  - docs: README section on controlling Rack externally via limen, including the loopback-only security model

### Fixed
  - alea: guard against undefined behaviour when no modules are available

### Changed
  - tools: reorganised `tools/` into `cli/` and `panel-editor/`, with a panel-editor README

## [2.3.0] - 2026-05-09
### Added
  - MMCCCXCIX: PT2399 delay chip emulation with feedback send/return loop
  - MMCCCXCIX: CV inputs for all parameters (time, feedback, mix, brightness, fb loop mix)
  - MMCCCXCIX: external feedback send/return loop with normalled bypass and blend control
  - MMCCCXCIX: soft compressor on wet output to limit self-oscillation amplitude
  - tools: panel-editor.py — browser-based drag-and-drop panel layout editor

## [2.2.1] - 2026-03-14
### Added
  - limen: `list_ports` command to query input/output port names by module id
  - limen: `list_models` command to enumerate available models (optional plugin filter)
  - limen: module filter and verbose mode (`outputModuleName`, `outputPortName`, etc.) for `list_cables`
  - cli: cable id prefix resolution for `disconnect`
  - docs: per-module documentation pages in `doc/`
  - docs: Latin naming section and About page in readme

## [2.2.0] - 2026-03-08
### Added
  - limen: TCP + JSON control interface for controlling VCV Rack from external tools
  - limen: right-click menu to enable/disable server and select TCP port
  - cli: limen command-line client (list modules/plugins, add/remove modules, connect/disconnect cables)

### Fixed
  - limen: Windows (win-x64) build compatibility via Winsock2 shim

## [2.1.0] - 2023-01-10
### Added
  - cumuli: reset input gate and button
  - cumuli: up and down buttons
  - cumuli: monopolar/bipolar selector

## [2.0.2] - 2023-01-01
### Fixed
  - cumuli: corrected z-order of output white outline

### Changed
  - forsitan: updated github action build script (arm64 support)

## [2.0.1] - 2022-02-07
### Added
  - readme: screenshots of v2.0
  - interea: bypass behaviour
  - interea, cumuli, deinde, pavo: labels to lights, inputs, outputs

## [2.0.0] - 2022-01-30
### Changed
 - forsitan: recompiled the plugin with Rack SDK v2.0.x

### Fixed
 - alea: modified module spawn code to compile with Rack v2.0.x

## [1.4.2] - 2021-05-16
### Changed
- alea: simplified panel svg code
- cumuli: simplified panel svg code
- deinde: simplified panel svg code
- pavo: simplified panel svg code

### Fixed
- plugin.json: corrected the link to the CHANGELOG on github
- changelog: fixed sub-headers indentation

## [1.4.1] - 2021-05-13
### Fixed
- interea: the selection of the chord quality when the harmonic option
is on is now based on the V/Octave input only, using thus the frequency
knob as the root note of the scale.
