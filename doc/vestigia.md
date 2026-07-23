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

It is built from the *Vestigium* design document (v0.2). This first
release implements the documented MVP: two playback heads, eight
active-region descriptors, a 16-second horizon, no external clock, no
per-voice polyphony.

## Memory mode

The right-hand three-way switch decides how incoming audio interacts with
whatever is already stored at the write position:

- **oblivion** — the present replaces the past. `buffer = input`. The
  clearest picture of the recent past; silence erases older material as
  the loop passes over it.
- **remanence** — the present rewrites the past but leaves traces behind.
  A fixed 30% of the old content is kept (`buffer = old*0.30 +
  new*0.70`), so events fade across repeated passes and silence
  *attenuates* memory rather than erasing it.
- **sediment** — the present accumulates on top of the past
  (`buffer = softSaturate(old + input)`), through a DC blocker and soft
  saturation, growing denser and more compressed with every cycle.

## Recollection mode

The left-hand three-way switch decides *when* memories return. These are
separate from the memory modes:

- **listen** — event-centered. Recall is triggered by transients and
  prefers recent, energetic regions. Suited to percussion, sequences and
  guitar.
- **breathe** — the recall rate follows the input envelope and density,
  and favors regions with energy close to the present. Suited to pads,
  drones and expressive playing.
- **dream** — recall probability *rises as the input falls quiet*. Older
  regions get more weight, reverse playback is more common and fades are
  longer. dream never recalls a fully silent region; it answers present
  silence with stored activity.

## The activity map

Recording and recollection are separate systems. The circular buffer says
what physically exists in memory; a parallel map, divided into 20 ms
blocks, says *where meaningful material is*. Each block tracks its stored
energy and a hysteretic active/inactive gate (opening near −45 dBFS,
closing near −52 dBFS, so a phrase with short internal gaps stays a single
region). The recollection engine builds valid regions from this map,
weights them by activity, age, mode and a cooldown on the just-played
region, and picks one; if nothing valid exists, nothing happens and
**event out** does not fire.

## Controls

Each macro knob is a coordinated bundle rather than a single parameter:

- **memory** — the temporal horizon available to recall, 50 ms to 16 s
  (exponential). Sets how far back regions may be drawn from.
- **recall** — the rate and probability of automatic recollection events.
  Fully down, only manual **event** triggers recall.
- **age** — how hard recalled memories deteriorate: progressive low-pass,
  sample-rate reduction, bit-depth reduction and timing jitter, worsening
  each time a memory is revisited.
- **smear** — turns distinct fragments into a diffuse all-pass cloud;
  from discrete repeats up to reverb-adjacent texture.
- **forget** — how fast memory and feedback lose persistence. Low forget
  lets recalled material feed back and persist; high forget drops it
  quickly.
- **temper** — behavioral instability: timing and speed jitter, spatial
  drift, per-recollection variation. Zero is stable and near-deterministic.
- **direction** — the probability of reverse versus forward playback,
  chosen fresh per recollection (not a crossfade).
- **mix** — equal-power dry/wet.
- **out** — final output level, unity at 12 o'clock.

CV inputs (unipolar, ±5 V nudges the knob) are provided for **memory**,
**recall**, **age** and **smear**. (The design document's remaining macro
CV inputs are deferred.)

## Buttons, triggers and outputs

- **freeze** — stops writing to the buffer while recollection, feedback
  and degradation keep running. The logical transport keeps advancing, so
  timing stays consistent when freeze is released. **frz** gate input; the
  context menu switches the button between gate+toggle and toggle.
- **event** — forces a recall from a valid region (and fires **event
  out** only if one actually starts). **evt** trigger input.
- **clear** — wipes the buffer, activity map, playback heads and feedback.
  **clr** trigger input.
- **audio in L / R** — R is normalled from L, so a mono source produces a
  meaningful stereo result.
- **audio out L / R** — the stereo memory effect.
- **evt out** — a short 10 V pulse when a real recollection begins.
- **env out** — the envelope of the incoming signal, 0–10 V.
- **chaos out** — a slow ±5 V voltage derived from the internal state
  (mean active-region age blended with live-head activity), for
  self-modulation.

## Display

The strip across the top is a linear view of the tape: yellow bars show
stored energy per block, a moving line marks the write head (dimmed when
frozen), and the panel flashes on each recollection.

## Notes

The audio buffer is **not** saved with the patch; only the random seed is,
so probabilistic behavior is reproducible across a reload. The feedback
path is bounded by soft saturation, DC blocking and non-finite guards, so
sediment accumulation and low **forget** can be pushed without numerical
failure. A context-menu **mono output** option sums L/R at equal power.
