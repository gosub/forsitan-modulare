# imber

**Generative rain: eight sample players on a field where position is
composition, everything synthesized from nothing.**

*imber* is Latin for "rain shower, downpour". The module is inspired by
Giorgio Sancristoforo's **Haiku**, "a generative record" — an app whose
entire sample bank is synthesized from scratch at every launch, then
arranged live by a clocked, probabilistic engine. imber ports that model
to Rack: the architecture, timing model, effect designs and voice
behavior follow a reverse-engineering of the original; the generator
magic numbers are tuned by ear; every sound is procedurally rendered by
a background thread and never loaded from disk.

## The field

The display shows a unit field. Three populations live on it:

- **8 players** (numbered dots) — looping sample voices, placed with the
  per-player X/Y knobs (plus the polyphonic XCV/YCV inputs: channel N
  drives player N, ±5 V spans the whole field; a **mono** cable moves
  every player at once). A player's X position is also its stereo pan.
- **5 clocks** (diamonds) — one per division 2n/4n/8n/16n/32n.
- **8 effects** (squares) — one per kind: rev, lpf, hpf, bpf, bit, dly,
  grn, rvb.

Position is routing:

- A player sounds only when a clock is within **REACH** (the nearest one
  wins). The clock's division sets how fast the player's loop window
  churns: a 2n-bound player drifts lazily, a 32n-bound one boils.
- A player picks up **every** effect within REACH; they stack in Haiku's
  fixed order (filters and quantisation before the spatial trio —
  rev → lpf → hpf → bpf → bit → dly → grn → rvb). The delay is a 375 ms
  tap with 0.5 feedback; the reverb a Schroeder (RVL sets its decay,
  RT60 0.3 s → 10 s); grn a granular ring whose taps re-roll every
  ~160 ms; the band-pass rolls three log-random centers on every sample
  load.
- REACH (with CV) scales both radii at once — it is the density macro,
  the "weather knob": small reach = sparse drizzle, large = downpour.

### Constellations

Clocks and effects are not placed by hand (the one deliberate deviation
from the original's mouse canvas). Each object owns two rolled
positions, **constellation A and B**, and the CLK / FX morph knobs
(with CV) glide every object between them — a whole weather system on
one knob. Divisions and effect kinds are fixed forever; only positions
move.

- **rr clk / rr fx** (buttons + triggers) — reroll new A and B
  constellations. Placement is stratified (a jittered grid), so a roll
  can never pile everything into one corner and leave the field dead.
- **nd clk / nd fx** — a small drunk step on all positions of both
  constellations: the field shifts, nothing is lost.

### Couplings

Players that line up influence each other (COUPLE knob + CV scales all
three; the display draws the pair lines):

- **vertically aligned** (same X, yellow) — read heads drift toward a
  common playback phase;
- **horizontally aligned** (same Y, teal) — loop windows converge, at
  half that rate;
- **diagonal** (orange) — the pair occasionally swaps read positions.

## Players

Each of the 8 columns: **X**, **Y**, **CHG** (probability of loading a
new random sample, evaluated on every quarter note — exponentially
mapped, so low values are genuinely rare; default ≈ 10%), and a **½/1/2**
speed switch (multiplied with the global SPD). The LED shows the bound
division's color (red 2n → blue 32n, brightness = activity); dark means
out of reach, and silent. Loop windows drunk-walk in ±200 ms steps up to
LPM seconds long; samples come from a 64-buffer loop bank via an urn
(never the same one twice in a row).

## Clocks: drunk time

The master clock (BPM 1–250, exponential knob — the bottom of the range
is glacial, a quarter note per minute; at the top a 32nd lands every
30 ms, which is grain rate rather than loop rate; the CV is 1 V/oct,
+1 V doubles the tempo, and can push past the knob up to 500) derives all five
divisions, and every division edge is jittered by a bounded random walk — Haiku's signature
"structured but never repeating" micro-timing. The same jittered edges
drive the voices and the five **gate outputs**, so external gear locks
to imber-time, not metronome-time.

## Skip and Micro

- **Skip** — on every 8th note, with probability SKP, fires a CD-skip
  buffer (frozen segment repeats, comb + tanh, choppy gate) at speed
  SKS, level SCV.
- **Micro** — on each beat of its division (DIV, 5 positions; the
  division itself drifts at high instability) rolls two independent
  dice: change the current micro-sound, and/or play it (PLS, with CV).
  **INSTAB** collapses the original's change/variability/division-drift
  trio into one macro: at 0 one sound repeats on a fixed grid, at 1 it
  is a fully shifting cloud. PLV sets the level.

Both have their own pre-master mono outputs.

## Master chain

sum/8 → tube warmth (always on, gentle) → **BIT** master bitcrush (TPDF
dither + post-highpass) → **NSE** noise inject (band-limited, gated open
by the signal itself) → **TAP** tape wow/flutter/age → **VOL** (up to
+8 dB) into a soft limiter with a −1 dBFS ceiling. **RVL** sets the
players' reverb decay. All four have CV.

## Transport

- **ON** — mutes and freezes the engine (fades, no clicks).
- **RND** (button + trigger) — the original's big red button: rerolls
  both constellations, randomizes every fader except VOL (lo-fi faders
  biased low), reseeds the timing walks.
- **SEED** (button + trigger) — regenerates the whole sample bank
  (64 loops + 64 skips + 64 micros) in a worker thread; the old bank
  keeps playing until the new one lands (progress bar in the display).
- **CLR** — spreads the players back out to a tidy grid.

## Impermanence

The bank seed and both constellations are saved with the patch, so a
reload sounds identical. Turn on **Ephemeral** in the context menu and
nothing is saved: every load rolls a new bank and new constellations —
permanence is an illusion.

## Sound material

All generated at seed time from ~22 loop archetypes (drones, pads,
bells, karplus plucks, fragments, glitch, ambients), 14 micro archetypes
and the CD-skip chain — see [sylla](sylla.md), which plays the same
library as a standalone voice. Every pitch snaps to one pentatonic-minor
table on D; a randomized lo-fi pass keeps it bruised. The skip/micro
taxonomy, fader ranges, urn selection, drunk timing, effect designs and
the −1 dBFS soft limit are the reverse-engineered Haiku architecture;
the exact partials and envelopes are imber's own, tuned by ear.
