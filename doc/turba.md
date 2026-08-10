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
   pulse osc ──> x AM ──> [filter, in pre] ──> (+) ──> [filter, in loop]
                                                ^                │
                                     x feedback │                v
                                                │              delay
                                                │                │
                                                └── normalizer <─┘
                                                        │
                                                        └──> y, to the mix
                                                             and to the ring
```

Read off the ensemble rather than guessed, and the order matters. The **delay
comes before the normalizer**, and the normalizer's output is both what the
lever puts out and what feeds back. So the oscillator is never heard directly:
everything reaches the output through the delay line, however short it is set.
The three topologies differ only in where the filter sits, which is exactly
what the factory manual says and now also what the patch says.

The oscillators never stop and there is no gate and no pitch input. Like the
original, you switch it on and it runs.

What makes it more than sixteen parallel drones is the coupling, and its shape
is read off the ensemble. The `crossvoice` macro beside each lever holds
**eight From Voice modules wired to the channel inputs of a selector**, and
the `fm` and `am` bars drive that selector's position. So a bar does not say
*how hard* a lever is modulated — it says **which channel modulates it**,
blending between two adjacent channels when it sits between them. The source
is the partner lever of the chosen channel: crossed within the pair, selected
across the bank.

The two bars therefore draw a **coupling topology**, sixteen values deciding
who listens to whom, and that is the instrument. Depth is one global amount
on the **flow** knob. Every loop ends up part of one system, and with the
filter saturating inside it that system is genuinely chaotic — see *Is it
actually chaotic* below.

Both levers always run, as they do in the ensemble.

### Through-zero FM

Worth its own note, because it is the single biggest thing separating this
module from a polite one, and it was got wrong at first.

Skrewell's oscillators are the FM variants of Reaktor's primary set, whose
extra input the manual describes as *"F — Linear frequency control, which is
added to the frequency of the P input"*. In every LEVER the `P` input is wired
to a constant **−300**: a MIDI pitch so low the oscillator would sit at a
fraction of a hertz. So the pitch input is dead and the entire frequency
arrives through `F`, in hertz — and a linear frequency input can go
**negative**, running the oscillator backwards through zero.

Here that is `freq = base × (1 + fm × mod)`. Exponential FM, which this module
used until the ensemble was read, cannot cross zero: it is smooth, it sounds
like vibrato at low index and like a siren at high index, and it is a much
politer thing. Switching to the linear form nearly tripled the energy above
2 kHz at the default bank, 0.137 → 0.381 of the total, and moved the centroid
from 1052 Hz to 1607 Hz.

The port order that makes the wiring consistent is **P, F, A, W**, which also
matches AlbertoZ's account on the Cycling '74 thread from the other side:
"the width W inlet is not used… and the P inlet is fixed to -300".

### Three topologies

The **mode** switch chooses where the filter sits, after Skrewell's three
operation modes. colB's reading of the ensemble matches the manual here:
three pairs of oscillators, *"two pairs use pulse waves and the other uses
sinusoid (par FM)"*, only one pair live at a time.

| mode | | |
|------|---|---|
| **loop** | pulse oscillator, filter **inside** the loop, between the summer and the delay | the harshest of the three: everything that recirculates is filtered and saturated again on every pass. This is the ensemble's bandpass generator, the one with separate HP and LP |
| **pre** | pulse oscillator, filter **between the oscillator and the summer** | the filter colours what enters the loop once and then leaves it alone; more tonal, more delay-like. The ensemble's multimode generator, the one with cutoff and type |
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
| **type** | low → band → high | the filter's *shape*. The ensemble's `lbh`: the `Pos` of a selector over the Multi 2-Pole's three outputs, blending between adjacent ones, so each channel can sit on a different slope |
| **time** | 0.15 ms – 307 ms, exponential | the delay. At the short end the loop is a comb rather than an echo |
| **fbk** | 0 – 102% | loop gain. Over unity the normalizer holds it |
| **fm** | channel 1 – 8 | **which channel frequency-modulates this one.** Not a depth: it is the position of an eight-way selector over the bank, blending between two adjacent channels when set between them. Depth comes from **flow** |
| **am** | channel 1 – 8 | the same, for amplitude modulation. The two bars together draw the bank's coupling: which channel listens to which |
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

**flow is not a bar mapping** like the other three. In the ensemble it is the
`Pos` of five selectors, each blending between **two knobs**, so it crossfades
a handful of global settings between a low and a high value. Here it sets:

1. the **depth** of the frequency modulation, 4% to 240%;
2. the **depth** of the amplitude modulation, 0 to 95%;
3. the **resonance** of every filter, **inverted** — right takes it down.
   Resonance has no bar, here or in the ensemble, where `res` is an input the
   tone generator feeds its levers;
4. the engine's **inertia**, the glide on every internal control, 1 s at hard
   left to 2.5 ms at hard right.

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
| **Display scale** | 1× to 8× on the Lissajous, the original's "Display Control". 1× is ±5 V filling the box; turn it up when the bank is running quietly |

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

## The normalizer

Each loop carries one, and its envelope follower is no longer a design of
mine. colB spotted "some sort of compression set up using peak detectors and
clippers on the post oscillator delay feedback sections", the `norm` macro in
the file confirms it, and the module reference then pins the behaviour down.
REAKTOR's Peak Detector rectifies the signal, **"the attack time of peak
detection is zero"**, and its release is quoted as the time for a peak to fall
to a tenth of its value: `Rel` 0 is 2.3 ms, 20 is 23 ms, 40 is 230 ms, 60 is
2300 ms, a decade per twenty on the knob.

So the attack here is instantaneous rather than the couple of milliseconds it
had before, which is audible — a transient pulls the gain down on the sample
it arrives on. The release runs at `Rel = 40`, 230 ms.

What happens *after* the envelope is still mine: the gain law, the ceiling,
and the saturator. The macro's five primitives are mapped but their classes
are unidentified, so that part is designed rather than read.

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

Beyond those two, what is left is the parts whose behaviour the file does not
give up: the normalizer's gain law after its envelope, the delay's
interpolation, and the arithmetic of the modulation chain, where 49 of the
ensemble's 57 module classes are still unidentified. Those are designed here,
not read.

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
| **long delays** | below about 10 ms a loop is a comb and settles in a few passes; up at 100–300 ms it takes a tenth of a second per pass and its state survives long enough to evolve |
| **feedback near unity** | at 0.5 every loop is safely damped and nothing ever builds. Push the **fbk** bars to 0.9–1.0 and loops build, saturate against the limiter and collapse, which is where the lurching comes from |
| **pitches close together** | eight channels spread over three octaves beat against each other at audio rate, which is timbre. Eight inside a fifth beat *slowly*, and the cross-modulation turns those slow beats into slow movement |
| **the coupling** | the **fm** and **am** bars decide who modulates whom. A bank where every channel listens to the same one behaves quite differently from a bank wired in a cycle |

The default bank is set that way — delays 30–307 ms, every loop between 0.88
and 1.0, pitches within a fifth — which is worth knowing if you wonder why it
sounds nothing like eight independent oscillators. An earlier default with half the feedback and 2–40 ms delays measured 0.04
octaves of wander; the current one measures **0.32 in the loop topology**,
against 0.75 for a reference recording of Skrewell standing still — and that
is with nothing invented propping it up. An earlier draft of this module had a
chaotically clocked two-state switch on each filter, which got the figure to
0.97 but is not in the patch anywhere; once the `fm` and `am` bars became
selectors and the bank could route its own modulation, the switch turned out
to be unnecessary as well as unfaithful, and it is gone.

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
- **Raw oscillators and a bit crush**, both shipped for a while and both
  removed. They came from colB's remark that Skrewell's sound owes something
  to it "being digital with aliasing and quantization", and both were wrong on
  two counts: the module reference says REAKTOR's oscillators *are*
  anti-aliased, so they were unfaithful, and measured against the finished
  engine they did almost nothing — raw moved the centroid 1454 to 1527 Hz and
  8-bit crush moved it 12 Hz further. Whatever aliasing the original has comes
  from its FM sidebands, which are still here.
- **Recovering the wander that lever pairs cost**, by reweighting the
  crossvoice against the ring. Swept from 0 (levers ignore their partner) to
  0.85 (they barely hear anything else): 0.48 down to 0.30 octaves, against
  0.82 with a single lever. It is the summing that does it, not the coupling.

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
port. The oscillator pairs and the count of filters and delays come from
**colB**'s reverse engineering of the ensemble in
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

Port names, though, *are* in the records — at the end of each one — and a
`KSModul` is followed by the port records belonging to it. So every macro's
**interface** comes out even though the wiring does not, and that turns out to
be most of what a re-implementation needs. The LEVER takes
`F A CUT TYP DEL FB fm am res rel nrm smt FM AM`, where lowercase `fm`/`am`
are amounts and uppercase `FM`/`AM` are the modulation signals arriving from
the `crossvoice` next door (`FV AV` + audio in, `FM AM` out). And each tone
generator's own inputs give its parameter list exactly: `F fm A am cut lbh
DEL FB` for the multimode one, `F fm A am hp lp DEL FB` for the bandpass one,
and `F fm A am DEL FB` — six — for the one with no filter. That is where the
**type** bar and the absence of a resonance bar in this module come from.

The wiring is in there too, and it took the right file to see it. Connections
are not objects — there is no cable class — they are stored as an *output's
fan-out*: a `KOutPort` payload ends in a count followed by that many
`(target, port)` pairs, where `target` indexes the enclosing macro's child
list. In a 4.5 MB patch with 1077 modules that is invisible; in a 33 KB test
instrument from
[github.com/fukuroder/Reaktor_Files](https://github.com/fukuroder/Reaktor_Files)
with twenty modules and twenty connections it is obvious. Skrewell yields 1308
connection entries, 1102 of them with a plausible target and port, and the
LEVER's insides read straight out: the `-300` constant goes to input 0 of a
class-188 module whose output feeds the filter macro's audio input, which is
AlbertoZ's fixed P inlet and identifies class 188 as the pulse oscillator.

Both loose ends are now tied off. The hierarchy is in the file after all: the
gap following a module's record begins `2, N, 1, 2` where **N is its child
count**, which reconstructs the tree and makes the sibling indices resolve —
`Pitch Gate` in the test instrument declares 7 and has exactly 7 children,
`snapvalue x 8` declares 24, being eight values and their sixteen terminals.
And the flag word's **bit 1 means "a connection list follows"**; without it
the record runs straight on to the port name, which is why the flag-4 outputs
looked like garbage. With both, 1069 of Skrewell's connections resolve to a
real target and a real input, 97.3% of them, and the rest are two macros whose
child count parses wrong.

That is where the loop order above comes from. The notes and the scripts are
in `~/dl/temp/reaktor-ens-tools/` rather than in this repository, since they
are a file-format reader and not a Rack module. What modulates what, in
what order, with what scaling, is still this module's own design, informed by
the factory manual's description and by the forum accounts. Anyone wanting to
go further should start by working out the layout of those blobs.
