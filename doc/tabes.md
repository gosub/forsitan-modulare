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
| **overlap** (+ **cv**) | crossfades the loop point. At zero it is inaudible (the recording seam already declicks the wrap). Opened up, a second play head half a loop ahead is mixed in under a constant-power window that is silent at each seam, so the loop's second half plays over its first — a seamless ambient blur. The tape still ages one copy underneath, so it keeps rotting while it smears |
| **mix** (+ **cv**) | how much of the **return** signal is re-recorded onto the tape (see FX loop below) |
| **rec** / **gate** | record toggle / gate |
| **splice** / **trig** | restore the pristine recording, age back to zero |

## FX loop (send / return)

**send** carries the loop read out to an external effect; patch its output
back into **return** and the **mix** knob folds that processed signal onto the
tape as it re-records. Because the write head bakes it in every pass, whatever
the effect does **compounds**: a reverb blooms into a cloud, a pitch shifter
spirals, a filter recolours a little more each time round. At **mix** = 0 the
return is ignored; at higher settings the tape becomes what your pedal makes
of it. The feedback carries the usual one-sample delay, and the tape is
bounded, so a hot effect saturates into a drone rather than exploding.

The loop only acts when **both** ends are patched — **send** out and
**return** back. With only **return** connected nothing is folded in (the
**mix** knob does nothing), so a stray cable can't quietly overwrite the tape.

## Outputs

| jack | function |
|------|----------|
| **out** | the loop (plus the live input, per the "Monitor input" menu mode) |
| **send** | the loop read, out to an external effect (returns via **return**) |
| **ramp** | the play head as a 0–10V saw, one ramp per loop (resets at the loop point, in step with **eoc**). A loop-locked phasor for sweeps, wavetable scans, or syncing other modules; it is clean of wow so it stays a stable clock |
| **age** | 0.1V per completed pass, clamps at 10V. Patch it somewhere: let the patch itself react to the tape dying |
| **eoc** | 1 ms trigger every time the loop wraps (its LED flashes with each wrap) |

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
  transport, not the audio). The CV inputs (**decay/wow/overlap/mix cv**),
  **gate** and **splice trig** read channel 1. **send** and **return** are
  polyphonic, matching the tape's width, so the FX loop stays per-track.
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
- **send → a reverb → return**, **mix** around 9 o'clock: the loop slowly
  dissolves into its own reverb tail as it ages. Push **mix** and the reverb
  feeds itself into a self-sustaining drone (the tape clamp keeps it in
  check).
- **ramp → a filter cutoff** locks a sweep to the loop; **ramp** into a
  wavetable index scans the table once per pass.
- The loop's contents are not saved with the patch; the tape is blank on
  load. (The right-click menu also has "Clear loop".)

## Notes

The loop buffer lives at the engine sample rate; changing the sample rate
clears the tape. Per-track buffers grow the first time you record that many
channels and are not released afterward, so a session that has recorded a
16-channel loop holds those buffers until the module is removed.
