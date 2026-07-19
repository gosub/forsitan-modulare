# sylla

**Random sample generator and player: press GEN, get a sound that has
never existed before.**

*sylla* is a clipped *syllaba*, Latin for "syllable" — haiku are counted
in syllables, and sylla speaks one small sound at a time. It is the
standalone voice of the [imber](imber.md) pair, inspired by Giorgio
Sancristoforo's **Haiku**, whose sample material is synthesized from
nothing at every launch. sylla exposes that generator library as a
normal sampler voice: no files, no disk, every sound rendered fresh by
a background thread while the previous one keeps playing.

## Families

The FAMILY knob (snap) picks which corner of the generator library the
next GEN press draws from:

| family | character |
|--------|-----------|
| **drone** | sustained looping cores: additive/detuned stacks, 2-op FM, resonant filtered noise, sub with drive, comb-fed noise |
| **pad** | slow multi-voice chords and shimmering clusters |
| **fragment** | short glitchy hits: dirty plucks, stutters, grain clouds, noise bursts, chord stabs, little melodies |
| **bell** | additive strikes, harmonic or inharmonic partial sets with exponential decays |
| **ambient** | long and soft: filtered washes over faint chord beds, wowing tape pads, sparse chimes |
| **glitch** | bubbly sample-and-hold tones |
| **karplus** | Karplus-Strong plucks and runs |
| **skip** | CD-skip material: a frozen segment repeated, comb + tanh, choppy gate |
| **micro** | tiny one-shots (grains, blips, ticks, chirps, thumps) — milliseconds long |
| **random** | let the sample's own seed pick the family, so every GEN is a surprise |

Every pitch is quantized to one pentatonic-minor table on D — the tonal
glue that keeps random material musical. All families get a light,
randomized lo-fi "dirt" pass.

## Controls

- **FAMILY** — generator family for the next render (snap knob). The
  last position, *random*, derives the family from the seed, so a
  reloaded patch still comes back with the same sound.
- **GEN** (button + trigger input) — render a new sample. The yellow LED
  lights while the worker thread is busy; the old sample plays until the
  new one lands. Requests during a render are ignored.
- **SPEED** — playback rate 0.1–2×. The CV input adds ±1 V/oct around
  the knob.
- **LEN** — play window, as a fraction of the buffer measured from its
  start (min 30 ms), with soft window edges.
- **LEVEL** — output level.
- **LOOP** switch — *loop* wraps at the window end; *one-shot* stops
  there.
- **GATE** switch — *trigger* plays the whole window per trigger;
  *gate* sustains only while the TRIG input is high.
- **PLAY** button — fires (or retriggers) playback by hand, with no
  cable patched. In gate mode it acts as a gate source of its own:
  the sound sustains while it is held.

## Patching

- **TRIG** fires (or retriggers) playback. With nothing patched and
  LOOP on, sylla free-runs — an instant generative drone/texture source.
  With nothing patched and one-shot, each freshly generated sample plays
  itself once.
- **OUT** is mono (the generators are mono by design; pan or spread it
  downstream — [pavo](pavo.md) is a good neighbor). The LED on the badge
  shows level.
- **EOC** emits a trigger at each window end / loop wrap — patch it back
  into GEN for a sound that regenerates itself forever.

## Impermanence

True to Haiku's "built from nothing" spirit, the rendered sample is not
saved with the patch — only its seed is. Reloading the patch regenerates
the exact same sound; pressing GEN never brings one back.
