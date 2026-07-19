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
  new one lands. Requests during a render are ignored. GEN only changes
  what is loaded, never whether it plays: a stopped sylla stays quiet.
- **SPEED** — playback rate 0.1–2×. The CV input adds ±1 V/oct around
  the knob.
- **LEN** — play window, as a fraction of the buffer measured from its
  start (min 30 ms), with soft window edges.
- **LEVEL** — output level.
- **LOOP** switch — *loop* wraps at the window end; *one-shot* stops
  there.
- **GATE** switch — *trigger* responds to rising edges; *gate* follows
  the level.
- **PLAY** button — a trigger and a gate source in its own right, so the
  whole transport works with nothing patched.

## Transport

LOOP and GATE together make a small square, and PLAY or the TRIG input
drive it identically:

| | trigger mode | gate mode |
|---|---|---|
| **one-shot** | an edge plays the window once | plays while the gate is high, and stops at the window end even if the gate stays up |
| **loop** | an edge toggles the loop on / off | loops while the gate is high |

A fresh sylla starts in one-shot + trigger, so it sits quiet until you
ask for a sound: press PLAY to speak the sample once. The loop toggle
starts *on*, so flipping LOOP up drones straight away with nothing
patched — an instant generative drone/texture source. PLAY stops it,
another press starts it over. The run state is saved with the patch.

Nothing ever cuts mid-signal. Retriggering a sounding window, and a
freshly generated sample landing under a playing head, both hand over
through a 4 ms crossfade: the outgoing audio keeps playing from a
second read head while the new one comes up under it. Hammer PLAY or
GEN as fast as you like, it stays clean.

## Patching

- **TRIG** drives the transport square above; it needs no cable, since
  PLAY does the same job by hand.
- **OUT** is mono (the generators are mono by design; pan or spread it
  downstream — [pavo](pavo.md) is a good neighbor). The LED on the badge
  shows level.
- **EOC** emits a trigger at each window end / loop wrap — patch it back
  into GEN with LOOP on for a drone that renews itself at every wrap. In
  one-shot, patch it to both GEN and TRIG, since generating alone no
  longer restarts playback.

## Impermanence

True to Haiku's "built from nothing" spirit, the rendered sample is not
saved with the patch — only its seed is. Reloading the patch regenerates
the exact same sound; pressing GEN never brings one back.
