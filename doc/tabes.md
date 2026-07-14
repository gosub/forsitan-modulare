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
pass zero.

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
| **wow** | wow/flutter depth on the play head (grows slightly with age) |
| **rec** / **gate** | record toggle / gate |
| **splice** / **trig** | restore the pristine recording, age back to zero |

## Outputs

| jack | function |
|------|----------|
| **out** | the loop (plus the live input, per the "Monitor input" menu mode) |
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
- **age** and **eoc** are always monophonic (they describe the transport,
  not the audio). **decay cv**, **gate** and **splice trig** read channel 1.
- A mono input into a stereo-recorded loop (with monitoring on) is heard on
  both channels.

## Tips

- Record 5–20 seconds of something tonal, set **decay** around 10 o'clock,
  and walk away. Come back in twenty minutes.
- **age → decay cv** (through an attenuator) makes the disintegration
  accelerate as it progresses, which is exactly what real tape does.
- **eoc → splice trig** through a probability gate: the loop occasionally
  resurrects.
- The loop's contents are not saved with the patch; the tape is blank on
  load. (The right-click menu also has "Clear loop".)

## Notes

The loop buffer lives at the engine sample rate; changing the sample rate
clears the tape. Per-track buffers grow the first time you record that many
channels and are not released afterward, so a session that has recorded a
16-channel loop holds those buffers until the module is removed.
