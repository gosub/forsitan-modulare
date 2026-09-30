# spira - design doc

Status: built 2026-09-30, audition pending. The module follows this doc;
`doc/spira.md` is the manual, `src/spira/spira.hpp` the engine, and
`test/spira_probe` measures what is claimed here.

## The idea

The user's picture: a line of audio, and small circles attached to it. A
circle is a section of the line that loops. If the loops change as they go
round (volume, pitch, duration, effects), the circle becomes a spiral.

The risk the user named: it would sound like a stutter effect. A stutter is
exactly a circle with nothing changing, cut hard at the seam. The design
answers that on four fronts, each an ordinary technique from granular
synthesis applied to loops rather than to grains:

1. **A lap is a grain, not a cut.** Every lap has an envelope (SHAPE, from
   plucked through flat to swelling) and a crossfade into the next (SOFT,
   from 1 ms to half the lap). With SOFT full a circle is overlapping
   grains that happen to read the same place.
2. **The lap moves.** Direction (forward, ping-pong, reverse), JITTER on
   each lap's place and length, TONE per turn.
3. **The line goes on.** A stutter stops the source. Here the line keeps
   playing and up to eight circles overlap, each at its own stage.
4. **The spiral.** SPIRAL, TAPE and FADE, below.

## The geometry

One rule: **SPIRAL sets the next lap's length in time** (ratio r). TAPE
only says how that length is reached:

    T' = r T                  the lap, in output samples
    v' = v r^(-tape)          the speed, source samples per output sample
    len' = T' v' = len r^(1 - tape)   the window, in source samples

TAPE 1 keeps the window and changes the speed (tape); TAPE 0 keeps the speed
and changes the window. Speed is clamped to [1/16, 4]; past the clamp the
window changes instead, so a converging spiral still converges.

For r < 1 the laps form a geometric series: the circle lasts
T0 (1 - r^N) / (1 - r), less than T0 / (1 - r), and ends when a lap reaches
1 ms. The probe measures it within 1%. For r > 1 laps grow to 16 s.

**FADE and TONE move by the lap's share of the first lap** (T / T0), not per
lap. Per lap, a converging spiral would pass through its last hundred laps
in a second and fade to nothing before its repetition became a pitch,
which is the sound the whole module is for. Per share of T0, the total fade
over infinitely many laps is FADE x 1 / (1 - r): finite. So the converging
spiral arrives, and is heard arriving.

## Lap mechanics

A circle holds two laps: the current one and the tail of the last, which
runs on past its window for the crossfade. The seam is equal power (the two
laps read different places) with a smoothstep inside the sine. Sine and
cosine alone end on a slope, and a slope that stops under a loud signal
measured 0.023 on the #22 click figure (second difference over peak; a clean
220 Hz sine is 0.0008). The smoothstep brings the hard seam to 0.003.

The window is placed where the buffer will still hold it for the whole lap,
and moved back if it would read past the write head. A circle whose window
has been overwritten (48 s of live line) ends, fading. Under HOLD nothing is
overwritten.

TONE is a one-pole low-pass and high-pass per circle; their coefficients
glide over 3 ms, since a step at every lap measured 0.0097.

## Levels

Circles sum at unity with the line, and the whole output is linear to 6 V,
then bends toward 10 V (a tanh with a matching slope). MIX gives both
the line and the circles unity in the middle, so the module works as an
insert. With many loud overlapping circles the limiter works; the audition
asks whether that is right.

## The ring

Eight red-green-blue lights on an arc over SPIRAL, one per circle, taken in
turn at birth, so the light a ninth circle takes is always the oldest's, the
one it replaces. Brightness is the circle's level from the engine (the larger
of its two laps' envelopes, times the steal fade), mapped over 48 dB rather
than linearly, where a circle 20 dB down would look off. Colour is the
circle's spiral along the knob's own travel (the square root of |log2 r|),
yellow to orange-red inward (heating) and to blue outward (cooling). The
BIRTH button's light flashes on births only: it followed TURN at first, and
the laps of the newest circle buried the press.

## Deliberately left out of v1

- A display of the line itself.
- A send/return per circle (effects accumulating turn by turn, as in tabes).
  The V/OCT and TURN outputs are the hooks for outside effects instead.
- Clock-synced SIZE. The converging spiral's total length is known
  (T0 / (1 - r)), so it could be solved to land on a bar; not yet.
- Anti-aliasing at high speeds: speed is capped at x4.
