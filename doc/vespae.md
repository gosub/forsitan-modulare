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
four responses come out at once: **lp**, **bp**, **hp** and **notch**.

## Controls

| control | function |
|---------|----------|
| **cutoff** | 20 Hz – 20 kHz |
| **fm** | attenuverter for the **fm** input (±1 V/oct at the extremes) |
| **trk** | how much the **v/oct** input moves the cutoff, 0–100 % |
| **res** | resonance. The last tenth of the travel tips it into self-oscillation |
| **drive** | input level, −21.6 dB to +21.6 dB, unity at noon. This is the dirt control |
| **grit** | supply headroom, from a roomy 12 V down to a mean 2 V |

| jack | |
|------|--|
| **in** | audio in |
| **v/oct** | 1 V/oct cutoff, scaled by **trk** |
| **fm** | cutoff CV through the **fm** attenuverter |
| **res** | resonance CV, ±5 V for the full range |
| **lp** / **bp** / **hp** / **notch** | the four simultaneous outputs, each with a level LED |

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
- The four outputs are simultaneous, so you can take **lp** to the mixer and
  **hp** to a delay send off the same filter.
- The module self-oscillates with nothing patched: it is a serviceable sine
  (well, sine-ish, and less so as **grit** rises) with 1 V/oct tracking.

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

**Deliberately different.** Two places where the circuit, taken literally,
does not make a good module:

- The circuit's own damping never quite reaches zero, so at maximum **res**
  the last of it is cancelled to give genuine self-oscillation. The
  hardware does sing; the small-signal analysis alone does not predict it.
- Read literally, the diode clamp holds the resonance to about a tenth of
  the rail — while the paper's own state-space plots show the integrator
  states reaching those rails at high resonance. The clamp is backed off by
  a fixed trim so that self-oscillation lands where the hardware sits.

Both are single named constants at the top of `src/vespae.cpp`.

`test/vespae_probe` prints the measurements this was tuned against:
magnitude response, Q versus cutoff, self-oscillation level and frequency,
THD versus **drive** and **grit**.
