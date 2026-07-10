# forsitan modulare

A collection of VCV Rack modules.

![forsitan-modulare build](https://github.com/gosub/forsitan-modulare/workflows/forsitan-modulare%20build/badge.svg)

![forsitan modulare](img/forsitan-modulare.png)

## Modules

| module | description | doc |
|--------|-------------|-----|
| [alea](doc/alea.md) | Add a random module to your rack | [doc/alea.md](doc/alea.md) |
| [interea](doc/interea.md) | Chord generator from a V/Oct input, with quality, voicing and inversion | [doc/interea.md](doc/interea.md) |
| [cumuli](doc/cumuli.md) | Accumulator with up/down gates and rates | [doc/cumuli.md](doc/cumuli.md) |
| [deinde](doc/deinde.md) | Quad cascading addressable attack-hold envelope | [doc/deinde.md](doc/deinde.md) |
| [pavo](doc/pavo.md) | Polyphonic stereo spreader (Splay Ugen) | [doc/pavo.md](doc/pavo.md) |
| [limen](doc/limen.md) | TCP+JSON control interface for VCV Rack | [doc/limen.md](doc/limen.md) |
| [MMCCCXCIX](doc/mmcccxcix.md) | PT2399 delay chip emulation with feedback send/return loop | [doc/mmcccxcix.md](doc/mmcccxcix.md) |
| [scando](doc/scando.md) | Scanned-synthesis oscillator (vibrating mass-spring string, scanned into a wavetable) | [doc/scando.md](doc/scando.md) |
| [pellicula](doc/pellicula.md) | Exploded 8-voice drum sampler: a knob + CV per voice for sample, pitch, decay and level, poly-normalled | [doc/pellicula.md](doc/pellicula.md) |
| [dræn](doc/draen.md) | Drone synthesizer: two banks of 37 drones (the dronecaster set and the original hyf set) played from hz + amp, with fading engine selection | [doc/draen.md](doc/draen.md) |
| [rete](doc/rete.md) | Feedback integrator network: 8 nodes in a random mixing matrix, self-oscillating chaos with per-node gain CVs | [doc/rete.md](doc/rete.md) |
| [ululo](doc/ululo.md) | Feedback guitar: six comb strings against a saturating amp, howling loop tunable by poly V/oct | [doc/ululo.md](doc/ululo.md) |
| [tabes](doc/tabes.md) | Disintegration looper: the tape loop degrades a little more on every pass | [doc/tabes.md](doc/tabes.md) |
| [lustro](doc/lustro.md) | Scanned filter: scando's mass-spring string plays the band gains of a 16-band resonant filterbank | [doc/lustro.md](doc/lustro.md) |

## Tools

| tool | description |
|------|-------------|
| [limen-tools](https://github.com/gosub/limen-tools) | limen CLI client (separate repo) — C and Python, prebuilt binaries on releases |
| [tools/panel-editor/](tools/panel-editor/) | Browser-based drag-and-drop panel layout editor |

## Controlling Rack externally (limen)

The [limen](doc/limen.md) module runs a small TCP server speaking newline-delimited JSON, so external tools can drive Rack: list/add/remove modules and cables, get/set parameters, and control the window (fullscreen, zoom-to-fit, quit). It is built for **automation and scripting**, **LLM tools** that build patches programmatically, and **alternative interfaces** (such as a planned Emacs mode).

Quickstart:

1. Launch Rack straight into a controllable state with the bundled patch — it contains a single limen module with the server already enabled: `./Rack patches/limen.vcv`. (Or add a **limen** module to any patch and enable its server from the right-click menu.)
2. Talk to it with the [limen-cli client](https://github.com/gosub/limen-tools): `limen-cli hello` (or `list_modules`, `set 0 0 0.5`, …).
3. Or from anything that opens a socket: `echo '{"cmd":"hello"}' | nc localhost 7000`.

**Security:** the server binds loopback only (`127.0.0.1`) and has no authentication by design, so it is reachable only from your own machine. Do not expose the port to untrusted networks. See [doc/limen.md](doc/limen.md) for the full protocol and command reference.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## About the names

Being Italian, giving the modules English names felt off, but Italian names would have been less accessible to most users. Latin seemed like a natural middle ground: familiar enough, international, and with a certain character.

| module | Latin meaning |
|--------|--------------|
| **alea** | dice, chance (as in *alea iacta est*, the die is cast) |
| **interea** | meanwhile, in the meantime |
| **cumuli** | heaps, piles (plural of *cumulus*) |
| **deinde** | then, next, afterwards |
| **pavo** | peacock (whose spreading tail mirrors the stereo spread) |
| **limen** | threshold, doorway |
| **MMCCCXCIX** | 2399 in Roman numerals — the PT2399 chip this module emulates |
| **scando** | I climb / I scan — *scandere* is also the Latin for scanning verse, the root of "scan" |
| **pellicula** | little skin, thin membrane (diminutive of *pellis*, skin/hide) — the drumhead, and an echo of "pico" (small) |
| **dræn** | *not Latin* — Old English for *bee*, the etymological root of "drone" (a port of the dronecaster norns instrument) |
| **rete** | net, network (the 8×8 feedback web at the module's heart) |
| **ululo** | I howl (guitar feedback, the Larsen effect as an instrument) |
| **tabes** | wasting away, decay, consumption (the tape loop dying pass by pass) |
| **lustro** | I traverse, I survey (the string surveyed into a filterbank; scando's sibling) |

## Author

Giampaolo Guiducci <giampaolo.guiducci@gmail.com>

## License

[GPL-3.0-or-later](LICENSE)
