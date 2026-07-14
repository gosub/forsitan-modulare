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
- Stopping is clickless even when the end of the recording doesn't line up
  with the beginning: for the first few milliseconds after the stop, the
  loop's head is crossfaded with the live input, so the seam carries real
  audio instead of a jump. Let the source keep playing for a moment after
  you stop recording and the seam will be seamless in the literal sense.
- While recording, the input is monitored at the output; when recording
  stops, monitoring stops with it and you hear only the loop. The
  right-click "Monitor input" menu offers **While recording** (default),
  **Always** (input passes through during playback too), and **Never**.

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
clears the tape.
