# vestigia

**Stereo memory effect: an endless tape loop, continuously rewritten,
that remembers meaningful fragments and returns them imperfectly.**

*vestigia* is Latin for "traces, footprints" — what sound leaves behind.
The module is not a delay. A write head runs across a circular stereo
buffer without ever stopping; what the present does to the past is set by
the **memory mode**. In parallel, vestigia keeps a block-based *activity
map* of where meaningful sound actually lives, so its recollection engine
only ever recalls regions that contain audio — never silence. Each time a
memory returns it deteriorates a little more.

It implements the full *Vestigium* design document (v0.2): the three
memory modes and three recollection modes, a memory-descriptor pool with
per-region integrity and wear-per-recollection, sonic-similarity
weighting, Hermite interpolation that degrades toward nearest-neighbour
with age, a modulated all-pass diffusion network with crossfeed,
cross-fed protected feedback, every macro CV, and the seven factory
presets.

## Memory mode

The right-hand three-way switch decides how incoming audio interacts with
whatever is already stored at the write position:

- **oblivion** — the present replaces the past. `buffer = input`. The
  clearest picture of the recent past; silence erases older material as
  the loop passes over it.
- **remanence** — the present rewrites the past but leaves traces behind.
  `buffer = old*retention + new*(1-retention)`, retention 30% by default
  (10/30/50/70% in the context menu), so events fade across passes and
  silence *attenuates* memory rather than erasing it.
- **sediment** — the present accumulates on top of the past
  (`buffer = saturate(old + input*amount)`) through a DC blocker, growing
  denser and more compressed with every cycle. The input amount
  (0.5–2.0x) and the saturation character (soft / tape / fold) are
  context-menu options.

## Recollection mode

The left-hand three-way switch decides *when* memories return. These are
separate from the memory modes:

- **listen** — event-centered. A short window is built around the
  strongest block of a region, triggered by transients, preferring recent
  and energetic material. Suited to percussion, sequences and guitar.
- **breathe** — the recall rate follows the input envelope and density,
  and favors regions whose energy *and zero-crossing character* resemble
  the present (sonic similarity). Suited to pads, drones and expressive
  playing.
- **dream** — recall probability *rises as the input falls quiet*. Older
  regions get more weight, reverse playback is more common, fades and
  padding are longer. dream never recalls a fully silent region; it
  answers present silence with stored activity.

At high **recall** or low **memory** the engine may extract a smaller
sub-region from a longer phrase instead of replaying the whole thing.

## The activity map and descriptors

Recording and recollection are separate systems. The circular buffer says
what physically exists in memory; a parallel map, divided into 20 ms
blocks, says *where meaningful material is*. Each block tracks stored
energy, transient strength, a coarse zero-crossing rate and a hysteretic
active/inactive gate (opening near −45 dBFS, closing near −52 dBFS, so a
phrase with short internal gaps stays a single region). Valid regions are
built from this map and weighted by activity, age, mode, similarity, a
cooldown on the just-played region, and a pool of **memory descriptors**
that carry each region's **integrity** (it decays as the write head
passes back over the region, at a rate set by the memory mode) and its
**recall count** (each replay adds a little permanent wear, so a memory
grows less faithful the more it returns). If nothing valid exists, nothing
happens and **event out** does not fire.

## Controls

Each macro knob is a coordinated bundle rather than a single parameter:

- **memory** — the temporal horizon available to recall, 50 ms up to the
  buffer size (exponential).
- **recall** — the rate and probability of automatic recollection events.
  Fully down, only manual **event** triggers recall.
- **age** — how hard recalled memories deteriorate: Hermite interpolation
  morphing toward nearest-neighbour, progressive low-pass, sample-rate
  reduction, bit-depth reduction, timing jitter and, at the top, dropouts
  — worsening each time a memory is revisited.
- **smear** — turns distinct fragments into a diffuse, slowly modulated
  all-pass cloud with stereo crossfeed; from discrete repeats up to
  reverb-adjacent texture.
- **forget** — how fast memory and feedback lose persistence. Low forget
  lets recalled material feed back and persist; high forget drops it.
- **temper** — behavioral instability: timing and speed jitter, spatial
  drift, per-recollection variation. Zero is stable and near-deterministic.
- **direction** — the probability of reverse versus forward playback,
  chosen fresh per recollection (not a crossfade).
- **harmony** — how the pitch of each recalled fragment relates to the
  source. Playback is pitch-quantized rather than freely detuned: fully
  down, fragments return only at unison and octaves, so they stay in tune
  with the source; turning it up widens the interval pool through fifths
  and fourths, thirds, and on toward the full chromatic set; near the top
  a continuous detune is layered on for a deliberately inharmonic,
  out-of-tune drift. **temper** still adds a small wow on top, but no
  longer throws the pitch off its grid.
- **mix** — equal-power dry/wet.
- **out** — final output level, unity at 12 o'clock.

Every macro (memory, recall, age, smear, forget, temper, direction) has a
CV input; each nudges its knob (±5 V ≈ ±half travel).

## Buttons, triggers and outputs

- **freeze** — stops writing to the buffer while recollection, feedback
  and degradation keep running. The logical transport keeps advancing, so
  timing stays consistent when freeze is released. **frz** gate input; the
  context menu offers gate / toggle / toggle-on-rising-edge.
- **event** — forces a recall from a valid region (and fires **event
  out** only if one actually starts). **evt** trigger input.
- **clear** — wipes the buffer, activity map, descriptors, playback heads
  and feedback. **clr** trigger input.
- **audio in L / R** — R is normalled from L, so a mono source produces a
  meaningful stereo result.
- **audio out L / R** — the stereo memory effect.
- **mem out** — the recalled material before smear, feedback and mixing,
  for parallel processing or alternate feedback paths.
- **evt out** — a short 10 V pulse when a real recollection begins.
- **env out** — the envelope of the incoming signal, 0–10 V.
- **chaos out** — a slow voltage derived from the internal state (mean
  active-region age blended with live-head activity); the context menu
  makes it stepped or smooth, bipolar (±5 V) or unipolar (0–10 V).

## Display

The strip across the top is a linear view of the tape: yellow bars show
stored energy per block, a moving line marks the write head (dimmed when
frozen), and the panel flashes on each recollection.

## Context menu

- **Quality** — Eco / Standard / High trades CPU for the descriptor pool
  and region count (4 / 8 / 16).
- **Buffer size** — 4 / 8 / 16 / 32 s of memory.
- **Remanence retention**, **Sediment input amount**, **Sediment
  saturation** (soft / tape / fold).
- **Freeze behavior**, **Mono output** (stereo / left / sum / equal-power
  sum).
- **Random seed** (random on load / fixed) and a **Reseed now** action.
- **Chaos out** stepped / bipolar toggles.
- **Safety**: feedback limiter, soft clipping, preserve tails on bypass.
- **Bypass clears memory** and **Save memory with patch** (off by
  default).

## Factory presets

Faint Trace, Broken Repeater, Last Chord, Oxidized Tape, Percussive
Ghost, Sedimentary Drone and Empty Room — the seven starting points from
the design document.

## Notes

The audio buffer is not saved with the patch unless **Save memory with
patch** is enabled; otherwise only the random seed survives a reload, so
probabilistic behavior is reproducible. The feedback path is bounded by
soft saturation, DC blocking, a limiter and non-finite guards, so sediment
accumulation and low **forget** can be pushed without numerical failure.
