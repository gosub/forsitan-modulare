# dræn

*dræn* is a **drone synthesizer**: a bank of drone engines, each a small
self-contained voice, played from just two controls — a fundamental (**hz**) and
a level (**amp**) — with a third control that selects which engine is sounding.
Change the engine and dræn fades the current one down and the next one up, so the
drone shifts without a hard cut.

It is a port of [dronecaster](https://github.com/northern-information/dronecaster),
a [norns](https://monome.org/docs/norns/) instrument by
[@northern-information](https://github.com/northern-information) and contributors.
In dronecaster each drone is a little
[SuperCollider](https://supercollider.github.io/) graph of the form
`{ |hz, amp| ... }`; dræn reimplements those graphs in C++ and drives them the
same way the original's `SynthSocket` does — one engine sounding at a time, with
an amplitude fade on every switch.

The name breaks from the collection's Latin: **dræn** is Old English for *bee*,
the etymological root of the word *drone*.

## How it works

One engine sounds at a time. **hz** sets its fundamental frequency and **amp**
its level; both take CV. The **engine** knob (and its CV) choose the active
engine from the roster, and the display shows its name. When the selection
changes, the current engine fades out over the **fade time**, then the newly
selected engine fades in — a sequential cross-fade rather than two engines
overlapping. The fade time is set from the right-click menu (0.25 s to 8 s).

Some engines are steady oscillators; others evolve on their own through internal
modulation. Because a fresh engine is (re)seeded when it becomes active, the
evolving ones start somewhere new each time you select them.

## Controls

| control | description |
|---------|-------------|
| **hz** | Fundamental frequency. Exponential, roughly 8 Hz–8 kHz; summed with the hz CV. |
| **amp** | Output level, 0–100%. Summed with the amp CV (10 V = full). |
| **engine** | Selects the active drone engine; the display shows its name. Summed with the engine CV (0–10 V spans the roster). |

## Inputs

| input | description |
|-------|-------------|
| **hz cv** | Frequency CV. By default **1V/oct**; the right-click menu can switch it to **linear** (100 Hz/V) added on top of the knob. |
| **amp cv** | Level CV (0.1 per volt, added to the knob). |
| **eng cv** | Engine-select CV. 0–10 V sweeps the whole roster; patch a sequencer or LFO to step or morph between drones. |

## Outputs

| output | description |
|--------|-------------|
| **l** / **r** | Stereo output, ±5 V nominal with a gentle soft-limiter on peaks. Many engines are genuinely stereo (e.g. supersaw spreads its voices). |

## Context menu

- **Hz CV input** — *1V/oct* (musical, tracks pitch) or *Linear (100 Hz/V)*.
- **Fade time** — duration of each fade-down / fade-up on an engine change.

## Engines

The initial roster is the set that needs only the base UGEN layer; it grows as
that layer fills out.

| engine | source | description |
|--------|--------|-------------|
| **sine** | @northern-information | Pure sine tone. |
| **square** | @taubaland | Band-limited square (50% pulse). |
| **triangle** | @taubaland | Triangle wave. |
| **supersaw** | @cfdrake | Five detuned, band-passed saws spread across the stereo field. |
| **harm's way** | @moonblind | Sixteen harmonics, each slowly amplitude-modulated; a shimmering additive drone. |
| **thx** | @infinitedigits | The THX "Deep Note": twelve saws sweep from a random cluster to a target chord. Here **amp doubles as the sweep position** (as in the original), so it shapes the sound rather than acting purely as a level. |
| **hecker** | @infinitedigits | Two stereo banks of sixteen filtered-noise voices, morphing between white and pink noise around the fundamental — a dense, evolving noise drone. |

Engines are loudness-normalized with a per-engine makeup gain so switching
between them doesn't jump levels; the quieter originals (which relied on norns'
master gain) are brought up to sit with the rest.

## Credits & license

Ported from **dronecaster** (© its authors, GPL-3.0). The per-engine author
credits above are carried over from the original SynthDefs. dræn, like the rest
of forsitan modulare, is licensed [GPL-3.0-or-later](../LICENSE).
