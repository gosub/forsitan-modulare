# tabes

**Disintegration looper: a tape loop that crumbles a little more on every
pass.**

*tabes* is Latin for "wasting away, decay, consumption". Record a phrase and
let it loop: the play head reads the tape and the write head re-records a
slightly worse copy in place, so the loss accumulates pass over pass, the
way a real tape loop dies. High frequencies dull, gentle saturation
compresses, the level sags, hiss creeps in, and dropouts appear more and
more often as the loop wears out. William Basinski's *Disintegration Loops*,
as a module.

A pristine copy of the original recording is kept off-tape. **splice** puts
fresh tape in the machine: the loop restarts from the original recording at
pass zero. The swap is crossfaded over about 10 ms, so however worn the tape
has become, splicing back to it doesn't click.

## Recording

- Press **rec** to start recording, press again to stop; the recorded length
  becomes the loop length (up to 30 seconds).
- Or drive the **gate** input: recording follows the gate (high = recording).
- Recording replaces the previous loop and resets the age.
- Recording is clickless in both directions. Pressing rec crossfades between
  the input monitor and the loop over about 10 ms, so neither the start nor
  the stop clicks or thumps regardless of where in the waveform you press.
- Stopping is clickless at the loop point too, even when the end of the
  recording doesn't line up with the beginning: for the first few
  milliseconds after the stop, the loop's head is crossfaded with the live
  input, so the seam carries real audio instead of a jump. Let the source
  keep playing for a moment after you stop recording and the seam will be
  seamless in the literal sense.
- While recording, the input is monitored at the output; when recording
  stops, monitoring stops with it and you hear only the loop. While the
  tape is empty (before the first recording, after "Clear loop", or after
  a too-short recording), the input also passes through, so the module is
  never a dead end in a chain. The right-click "Monitor input" menu offers
  **While recording or empty** (default), **Always** (input passes through
  during playback too), and **Never**.

## Controls

| control | function |
|---------|----------|
| **decay** (+ **cv**) | how much is lost per pass. Low: archival, takes hundreds of passes to change. High: audibly worse every pass, crumbles in minutes |
| **wow** (+ **cv**) | wow/flutter depth on the play head (grows slightly with age) |
| **overlap** (+ **cv**) | how soon the next play head starts. Two heads take turns playing the loop straight through; each new head starts *overlap* earlier than the previous one finishes, and the two crossfade (equal power) where they meet. At zero they play back to back (a plain loop). Turned up, the next repeat begins before the last has ended — at the max it starts when the current head is halfway, so the loop plays over itself for an ambient wash. The tape still ages underneath, so it keeps rotting while it smears |
| **send** (+ **cv**) | how much of the **return** signal is re-recorded onto the tape (see FX loop below); default 50% |
| **rec** / **gate** | record toggle / gate |
| **splice** / **trig** | restore the pristine recording, age back to zero |

## FX loop (send / return)

The **send** jack carries the loop read out to an external effect; patch its
output back into **return** and the **send** knob folds that processed signal
onto the tape as it re-records. Because the write head bakes it in every pass,
whatever the effect does **compounds**: a reverb blooms into a cloud, a pitch
shifter spirals, a filter recolours a little more each time round. With the
**send** knob at 0 the return is ignored; at higher settings the tape becomes
what your pedal makes of it (it defaults to 50%). The feedback carries the
usual one-sample delay, and the tape is bounded, so a hot effect saturates into
a drone rather than exploding.

The loop only acts when **both** ends are patched — the **send** jack out and
**return** back. With only **return** connected nothing is folded in (the
**send** knob does nothing), so a stray cable can't quietly overwrite the tape.

## Outputs

| jack | function |
|------|----------|
| **out** | the loop (plus the live input, per the "Monitor input" menu mode) |
| **send** | the loop read, out to an external effect (returns via **return**) |
| **ramp** | the audible play head as a 0–10V saw, one ramp per heard repeat (in step with **eoc**). A loop-locked phasor for sweeps, wavetable scans, or syncing other modules; it is clean of wow so it stays a stable clock. With **overlap** up the heard repeat is shorter than the tape (a new head starts every loop − overlap), and ramp follows that |
| **age** | 0.1V per completed pass, clamps at 10V. This is the tape odometer: it steps once per full rotation of the tape (the aging pass), regardless of **overlap**. Patch it somewhere: let the patch itself react to the tape dying |
| **eoc** | 1 ms trigger at every heard loop restart (its LED flashes with each one). With **overlap** at max that is twice per tape rotation; for a once-per-aging-pass signal, use the steps of **age** instead |

## Polyphony (stereo and beyond)

**in** and **out** are polyphonic. Patch a poly cable with two channels and
tabes records a **stereo** tape; the output carries the same two channels.
Any channel count up to 16 works, so a multi-track source becomes a
multi-track tape.

The tape has one **transport** and several **tracks**. Everything about the
tape's motion is shared across the tracks — the same wow/flutter warble, the
same dropouts, the same loop seam, the same crossfade when you press rec — so
a stereo image stays phase-locked and moves as one, which two separate mono
tabes could never do. Only the audio itself and the per-track tape hiss are
independent.

- The tape's width is set **when you record**, from the input's channel
  count at that moment. Playback outputs that many channels no matter what
  you patch in later, so repatching the input can't split a stereo loop.
- **age**, **eoc** and **ramp** are always monophonic (they describe the
  transport, not the audio). The CV inputs (**decay/wow/overlap/send cv**),
  **gate** and **splice trig** read channel 1. The **send** jack and **return**
  are polyphonic, matching the tape's width, so the FX loop stays per-track.
- A mono input into a stereo-recorded loop (with monitoring on) is heard on
  both channels.

## Tips

- Record 5–20 seconds of something tonal, set **decay** around 10 o'clock,
  and walk away. Come back in twenty minutes.
- **age → decay cv** (through an attenuator) makes the disintegration
  accelerate as it progresses, which is exactly what real tape does.
- **eoc → splice trig** through a probability gate: the loop occasionally
  resurrects.
- **overlap** up full turns even a short, hard-edged loop into a smooth
  ambient wash; automate it from an LFO into **overlap cv** to breathe.
- **send → a reverb → return**, the **send** knob around 9 o'clock: the loop
  slowly dissolves into its own reverb tail as it ages. Push the **send** knob
  and the reverb feeds itself into a self-sustaining drone (the tape saturates
  softly above nominal level, so the loop settles instead of clipping).
- **ramp → a filter cutoff** locks a sweep to the loop; **ramp** into a
  wavetable index scans the table once per pass.
- The loop's contents are not saved with the patch; the tape is blank on
  load. (The right-click menu also has "Clear loop".)

## Notes

The loop buffer lives at the engine sample rate; changing the sample rate
clears the tape. Per-track buffers grow the first time you record that many
channels and are not released afterward, so a session that has recorded a
16-channel loop holds those buffers until the module is removed.
