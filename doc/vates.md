# vates

![vates](../img/vates.png)

**A stereo sample player that finds the beat for you: banks of hits it
generates itself, a knob that plays them backwards, and a pattern generator
underneath the whole thing.**

*vates* is Latin for the bard — the poet-seer, the one who sings what he is
given. The module is built after the Bastl Instruments *Citadel Wave Bard*,
whose idea is that you do not program a rhythm: you modulate sample
selection and let the rhythm fall out. Everything here serves that idea,
with the hardware's two modifier buttons unpacked into real controls, since
holding one thing while turning another is a gesture a mouse does badly.

It shares its kit loader with [pellicula](pellicula.md) and its sample
generators with [sylla](sylla.md) and [imber](imber.md), but it is neither:
pellicula is eight voices with no sequencer, sylla is one voice with no
banks, and vates is one voice, in stereo, with a sequencer built in.

## Where the sound comes from

**Six generated banks, then your own.** vates ships no audio files. Its
first six banks are synthesized from a seed the moment you place the module,
eight samples each:

| bank | what it holds |
|------|---------------|
| **drums** | drum voices, hats down to kicks |
| **objects** | struck objects — woodblock, tine, glass, marimba, vibraphone, harp string, membrane, bell |
| **grains** | microsound: dust, crackle, glissons, click trains, bubbles |
| **micro** | clicks, data bursts, test blips, sub thumps |
| **tones** | sustained harmonic material: drones, pads, chord stabs |
| **air** | sustained spectral material: filtered noise, washes, vowels, combs |

The first four are struck or granular, the last two sustain — which under
the **length** knob (below) is the difference between a hit and a swell.
Every generated sample is rendered at C, so pitch tracking and the quantizer
are exact without you tuning anything.

Each bank holds one sample of each kind its generator knows: the drums bank
is a click, a hat, a tom, a snare, a kick and so on, one of each, at eight
independent pitches — never one drum in eight sizes. A knob position
therefore always means the same *kind* of sound, which is what makes the
sample knob playable, and pitch variety is left to the pitch knob and the
note input, where it belongs.

The whole set derives from one seed, saved with the patch. **reroll kit** in
the context menu draws a new one: the same six flavours and the same eight
roles per bank, forty-eight different sounds.

**Your kits** follow the generated banks. vates reads the same kits folder
as pellicula — set it once from either module's context menu — where each
subfolder is a kit of `.wav` files ordered by filename. Stereo files stay
stereo. Kits load on a background thread, so browsing them never interrupts
the audio.

## The panel

### sample

| control | what it does |
|---------|--------------|
| **bank** | selects the bank: six generated, then your kits. CV in with attenuverter; the selection wraps. |
| **sample** | selects one sample within the bank. CV in with attenuverter, and it wraps too — modulation past the last sample comes back to the first. |
| **play / cue** | what modulation of *sample* does. See below. |
| **trig** | fires the selected sample. The button does the same by hand. |

**bank** and **sample** are the module's instrument. Nothing else needs
patching: a trigger and a moving CV on **sample** is already a part.

**play** fires a sample the moment modulation crosses into it, so the
modulation source *is* the rhythm — a triangle LFO into **sample** in play
mode gives you off-grid triggers whose density follows the attenuverter.
**cue** does not fire: modulation aims at a sample and it waits for **trig**,
so the same LFO stays on the grid and only chooses what plays.

On the hardware these two modes are the sign of one attenuverter, which
means play mode can never be modulated downward. Splitting the mode onto its
own switch fixes that: in either mode the attenuverter runs the full range,
positive or negative.

### pitch

| control | what it does |
|---------|--------------|
| **pitch** | playback rate, ±2 octaves, unquantized. |
| **pitch mod** | attenuverter for both pitch inputs. Fully clockwise is exactly 1V/oct. |
| **free** | continuous pitch modulation, applied as it arrives. |
| **note** | quantized pitch, latched when a sample is triggered. |

Two inputs for one parameter because they are two different musical jobs.
**free** bends: patch an envelope or an LFO and hear it move under the
sample. **note** plays: it is quantized to the root and scale set in the
context menu and it only updates on a trigger, so a sequencer's CV lands as
a note rather than a glide.

Root and scale live in the menu, following [sylla](sylla.md), and they
affect the quantizer only — generated banks are always rendered at C.

### length

One knob for the envelope *and* the playback direction, exactly as on the
hardware, because sweeping through the middle is the point.

- **centre**: the shortest envelope, a click of the sample.
- **right**: decay grows, the sample plays forward.
- **left**: attack grows, and the sample plays **backwards**.

The value is latched at the trigger, so modulation flips the direction
between hits and never mid-sample. **env** outputs the envelope, 0–10V.
During the attack of a reversed hit a new trigger is ignored, so reversed
swells are not cut short by the sequence that is playing them.

### tone

Two more knobs that do different things on each side of centre:

| knob | left | centre | right |
|------|------|--------|-------|
| **filter** | resonant lowpass, closing | open | resonant highpass, opening |
| **fx** | tempo-synced delay at 3/8, wetter | dry | chorus/flanger with soft clipping, deeper |

Both take CV. Sweeping **fx** across centre is a musical move — a beat that
falls out of the delay and into the flanger.

### lfo

| control | what it does |
|---------|--------------|
| **sync / free** | whether the LFO follows the clock or runs on its own. |
| **rate** | in sync, the clock divider; in free, 0.01–20 Hz. |
| **lfo mod** | attenuverter and input for the rate. |
| **reset** | a rising edge restarts the triangle at its peak. |
| **tri**, **pulse** | triangle and its rising-edge pulse, 0–10V. |

The hardware crams sync and free onto the two ends of one knob; its own
manual admits that modulation never crosses between them, only speeds the
LFO up or down. So the mode is a switch here and the knob means one thing at
a time.

The LFO is patch-programmable, which survives intact: **pulse** into **lfo
mod** tilts the triangle into a ramp or a saw, **tri** into **lfo mod**
bends it exponential or logarithmic, **pulse** into **reset** turns it into
a saw outright.

### clock and pattern

| control | what it does |
|---------|--------------|
| **tempo** | internal clock, 30–300 BPM. |
| **clk in** | external clock. It takes over while it runs; two seconds of silence hands the tempo back to **tempo**. |
| **clk out** | the clock in use, internal or external. |
| **rhythm** | selects one of 32 built-in 16-step gate patterns. |
| **gate**, **cv** | the pattern's gate (75% of a step) and its stepped CV, 0–10V. |
| **reset** | restarts both sequences. Patch a slow LFO here to shorten the pattern. |
| **G**, **C** switches | live surgery on the gate and CV sequences. |

The two three-position switches are the hardware's best idea and they come
over unchanged. Middle leaves the sequence alone. Up randomizes the step the
sequence is on right now. Down inverts it — silent steps sound, sounding
steps go quiet, and CV flips around its own midpoint. Flick one for a bar
and the pattern is different when you flick it back; hold it down and the
inversion keeps re-applying, so a 16-step pattern reads as 32.

Each switch is normalled to its input jack, **G in** and **C in**, so a
patched voltage does the same job: above 3.2V randomize, below 1.6V invert,
between them leave it alone.

The hardware's rhythms come from a web app that rebuilds the firmware. Here
they are 32 patterns on a knob, and the switches make them yours.

### output

**level** sets the output, **L** and **R** carry it, each with a lamp. One
voice at a time: a new trigger chokes the one that is sounding, as on the
hardware, and that choking is half of what makes a fast sequence sound
tight.

## Context menu

- **root**, **scale** — the quantizer for the **note** input.
- **reroll kit** — a new seed for the six generated banks.
- **kits folder** — shared with pellicula.
- **clock** — whether **clk in** may take over from the internal tempo.

## What was left behind

The hardware is a standalone instrument as much as a module, and the parts
that make it standalone are the parts Rack already has. Gone: MIDI in every
form (TRS, USB, the CC map, clock priority, MIDI learn), the headphone
output, the audio input with its gain and routing — patch a mixer — the tap
tempo, the hidden settings mode, and the sample loader web app, replaced by
generated banks and a kits folder.

Kept, because nothing in Rack does it as well: the length knob that reverses,
play versus cue, the two pattern switches, and the rule that you find the
beat by modulating what plays rather than by drawing it.
