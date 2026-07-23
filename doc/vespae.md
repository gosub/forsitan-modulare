# vespae

**The Wasp filter: a state-variable filter built out of CMOS inverters on a
single unipolar supply, and dirty because of it.**

*vespae* is Latin for "of the wasp". It emulates the filter Chris Huggett
designed for the 1978 EDP Wasp — specifically the Doepfer A-124 version of
it, which is the one most people have actually heard.

The Wasp was built to a price. Rather than op-amps, Huggett used the six
CMOS inverters in a CD4069 as amplifiers, and ran the whole synthesizer from
one +5 V rail with no negative supply. Both decisions were about money, and
both are why the filter sounds the way it does. A CMOS inverter used as a
linear amplifier is a sloppy, asymmetric thing whose switching threshold is
not quite mid-supply; with no negative rail there is nowhere for a signal to
go but into the rails; and a pair of diodes across the resonance network
clamps the feedback once things get loud. Push it and it does not politely
compress — it goes lopsided, buzzes, and shifts under you.

Structurally it is an ordinary 12 dB/octave state-variable filter, so all
four responses come out at once: **lp**, **bp**, **hp** and **notch**. A
fifth output, **mix**, is the A-124's own: a pot crossfading the lowpass and
highpass nodes.

## Controls

| control | function |
|---------|----------|
| **cutoff** | 20 Hz – 20 kHz |
| **res** | resonance. The last tenth of the travel tips it into self-oscillation |
| **drive** | input level, −21.6 dB to +21.6 dB, unity at noon. This is the dirt control |
| **grit** | supply headroom, from a roomy 12 V down to a mean 2 V |
| **mix** | crossfades the **mix** output from pure lowpass (full left) to pure highpass (full right) |

| trimpot | function |
|---------|----------|
| **fm** | attenuverter for the **fm** input (±1 V/oct at the extremes) |
| **trk** | how much the **v/oct** input moves the cutoff, 0–100 % |
| **bias** | *mod*: walks the inverter's switching point off mid-supply, so the clipping goes lopsided |
| **hiss** | *mod*: raises the inverter noise inside the feedback loop |

| jack | |
|------|--|
| **in** | audio in |
| **v/oct** | 1 V/oct cutoff, scaled by **trk** |
| **fm** | cutoff CV through the **fm** attenuverter |
| **res** | resonance CV, ±5 V for the full range |
| **mix** | mix CV, ±5 V for the full range |
| **lp** / **bp** / **hp** / **notch** / **mix** | the five simultaneous outputs, each with a level LED |

## The mix output

The real A-124 has only two jacks: a bandpass output, and one output fed by
a pot with the lowpass on one end and the highpass on the other. That pot is
the **mix** knob, and **mix** is its output. We keep the four filter nodes on
their own jacks as well, so nothing is lost by having it.

It is worth a knob of its own because it is not simply a fader between two
sounds. Blending a lowpass and a highpass always produces a null, and the
null moves as you turn:

| **mix** | null sits at | result |
|---------|--------------|--------|
| hard left | — | pure lowpass |
| left of centre | above the cutoff | lowpass with a notch above it |
| centre | the cutoff | symmetrical notch |
| right of centre | below the cutoff | highpass with a notch below it |
| hard right | — | pure highpass |

The null is at `fc·√((1−mix)/mix)`, which is the manual's "asymmetrical /
symmetrical / asymmetrical notch". Sweeping the **mix** CV with an LFO
sounds like phasing, as Doepfer's manual points out — and unlike a real
phaser you can move the cutoff at the same time.

Two things to know. The pot is passive, so at the centre both halves are at
half level and the notch output is 6 dB down on the extremes; that is what
the hardware does and it is not compensated. And the notch is only deep
while the filter is behaving: at a 20 mV probe level the null measures below
−70 dB, but by 1 V in it is −30 dB and by 5 V only −10 dB, because the
saturating OTAs pull the cutoff around at twice the signal frequency and the
null smears with it. Drive it hard and the notch opens up.

## drive and grit

These two are the module. **drive** sets how hard the signal hits the
circuit; **grit** sets what it hits.

**drive** is the Wasp's own level pot. At noon the module is unity gain and
a normal ±5 V signal sits right at the edge of the rails — which is where
this filter is supposed to live. Back it off and vespae is a clean,
well-behaved SVF. Push it and the summing inverter and both integrators
start clipping against their supply, asymmetrically, and the OTAs saturate
and drag the cutoff down with the signal envelope.

**grit** is the supply rails, and it is the axis between the original +5 V
EDP machine and the +12 V Doepfer module. It is not a level control and it
does nothing at all if **drive** is low — there is no clipping to shape.
Turned down, the rails are far away, so the OTA's tanh compression is the
first thing you meet: the filter squashes, sags in pitch, and rounds off.
Turned up, the rails close in and the tanh knee ends up above them, so the
signal runs linear and then slams: square, buzzy, rude. It also decides how
long the diode clamp lets the resonance run before it bites, so a high
**grit** self-oscillates several times louder than a low one.

## bias and hiss: the two mods

The trimpots on the right of **cutoff** are not on the A-124. They push the
filter past what the circuit does, and they work in **opposite regimes**,
which is why they are two trimpots and not one:

**bias** walks the CD4069's switching threshold further off mid-supply. The
two halves of the waveform then clip at very different levels, and the rasp
turns even-harmonic — measured at a hard-driven setting, the even/odd
harmonic ratio goes from 0.07 to 2.7, a 37-fold shift, while the output
level moves less than 2 %. It does **nothing** on a clean patch, because
there is no clipping to make lopsided. Turn **drive** up first.

**hiss** raises the inverter's own noise inside the feedback loop. On a
loud signal it is inaudible — the signal swamps it. Its real job is near the
top of **res**, where the loop is barely stable: the noise wanders the
operating point and the filter stops being able to hold a steady note.
Non-harmonic energy goes from 0.4 % to 7.7 % just short of self-oscillation,
and the self-oscillation's own amplitude wobble quadruples. Fully down it is
inaudible (7 µV), which also keeps silence silent.

So: **bias** for gnarl when you are driving it hard, **hiss** for
instability when you are sitting on the edge of resonance. Both at once,
with **drive** and **res** up, is the nastiest the module gets — and it
still cannot run away, because the rail clippers bound the loop structurally
no matter where these are set.

## What the filter does that a normal SVF does not

- **Maximum Q depends on where you are.** The LP feedback leg has a small
  capacitor across it, which adds damping in proportion to the cutoff
  frequency. The filter is at its most resonant around 300–600 Hz and gets
  progressively tamer as you open it: measured here, Q ≈ 10.3 at 640 Hz but
  only 3.9 at 10 kHz. This is real Wasp behaviour, not a bug — the knob is
  not broken at the top of its range.
- **The resonance network is frequency-dependent.** The feedback path is not
  a plain gain but a first-order shelf, so raising **res** does not simply
  scale the damping, it tilts it.
- **Self-oscillation drifts flat as it gets loud.** The OTAs saturate, the
  effective integrator gain drops, and the pitch falls with it — up to about
  a semitone. Driven (below self-oscillation) the tuning is exact.
- **It is lopsided.** The inverter's switching point sits below mid-supply,
  so the positive and negative halves clip at different levels with
  different knees. That is where the buzz comes from.

## Tips

- The classic sound is **drive** past noon, **res** high, **grit** up, and
  the **bp** output. Sweep the cutoff by hand.
- For an acid-ish line, patch **v/oct** with **trk** at full, put a decay
  envelope into **fm**, and keep **drive** near noon so the accent notes
  break up and the quiet ones do not.
- **notch** with **res** low is a usable tone-shaper; with **res** high it
  is a narrow, moving hole.
- A slow LFO into the **mix** CV, **res** low, and the **mix** output is a
  passable phaser. Add a second, slower LFO on the cutoff and the two nulls
  drift against each other.
- The five outputs are simultaneous, so you can take **lp** to the mixer and
  **hp** to a delay send off the same filter — or **mix** to one and **bp**
  to another.
- The module self-oscillates with nothing patched: it is a serviceable sine
  (well, sine-ish, and less so as **grit** rises) with 1 V/oct tracking. The
  hardware cannot do this; see the notes at the end.

## Oversampling

Right-click for **Oversampling**, 1×–16×, default 2×. The nonlinearities
alias, and 2× is a good trade; at 1× the aliasing is audible on hard-driven
high-frequency material and is arguably part of the fun. The whole module
costs about 1 % of one core at 48 kHz at the default setting.

## Notes on the emulation

The topology, the component values, the resonance network, the damping terms
and the *shapes* of the nonlinearities are taken from the circuit analysis
in:

> L. Köper, M. Holters, F. Esqueda and J. D. Parker, "A Virtual Analog Model
> of the EDP Wasp VCF", Proc. 25th Int. Conf. on Digital Audio Effects
> (DAFx20in22), Vienna, September 2022.

That paper builds a white-box state-space model with fitted MOSFET models
for each half of the CD4069 and a measured rail model for the CA3080 OTAs,
and solves it numerically. This module is not that. It is a real-time
caricature that keeps the parts you can hear and drops the parts you cannot
afford:

**Kept, from the circuit.** The state-variable topology and its component
values; the resonance network solved as the paper's Δ-to-Y star, including
its first-order shelf behaviour; the R2·C2 damping term that costs the
filter its Q as it opens; the OTA `tanh` law with the paper's input divider
and fitted scaling; the diode pair across R4 with fitted Shockley
parameters; the asymmetric rail knees and the sub-mid-supply inverter
threshold.

**Simplified.** The loop is a zero-delay-feedback SVF rather than a Newton
solve, and the OTA saturation is applied by scheduling the integrator gains
on the previous sample instead of iterating. The CD4069 is a soft asymmetric
clipper rather than the paper's two fitted MOSFETs.

**Deliberately different.** Three places where the circuit, taken literally,
does not make a good module:

- **Five outputs where the hardware has two.** The A-124 brings out only the
  bandpass and the LP/HP mix; the lowpass, highpass and notch exist inside it
  but never reach a jack. They are all on the panel here, with the mix pot
  kept as well (and given a CV input, as on the A-124-2 slim version).
- **The bias and hiss trimpots**, which are mods rather than modelling.
  Neither exists on the hardware; see above for what they do.

- **Self-oscillation, which the hardware does not have.** The A-124 manual
  is blunt about it: "The filter can't go into self oscillation, in contrast
  to most of the other VCFs in the A-100 system." Its damping never quite
  reaches zero. Here the last tenth of the **res** travel cancels that
  damping anyway, so the filter sings — bounded by the diode clamp, and
  tracking 1 V/oct. This is an addition for the sake of the module, not a
  correction of the circuit.
- Read literally, the diode clamp holds the resonance to about a tenth of
  the rail — while the paper's own state-space plots show the integrator
  states reaching those rails at high resonance. The clamp is backed off by
  a fixed trim so that self-oscillation lands where the hardware sits.

The latter two are single named constants at the top of `src/vespae.cpp`.

`test/vespae_probe` prints the measurements this was tuned against:
magnitude response, Q versus cutoff, self-oscillation level and frequency,
the mix output's null position and depth, and THD versus **drive** and
**grit**.
