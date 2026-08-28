# experiments

Modules that were built, auditioned and not kept. The code is not deleted, it
is parked on annotated tags outside the branch list:

| tag | what | archived |
|---|---|---|
| `exp/sdt-machina` | stridor, crepitus, ruina, machina, and the `src/sdt/` port under them | 2026-08-09 |
| `exp/fracta` | fractal interpolation oscillator | 2026-08-09 |
| `exp/inedia` | starved-clock lo-fi delay core | 2026-08-09 |
| `exp/umbrae` | audio feedback instrument, after Dark Matter | 2026-08-19 |

To look at one:

```
git show exp/fracta                  # the tag message and the tip commit
git log --oneline master-v2..exp/fracta
git checkout -b revive exp/fracta    # a branch again, if it deserves one
```

The point of this file is not the code, which git keeps by itself. It is the
verdict and the reasons, which are the part that evaporates: without them the
idea sweeps in `ideas.md` re-propose these every few months and the same work
gets done twice to reach the same answer.

**All of them were rejected for the same reason: the sound.** Not the
implementation, not the CPU cost, not the panel. Each does what it says it
does, and none of them was worth listening to. That verdict is about the
result, not about whether the mechanism is interesting, so anything below
marked as salvageable is salvageable.

## sdt-machina: stridor, crepitus, ruina, machina

Four modules and a shared engine, ideas M0 to M3 and SC6 from `ideas.md`,
built together in one pass on 2026-08-09 to be auditioned as a set.

- **stridor** (10HP): dry friction as a voice. A probe pressed against a modal
  object with a normal force and dragged at a sliding velocity, sticking and
  slipping. Grinds at low speed, squeals as the slips lock onto the object's
  modes, hisses when the contact gives up and slides. One relaxation oscillator
  crossing a bifurcation, with **vel** as the knob that crosses it.
- **crepitus** (10HP): fracture as a point process. Impacts on a resonant
  object, with **crit** as the branching ratio of a Hawkes process deciding
  whether events ignore each other or set each other off. Self-limiting above
  criticality.
- **ruina** (12HP): the same engine framed as an object under load. Cube-law
  damage accumulation, a Weibull failure draw, a break trigger and a strain CV.
  Built alongside crepitus rather than instead of it, against `ideas.md`'s own
  advice, so the choice could be made by ear.
- **machina** (16HP): a combustion engine, a port of `SDTMotor`. Up to twelve
  cylinders on a shared crank, intake pipes, chambers that breathe with the
  pistons, an exhaust, expansion chamber, muffler bank and tailpipe, all
  waveguides. Intended as a rhythm source that is not a clock, with **trig**
  firing once per crank cycle.

Complete when archived: panels, docs, panel images, presets, `make check`
smoke tests, probe harnesses and a changelog entry. Measured CPU at 48 kHz was
stridor 1.1%, crepitus 1.2%, ruina 1.5%, machina 6.5% at twelve cylinders
(which would have been the heaviest module in the plugin) and 1.1% at one.

### What is worth salvaging

**`src/sdt/`**, the header-only port of the [Sound Design
Toolkit](https://soundobject.org/SDT/) (SkAT-VG/SDT, GPL-3.0-or-later, same
licence as this plugin). It covers the modal resonator, the impact and friction
interactors, the crumpling, breaking and scraping control layer, the motor, and
the filter and waveguide primitives. The C keeps one global sample rate, one
`rand()` stream and mallocs behind opaque structs; the port makes rate and RNG
per object and uses fixed-capacity arrays so the heap stays out of the audio
path. It is a working foundation for anything contact, material or machine
flavoured, and it is worth pulling back rather than redoing.

**Four settings that measurement forced.** If the port is ever revived, do not
"fix" these back to the SDT defaults without re-measuring:

1. Pickup gain 1 in stridor, 100 in the fracture engine. The resonator's pickup
   gain scales the velocity the contact senses, not just the output, so for
   friction it is the loop gain: at the tutorial patches' 100 the viscosity term
   damps every mode to Q of about 4 and the object never rings. Impact has no
   velocity feedback worth speaking of, and 100 is what gives a click instead
   of a sine burst.
2. stridor's modes weigh 5 g and **vel** is exponential over 1.5 mm/s to
   0.8 m/s. Heavier modes, or a linear 0 to 3 m/s knob, and the contact simply
   slides at every playable speed: no stick-slip, no squeal, just hiss.
3. crepitus needs a 0.5 ms refractory period. Without it a supercritical
   setting saturates at one event per sample, and because an event sets the
   hammer's velocity, that pins the hammer instead of striking with it. The
   module went silent at exactly the setting that should have been loudest.
4. machina uses 2500 Hz body damping, not `SDTMotor`'s nominal 20. Nothing in
   the C ever applies the 20: `SDTMotor_new` leaves the one-poles at
   pass-through and only a host setting the attributes calls `update()`.
   Applied literally it lowpasses the intake hiss and the block radiation to
   nothing.

**The Hawkes construction** in `fracture_dsp.hpp` is original design work, not
port: the SDT ships crumpling and breaking as separate models, joined here by
making the event rate self-exciting with the branching ratio as a knob. The
rate also scales with remaining integrity, so above criticality the cascade
eats the object down to `integrity = 1/crit` and parks. Self-organised
criticality, verified against `1/crit` in the probe.

One thing did come back into the plugin from this branch:
`tools/panel-editor/regen_panel.py`, which rebuilds a panel SVG from a
hand-written `@layout` block without opening the browser editor.

## fracta: fractal interpolation oscillator

A wavetable oscillator whose table is generated by fractal interpolation after
Monro, *Fractal interpolation waveforms*, Computer Music Journal 19:1 (1995).
An initial 2 or 3 segment core is shear-transformed into itself to a chosen
iteration depth, with a global displacement parameter warping the result. At
one iteration the warp control is a wavefolder; deeper, it is the fractal
displacement.

The target was the Blukac **Fractalist**, whose manual says **warp** acts as a
waveshaper at one iteration and as the IFS displacement `d` beyond that.
Started 2026-07-14, archived at 22 commits. It built and was registered in the
plugin, and the panel was finished. It never got a `doc/fracta.md`.

One design decision was left open at archive time, and it is the one to settle
first if this ever comes back: **the manual does not say which waveshaper**.
The code uses a triangular wavefolder at one iteration, gain `4^warp` over 128
points, explicitly marked as a placeholder pending a scope trace, more demos,
or an answer from Blukac. The gain formula and the point density are both
tuning candidates rather than findings.

## inedia: starved-clock lo-fi delay

Not a module, a DSP core plus an audition harness. A digital effect running off
a dying battery, after Nathan Ho's
[low battery audio effects](https://nathan.ho.name/posts/low-battery-audio-effects).
A dead battery has high internal resistance, so its terminal voltage sags under
load; a cheap device's master clock is an RC or ring oscillator that tracks
Vcc, so the sag detunes the clock. The audio is the load, which closes the
loop: loud output, more current, more sag, slower clock, pitch and time droop.

The appeal was that this is one mechanism rather than a per-effect hack. An
arbitrary inner engine hangs off the starved clock and inherits the behaviour;
a chip delay was the first one hung off it.

Started 2026-07-20, archived at 3 commits: `src/inedia_engine.hpp`, a probe,
and a `test/Makefile` entry. No module, no panel, not registered.

Three details in the header are load-bearing and are documented there, in case
this is ever picked up: the DC block on the load (without it a loud input pins
the rail and the effect sticks instead of drooping and recovering), the
zero-order-held output against a fixed reconstruction filter (the aliasing is
the sound, and a tracking anti-imaging filter would remove the grit), and the
delay time being fixed in inner samples rather than seconds (which is what
makes it read as tape rather than vibrato).


## umbrae: audio feedback instrument

A 20HP loop around a saturating two-band tone section, after Bastl Instruments
and Casper Electronics' **Dark Matter**. Below unity an overdrive with a
resonance in it; above unity a howl whose register the two band faders pick -
around 110 Hz with the bass fader up, 3.3 kHz with the treble one, a kilohertz
or so with both. An input VCA with the hardware's x3 gain and soft clipping in
front, a crossfader between the clean signal and the fed-back one behind, an
envelope follower normalled to the feedback and crossfade CV so a signal gates
the feedback it causes, and send/return jacks to put a delay or a reverb
inside the loop.

Built 2026-08-19 off the aether branch, archived at 4 commits. Complete when
archived: engine, module, panel, manual, panel image, plugin entry, changelog
entry, a probe harness and 30 passing smoke checks.

### The verdict

Not interesting enough acoustically or musically, and not versatile - in
neither of the two things it does. As a self-oscillating instrument it makes
one howl per setting, and the settings do not lead anywhere; as a processor of
external audio it is an overdrive with a resonance, and the loop does not earn
the panel it takes up. That is a judgement on the module, not on the port: the
loop does what the block diagram says it does, at the pitches and levels the
manual describes, and the measurements below all held.

Worth knowing before anyone revives it: there is no Dark Matter schematic, so
every frequency in the build was inference - band corners, loop band limits,
amplifier bandwidth, the envelope's time constants. It is not knowable from
here whether a different guess at those would have made a more interesting
instrument, and finding out means tracing a board rather than writing more
code. The generous reading is that a feedback loop of this shape is thinner in
a rack than it is on a desk, where a no-input mixer's interest comes from
hands on several faders at once and from what the mixer's own circuit does
badly.

### What is worth salvaging

**The loop delay, which is the whole technique.** Dark Matter's loop is
instantaneous, and its musical identity is that it oscillates at "the sound of
the circuit itself, its own resonant frequency". Ported with the one-sample
delay a digital loop implies, it screams near Nyquist at a pitch that moves
with the host's sample rate - a different instrument at 44.1 kHz and at
96 kHz. The fix was to give the loop an explicit **16 us propagation delay**,
a few op-amp stages' worth of group delay, read out of a fractional delay
line, so the pitch falls out of modelled time constants instead of out of the
grid. Measured across an 8.7:1 range of engine rates, 176.4 kHz to 1.536 MHz,
one patch held 75.6-75.7 Hz.

Two things fall out of that and would fall out again:

- the delay line needs two samples to interpolate between, so the module has a
  **minimum oversampling ratio** rather than a preferred one (4x, which is
  176.4 kHz at a 44.1 kHz host). The menu offered nothing lower and said why.
- the read happens before the write, so a request of *d* samples comes back
  *d + 1* later. Not compensating for that made the effective delay vary from
  8.6 to 13.7 us across sample rates, which is a 10% spread in pitch - the bug
  looked exactly like the problem the delay was there to fix.

**And the measurement that catches it.** Any module here whose sound is a
self-oscillating loop should be held to one number: the oscillation frequency
at 44.1, 48, 96 and 192 kHz, and across every oversampling setting, has to be
the same. It is a two-line check and it is the difference between a port and a
thing that happens to sound good on the machine it was written on.

The engine is `src/umbrae_dsp.hpp` on the tag, Rack-free, with the loop
delay's arithmetic and the reasoning documented in its header.
