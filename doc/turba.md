# turba

![turba](../img/turba.png)

**Eight oscillators, eight feedback delays, wired into a ring that
cross-modulates itself. One knob decides whether it drones or shatters.**

*turba* is Latin for uproar, tumult, commotion; a disorderly crowd. The module
takes its architecture and its interface from **Skrewell**, the chaotic sound
generator in the REAKTOR factory library — "an intuitive and
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

Eight channels, **sixteen loops**: the ensemble was taken apart for this (see
*Attribution*) and every tone generator in it holds exactly two `LEVER`
macros with a `crossvoice` between them, where a LEVER is not an oscillator
but a whole channel — oscillator, filter, resonance, normalizer, delay and
feedback. So a channel here is a pair of those, driven by the same bar, the
second offset a tritone up with a shorter loop so the pair is two loops and
not one played twice. Each lever:

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

What makes it more than sixteen parallel drones is the coupling, which comes
in two kinds, as it does in the ensemble. Inside a pair the two levers
modulate each other — that is what `crossvoice` is there for — and on top of
that each lever is FM'd by the corresponding lever of the next channel along
and AM'd by the previous one, a ring through the whole bank. Every loop is
therefore part of one system, and with the filter saturating inside it that
system is genuinely chaotic — see *Is it actually chaotic* below.

Lever pairs can be switched off in the context menu, which halves the CPU and
is not just an economy: see the note there, it is the setting that evolves
most.

### Two-state switching

The thing that makes the bank evolve with nobody touching it, and the last
piece to go in. From colB's reverse engineering of the ensemble: *"There are
some parameters that have two settings that get switched between… It does
that thing where components of the sound toggle chaotically between two
states, and when lots of things are doing that you get loads of layers that
still make sense."*

Once per pass of its own delay line — so every 30 to 300 ms, at eight
different rates — each channel latches one bit from the sign of another
channel's loop signal, and that bit picks between two values of its filter
cutoff. Nothing drifts and there is no LFO: the sound *flips*, and eight
channels flipping out of step with each other is what keeps it moving.

It has to be the **cutoff** that switches. Measured over an untouched minute,
switching the cutoff takes the spectral wander from 0.19 to 0.97 octaves;
switching the pitch or the delay time instead makes it *worse*, 0.10 to 0.14,
because those move the sound without moving where its energy sits. In the
bare topology, which has no filter, the switch shortens the delay instead.

Depth is in the context menu, off / light / normal / wild, defaulting to
normal. The switch clock never runs faster than 10 ms however short the
delays get: past that it stops being a change of state and becomes an
audio-rate modulator.

### Three topologies

The **mode** switch chooses where the filter sits, after Skrewell's three
operation modes. colB's reading of the ensemble matches the manual here:
three pairs of oscillators, *"two pairs use pulse waves and the other uses
sinusoid (par FM)"*, only one pair live at a time.

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
| **Lever pairs** | both loops of every channel, on by default because it is what the ensemble does. Off, each channel is a single lever: half the CPU, a thinner and more separated bank, and — this is the awkward part — **more** self-evolution, 0.82 octaves of spectral wander against 0.34. Two chaotic loops summed into one voice average each other out, and no amount of coupling weight recovers it (measured at six settings from 0 to 0.85). On is denser, rougher and more faithful; off moves more. There is no setting that is both |
| **Raw oscillators (aliasing)** | drops the band-limiting from the pulses so every edge folds its harmonics back down the spectrum. colB puts part of Skrewell's character down to it being "digital with aliasing and quantization". Measured, it is a **small** effect here and honesty demands saying so: at the default bank the centroid moves 1052 → 1092 Hz and the spectral flatness 0.011 → 0.013. It shows up properly only with the pitch macro up, where the flatness goes 0.041 → 0.051. The reason is that most of this engine's aliasing never came from the waveform edges in the first place — exponential FM at audio rate throws sidebands past Nyquist whatever shape the oscillator is, and polyBLEP was never correcting those. It also costs nothing; raw is cheaper than band-limited |
| **Bit crush** | 12, 10 or 8 bits, quantizing each loop signal on its way into the delay. The other half of "aliasing and quantization", and similarly small on its own: 8 bit moves the centroid 1052 → 1123 Hz and doubles the energy above 5 kHz |
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
| **two-state switching** | the biggest one, and the only one that is not a property of the patch. See above; off it is 0.19 octaves, wild it is 1.02 |
| **long delays** | below about 10 ms a loop is a comb and settles in a few passes; up at 100–300 ms it takes a tenth of a second per pass and its state survives long enough to evolve |
| **feedback near unity** | at 0.5 every loop is safely damped and nothing ever builds. Push the **fbk** bars to 0.9–1.0 and loops build, saturate against the limiter and collapse, which is where the lurching comes from |
| **pitches close together** | eight channels spread over three octaves beat against each other at audio rate, which is timbre. Eight inside a fifth beat *slowly*, and the cross-modulation turns those slow beats into slow movement |

The default bank is set that way — delays 30–307 ms, every loop between 0.88
and 1.0, pitches within a fifth — which is worth knowing if you wonder why it
sounds nothing like eight independent oscillators. An earlier default with
half the feedback and 2–40 ms delays, and no switching, measured 0.04 octaves
of wander; with both it measures **0.82 in the loop topology**, against 0.75
for a reference recording of Skrewell standing still.

Two honest caveats. **flow at either extreme kills it** — 0.29 octaves hard
left, 0.21 hard right, against 0.82 at noon. Left is the ordered, periodic
end with a 1 s inertia on everything; right takes the resonance down until
the filters stop being able to say where the energy is, so switching them has
less to switch. The middle is where it lives. And the second reference
recording measures 3.74 octaves, which turba does not come near — though every
reference is a *performance*, with a hand on the controls, so it is an upper
bound rather than a target.

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
- **Recovering the wander that lever pairs cost**, by reweighting the
  crossvoice against the ring. Swept from 0 (levers ignore their partner) to
  0.85 (they barely hear anything else): 0.48 down to 0.30 octaves, against
  0.82 with a single lever. It is the summing that does it, not the coupling.
- **Switching the pitch or the delay time** instead of the cutoff, once the
  two-state mechanism was in. Both made the wander worse than no switching at
  all.

## Cost

2.8% of one core at 48 kHz with lever pairs on, 1.4% with them off, all eight channels always running. There is no
oversampling: the loops are saturating feedback paths where aliasing folds
back into the signal and becomes part of the chaos, and oversampling eight of
them would cost more than the module is worth.

## Attribution

Skrewell ships in the REAKTOR factory library by Native Instruments, which
does not credit it. Its authorship is reported inconsistently: on the
Cycling '74 thread below it is attributed to **John Nowak**, while the NI
community thread discusses it as the work of **Lazyfish** (Alexander
Potekhin), whose **TG-8H** is described as its prototype and is built in
Reaktor Core rather than closed primitives. No code from it was used or could
have been. The architecture came from the
[REAKTOR Factory Library manual](https://www.native-instruments.com/fileadmin/ni_media/downloads/manuals/REAKTOR_Factory_Library_Manual_English_2015_11.pdf)
(section 7.2, pp. 262–265), and the account of where its chaos comes from from
the Cycling '74 thread
[Problem rebuilding Reaktors Skrewell in Max/Gen](https://cycling74.com/forums/problem-rebuilding-the-reaktors-skrewell-in-maxgen),
where zangpa, Matteo Marson and AlbertoZ compare notes on why it will not
port. The two-state switching, the oscillator pairs and the count of filters
and delays come from **colB**'s reverse engineering of the ensemble in
[SKREWELL - Hardware synth equivalent ideas? What is the structure of
Skrewell?](https://community.native-instruments.com/discussion/14722/skrewell-hardware-synth-equivalent-ideas-what-is-the-structure-of-skrewell),
which is the only public account of the structure written by somebody who
actually opened it.

Beyond that, two ensembles were read directly: **TG-8H** (lAZyfISh, 2003–2013,
whose own header calls it "an early Skrewell prototype") and a
multi-output modification of Skrewell itself. Reaktor's `.ens` is a chunked
binary, but its strings are stored length-prefixed and in structural order,
which is enough to recover the module tree without parsing the object graph.
That is where the two-lever architecture, the `crossvoice` between them, the
per-tone-generator filter differences (`CUT`/`TYP` in one, `HPF`/`LPF` in the
next, none at all in the third) and the `cc`/`min`/`max` shape of the macro
mappings all come from. The tooling for it is not in this repository; it was
forty lines of Python and is described here so it can be redone.

**What that does not include is the wiring.** The records are bracketed —
`[`, a length-prefixed class name, a payload, `]` — and a bracket walk over
the Skrewell file yields 3531 of them: 1077 `KSModul`, 1345 `KInPort`, 1108
`KOutPort`, one `KEnsemble`. That is the object *tree*, and containment and
order come out of it reliably. The connections do not. A port record's payload
is 25 bytes and holds no reference to another port: the `-1`s in it are the
same in every port, and the trailing values that look like identifiers cross
match between inputs and outputs 18 times out of 1345, which is chance. The
graph is somewhere in the 3.1 MB of unparsed blobs hanging off four
`KSModul` records — most of that will be the snapshot banks and the panel
bitmaps, but the connection table is in there too.

So: the parts list and the grouping are read from the ensemble and can be
relied on. **The signal flow inside a lever is not.** What modulates what, in
what order, with what scaling, is still this module's own design, informed by
the factory manual's description and by the forum accounts. Anyone wanting to
go further should start by working out the layout of those blobs.
