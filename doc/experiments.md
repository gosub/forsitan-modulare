# experiments

Modules that were built, auditioned and not kept. The code is not deleted, it
is parked on annotated tags outside the branch list:

| tag | what | archived |
|---|---|---|
| `exp/sdt-machina` | stridor, crepitus, ruina, machina, and the `src/sdt/` port under them | 2026-08-09 |
| `exp/fracta` | fractal interpolation oscillator | 2026-08-09 |
| `exp/inedia` | starved-clock lo-fi delay core | 2026-08-09 |

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

**All three were rejected for the same reason: the sound.** Not the
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
