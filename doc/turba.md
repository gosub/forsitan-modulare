# turba

![turba](../img/turba.png)

**Eight oscillators, eight feedback delays, wired into a ring that
cross-modulates itself. One knob decides whether it drones or shatters.**

*turba* is Latin for uproar, tumult, commotion; a disorderly crowd. The module
takes its architecture and its interface from **Skrewell**, John Nowak's
chaotic sound generator in the REAKTOR factory library — "an intuitive and
visual sound design workstation whose soundscapes can range from meditative
atmospheres to crackling harshness", in the manual's words.

It is **not a port**, and not for want of trying by other people: Skrewell's
chaos lives inside REAKTOR's built-in filters, which cannot be opened, and
every attempt to rebuild it in Max/gen~ or Pd has run aground on exactly that.
The verdict on the Cycling '74 thread is worth quoting, because it is the
reason this module is inspired-by rather than a clone: "being a sound
generator tool based on feedback, any minor difference in any of the modules
makes the result very different (butterflies, hurricanes, and stuff)". What is
taken here is the structure the factory library manual describes, and the
interface, both reimplemented from scratch. Same precedent as
[bulla](bulla.md).

## The engine

Eight parallel channels, mixed into one stereo signal. Each channel:

```
              ┌──────────── FM from the channel to its right ───┐
              │                                                 │
              v                                                 │
  pulse osc ──> x AM from the channel to its left ──> (+) ──> filter ──┐
                                                       ^              │
                                                       │              v
                                            x feedback │        normalizer
                                                       │              │
                                                       └── delay <────┤
                                                                      │
                                                       y ─────────────┴──> mix
```

The oscillators never stop and there is no gate and no pitch input. Like the
original, you switch it on and it runs.

What makes it more than eight parallel drones is the **ring**: `y`, each
channel's loop signal, is what frequency-modulates the oscillator one seat to
the left and amplitude-modulates the one to the right. All eight loops are
therefore one system, and with the filter saturating inside that system it is
a genuinely chaotic one — see *Is it actually chaotic* below.

### Three topologies

The **mode** switch chooses where the filter sits, after Skrewell's three
operation modes:

| mode | | |
|------|---|---|
| **loop** | pulse oscillator, filter **inside** the feedback loop | the harshest of the three: everything that recirculates is filtered and saturated again on every pass |
| **pre** | pulse oscillator, filter **in front of** the delay | the filter shapes what enters the loop once and then leaves it alone; more tonal, more delay-like |
| **bare** | **parabolic** oscillator, **no filter** | the calm one. A parabolic wave is much rounder than a pulse and with no filter in the way the loops behave like plain combs. Measured centroid ~276 Hz against ~574 Hz for **loop** |

The cutoff macro and the resonance bars do nothing in **bare**, which has no
filter to point them at.

## The edit area

The eight bars are the eight channels. Which parameter they show is the
**function** knob; there are eight functions, so the bank is 64 values in all.
They are real module parameters: they save with the patch, they randomize with
the module, and they are visible over [limen](limen.md) as
`Channel <n> <function>`.

| function | range | |
|----------|-------|---|
| **pitch** | 8 Hz – 16 kHz, exponential | the oscillator |
| **cutoff** | 20 Hz – 20 kHz, exponential | the filter (**loop** and **pre** only) |
| **reso** | Q 0.6 – 18.6 | mapped by **flow**, and mapped *backwards*; see below |
| **time** | 0.15 ms – 307 ms, exponential | the delay. At the short end the loop is a comb rather than an echo |
| **fbk** | 0 – 102% | loop gain. Over unity the normalizer holds it |
| **fm** | 0 – 4 octaves | how hard the neighbour's loop signal drives this oscillator |
| **am** | 0 – 100% | how hard the other neighbour amplitude-modulates it |
| **level** | 0 – 100% | this channel's contribution to the mix. Channels are panned across the field in order, channel 1 hard left |

The **edit** switch is Skrewell's three mouse behaviours:

- **draw** — drag a bar to set it. Click and sweep across to draw a shape.
- **wrap** — drag anywhere and all eight bars move together, keeping their
  shape. A bar that runs off an end **mirrors** back rather than piling up
  against it, so a long drag folds the whole bank through the range.
- **rand** — drag and all eight bars jog randomly, by as much as you moved.

Double-click the edit area to put the current function back to its default
shape. One drag is one undo step.

**rand** (the button, and its trigger input) randomizes the whole bank. Turn
the level down first: the original's manual warns about "unexpected bursts of
noise" and that warning transfers.

## The macros

The four big knobs are the reason the interface works at all, and they are not
what a knob in a modular usually is. **They do not offset the bars, they map
them.** Each applies a power curve `v^γ` to all eight of its bars at once,
with `γ = 5^-knob`:

- **centre** — `γ = 1`, the mapping is the identity and the bars mean exactly
  what they show.
- **hard left** — `γ = 5`. A bar at 50% maps to 3%. Only bars already near the
  top survive; everything else is crushed to the bottom of the range.
- **hard right** — `γ = 1/5`. A bar at 50% maps to 87%. The whole bank is
  lifted and the differences between bars compress.

So one knob asks "how much of this parameter does the bank get", and it asks
it of eight channels at once while preserving their order. Sweeping **pitch**
does not transpose the bank rigidly, it opens and closes the *spread* of it.

| knob | what it maps | measured |
|------|--------------|----------|
| **pitch** | all eight oscillator frequencies | centroid 71 Hz hard left → 1385 Hz hard right |
| **cutoff** | all eight filter cutoffs | centroid 83 Hz → 1862 Hz, and output RMS 0.05 → 1.06: hard left the filters close and there is almost nothing left |
| **delay** | all eight delay times | 0.17 ms to 134 ms across the knob at the default bank. Level barely moves; what moves is where the comb sits, which is why the left end sounds like ring modulation and the right end like an echo |
| **flow** | the FM and AM amounts, the resonances, and the inertia | see below |

Each has an attenuverter and a CV input; CV is ±5 V for the full sweep, scaled
by the attenuverter.

### flow

**flow** is the one to reach for. It is Skrewell's knob of the same name, which
the REAKTOR manual describes as adjusting "various amounts of modulation… turn
to the left for less modulation and more inertia, turn to the right for the
opposite". Here it does three things at once:

1. maps the **fm** and **am** bars, the same way the other macros map theirs;
2. maps the **reso** bars, **inverted** — right takes resonance *down*;
3. sets the engine's inertia, the glide on every internal control, from 1 s at
   hard left to 2.5 ms at hard right.

The inversion in (2) is the interesting one, and it is not an affectation. The
Max porter's finding was that what tips Skrewell into chaos is "the resonance
parameter in a 2-pole filter being turned **down**", which he called "a pretty
surprising behavior" and never explained. It is not that surprising in a
feedback loop: a high-Q lowpass hands the loop gain in one narrow band and it
rings there, orderly; open the Q out and the loop gets broadband gain, the
saturator inside it has much more to fold, and the trajectory stops closing on
itself. Reproducing that here reproduces the musical gesture — flow right is
where it shatters.

The cost is about 6 dB: the bank measures RMS 1.19 at flow hard left and 0.61
hard right, because a damped wide filter passes less and chaos spreads what is
left across the spectrum. That is the trade, and the **level** knob is next
door.

### Is it actually chaotic

Yes, and `test/turba_probe lyapunov` measures it. Two copies of the engine are
started from identical state, one nudged by 1e-6 on a single oscillator's
phase, and the growth rate of the difference is a largest-Lyapunov estimate:

| flow | λ (1/s) | |
|------|---------|---|
| −1.00 | 0 | periodic; the two runs never separate |
| −0.50 | 0 | periodic |
| ±0.00 | 645 | chaotic |
| +0.50 | 670 | chaotic |
| +1.00 | 670 | chaotic |

There is a real bifurcation somewhere between flow −0.5 and 0: below it the
bank is a drone that repeats, above it two runs of the same patch are two
different pieces of music.

## Inputs and outputs

| jack | |
|------|---|
| **in** | audio, injected into **all eight loops** at once. The original has no input at all; this is the addition that makes turba usable as a processor. A signal in here is filtered, delayed, saturated and cross-modulated eight ways, and it also becomes part of what the ring modulates itself with |
| **rand** | trigger, randomizes the whole bank. Same as the button |
| **L**, **R** | the mix. Channels are equal-power panned across the field in index order, so left and right are genuinely different mixes and the Lissajous display has something to draw |
| **cv** | the bank's own slow wander, ±5 V. The eight channels summed with alternating sign and lowpassed at 25 Hz, so common motion cancels and what is left is how unevenly the eight are behaving. Typically ~1 V RMS |

## The displays

The **edit area** is the eight bars. The square to its right is a **Lissajous**
of L against R, as on the original panel — it is the fastest way to see what
the bank is doing. A single loop means the two channels are correlated and the
bank is behaving; a filled square means it has gone to noise; a slowly
precessing figure is the interesting middle.

## Context menu

| item | |
|------|---|
| **Ring coupling** | off makes each channel modulate *itself* instead of its neighbours. Eight independent chaotic loops rather than one coupled system: much tamer, and useful as a bank of eight droning comb resonators |
| **Randomize all functions** | off makes the rand button and trigger randomize only the function currently on screen, which is far more controllable than rolling all 64 |
| **Display scale** | 1× to 8× on the Lissajous, the original's "Display Control". 1× is ±5 V filling the box; turn it up when the bank is running quietly |
| **Randomize channels** | the button, from the menu |
| **Reset channels to default** | all 64 bars back to the starting bank |

## Tips

- The default patch is deliberately mild. Start by pushing **flow** right and
  watching the Lissajous.
- For a drone: **flow** hard left (which also gives you the 1 s inertia, so
  everything you touch glides), **fbk** high, **time** long.
- For percussion and crackle: **delay** hard left so the loops are combs,
  **flow** right, and modulate **cutoff** from an envelope.
- The **level** function is the one to shape by hand. Muting five of the eight
  channels turns a wall into a trio, and the three that are left are still
  being modulated by the five you cannot hear.
- **rand** with "Randomize all functions" off, sitting on the **pitch**
  function, is a fast way to re-roll a chord without losing the patch.
- turba is a fine source for [lustro](lustro.md) or [quadrare](quadrare.md),
  and the **cv** out drives anything that wants a lazy unpredictable voltage.

## Differences from Skrewell

Beyond the obvious one — this is a different implementation of a described
architecture, not a translation of a patch — two changes were made on purpose,
and both were forced by measurement.

**The normalizer only turns a loop down.** The manual says each channel has a
normalizer in its delay line, and a normalizer, properly, holds a signal at a
fixed level in both directions. Built that way it flattens the bank's
*dynamics*: with every channel pushed back up to the same level, the macro
knobs stop changing how loud anything is. Measured across the four macros,
knob hard left to hard right, with a true normalizer against the limiter that
shipped:

| macro | RMS span, normalizer | RMS span, limiter |
|-------|----------------------|-------------------|
| pitch | 1.47 → 1.56 | 1.20 → 0.56 |
| cutoff | 0.50 → 1.45 | 0.05 → 1.06 |
| delay | 1.66 → 1.55 | 1.15 → 0.93 |
| flow | 1.58 → 1.43 | 1.19 → 0.60 |

The timbral effect survives either way — the centroid spans are comparable,
and on cutoff the normalizer's is actually wider — so this is not a claim that
a normalizer makes the macros inaudible. It is narrower than that: the level
differences the bars ask for do not survive it, the per-channel crest factor
drops from 3.2 to 2.7, and a bank where nothing can be quiet is a bank with
one dynamic. So it is a limiter here — it holds a loop that is running away
and leaves a quiet one quiet. What keeps this bank alive with no gate is not
the normalizer, it is that the oscillators never stop.

**Flow maps resonance backwards**, as described above. Skrewell's own
behaviour, but arrived at deliberately here rather than as a side effect of a
closed filter.

Smaller ones: the filter is a topology-preserving 2-pole SVF with its
integrator states soft-limited, not a model of whatever REAKTOR uses; the
oscillators are polyBLEP pulses rather than REAKTOR's; the panning across the
eight channels is fixed rather than a parameter; and the input, the four CV
inputs, the CV output and the rand trigger have no counterpart in the
original.

## Making it wander

turba has two quite different kinds of motion in it and they are worth
separating, because only one of them is automatic.

The **fast** one is the chaos: a largest-Lyapunov estimate of 670/s means the
waveform decorrelates in a millisecond or two. That is what makes it restless
rather than a static tone. But fast chaos mixes fast, and a fast-mixing system
has *stationary statistics* — it can be violent and still not go anywhere.

The **slow** one is the bank moving through its own range, and that is a
property of where you put the bars, not something the engine does by itself.
The measure is the spread of the spectral centroid over a long untouched run,
in octaves (`test/turba_probe wander`). Three things control it, in order of
how much they matter:

| | |
|---|---|
| **long delays** | the biggest one by far. Below about 10 ms a loop is a comb and settles in a few passes; up at 100–300 ms it takes a tenth of a second per pass and its state survives long enough to evolve |
| **feedback near unity** | at 0.5 every loop is safely damped and nothing ever builds. Push the **fbk** bars to 0.9–1.0 and loops build, saturate against the limiter and collapse, which is where the lurching comes from |
| **pitches close together** | eight channels spread over three octaves beat against each other at audio rate, which is timbre. Eight inside a fifth beat *slowly*, and the cross-modulation turns those slow beats into slow movement |

The default bank is set that way — delays 30–307 ms, every loop between 0.88
and 1.0, pitches within a fifth — which is worth knowing if you wonder why it
sounds nothing like eight independent oscillators. An earlier default with
half the feedback and 2–40 ms delays measured 0.04 octaves of wander; this one
measures 0.19–0.24 in all three topologies.

Two honest caveats. **flow hard left kills it** (0.01 octaves): that end is
the periodic, ordered one, and the 1 s inertia there means nothing moves
quickly either. And turba still does not reach the reference recordings of
Skrewell, which measure 0.75 and 3.74 octaves — though every one of those is a
*performance*, with a hand on the controls, so it is not a like-for-like
comparison. Driven equivalently, with flow swept over twenty seconds, turba's
short-time loudness moves 5.8 dB against their 1.5–5.7.

If you want more movement than the bank gives you, patch something slow into a
macro. The **cv** output into one of the macro inputs is the cheapest, and the
attenuverter sets how far it goes.

### What was tried and did not work

Recorded so nobody spends the afternoon again:

- **A second, slow ring**: an envelope follower per channel, cross-coupling
  the loop gains, at both signs, three lag settings and gains up to 3. All
  null, 0.15–0.32 dB. Eight oscillators at constant amplitude sum to constant
  power, and modulating loop *gain* barely moves a channel's level, so the
  slow loop has almost no gain to go unstable with.
- **Cross-FM tapped after the neighbour's delay line** rather than from its
  current output, so the modulation arrives 0.15–307 ms late. Plausible, one
  line to implement, and null: over fourteen random banks, paired against the
  same banks with the instantaneous tap, it won five times and the means were
  0.213 against 0.221 octaves. Delaying a stationary audio-rate modulator
  changes its phase, not its statistics, so the FM sidebands come out the same.
- **The normalizer, at every speed and depth.** It is not what holds the
  levels still; see the note above.

## Cost

1.15% of one core at 48 kHz, all eight channels always running. There is no
oversampling: the loops are saturating feedback paths where aliasing folds
back into the signal and becomes part of the chaos, and oversampling eight of
them would cost more than the module is worth.

## Attribution

Skrewell is by **John Nowak**, shipped in the REAKTOR factory library by
Native Instruments. No code from it was used or could have been — it is a
closed patch built on closed primitives. The architecture came from the
[REAKTOR Factory Library manual](https://www.native-instruments.com/fileadmin/ni_media/downloads/manuals/REAKTOR_Factory_Library_Manual_English_2015_11.pdf)
(section 7.2, pp. 262–265), and the account of where its chaos comes from from
the Cycling '74 thread
[Problem rebuilding Reaktors Skrewell in Max/Gen](https://cycling74.com/forums/problem-rebuilding-the-reaktors-skrewell-in-maxgen),
where zangpa, Matteo Marson and AlbertoZ compare notes on why it will not
port.
