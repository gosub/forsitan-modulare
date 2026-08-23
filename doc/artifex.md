# artifex

![artifex](../img/artifex.png)

**Nine stereo effects that share one feedback loop: a delay, a flanger, a
freezer, a panner, a crusher, a slicer and three ways of moving pitch — with
a pattern generator and an LFO wired to every parameter that matters.**

*artifex* is Latin for the maker, the contriver — the craftsman who works a
material rather than choosing from a catalogue. The module is built after the
Bastl Instruments *Citadel FX Wizard*, the other face of the hardware
[vates](vates.md) is built after, and it keeps that machine's idea: you do
not step through presets, you take three knobs that mean something different
in every mode and modulate them until the effect becomes an instrument.

The hardware folds three functions onto most knobs behind a SHIFT button.
Here they are unpacked into real controls, for the same reason as in vates:
holding one thing while turning another is a gesture a mouse does badly.

artifex and vates are built to sit next to each other. Their bottom halves
are the same instrument — the same tempo generator, the same 32 rhythms, the
same pattern generator with its two switches, the same LFO — because on the
hardware they are one circuit board with two panels.

## The FX core

Every mode is a variation on one signal path:

```
  L/R in ──▶ gain ──▶( + )──▶ [ mode ]──┬──▶ amount ──▶ level ──▶ L/R out
                       ▲                │
                       │                ▼
                       └── feedback ◀── filter
```

Three knobs drive it, and they keep their meaning across all nine modes even
though the mode decides what they act on:

- **time** is always the mode's rate: how fast it repeats, modulates, chops
  or scans. Clockwise is always faster, whatever "faster" means there.
- **feedback** is the loop above — the module's own output, filtered, folded
  back into its input. It is not a repeat count. It interacts with what you
  are feeding in, so the level at the input changes the character of the
  feedback, which is why there is an input gain.
- **amount** is how much effect you get. Fully left is the dry signal.

Two more knobs shape the loop rather than the mode:

- **filter** is open at the centre, a lowpass to the left and a highpass to
  the right, and it does not touch the dry signal — it colours the effect,
  not what you put in. *Where* it sits inside the effect depends on the mode,
  and the context menu can move it; see [the filter](#the-filter) below.
- **stereo** detunes the mode's own time parameter between left and right.
  At zero the two channels are identical; turned up they drift apart, and in
  most modes that is the whole stereo image.

**trig** aligns the mode to a beat. What it does depends on the mode — reset,
freeze, fill, dip — and it is listed per mode below. The button beside the
jack does the same thing by hand.

Four of the modes used to reset a phase that a read position or a gain depends
on, which landed the output somewhere it was not heading — a click, and not a
gesture. None of them jump any more: the panner and the flanger turn round from
where they are, the shifter squares its right channel up to its left instead of
moving both, and the pitcher takes its new window at a grain boundary where the
crossfade already has it silent. What each trig *does* is unchanged.

Behind that, a step left at the output by anything else — the freezer and the
replayer swap the buffer under the playhead, which is a discontinuity by
nature — is measured against where the last two samples were heading and
subtracted, relaxing over two milliseconds.

## The nine modes

| # | mode | time | amount | feedback | stereo | trig |
|---|------|------|--------|----------|--------|------|
| 1 | **delay** | delay time, 1.15 s → 2 ms | dry/wet | repeats | L/R delay detune | a clock here snaps the time, if clk has none |
| 2 | **flanger** | modulator frequency | sweep depth + wet | resonance | L/R modulator detune | turn the sweep round |
| 3 | **freezer** | repeat length | wet, and refreezes | feeds new audio in | L/R repeat detune | freeze a new chunk |
| 4 | **panner** | pan frequency, up to audio rate | sine → square | global | L/R pan detune | throw the pan to the other side |
| 5 | **crusher** | sample rate | crush depth, then XOR | distorted backdrop | L/R rate detune | dip the rate |
| 6 | **slicer** | which rhythm chops | decay + wet | chance of an inverted step | a different rhythm per channel | fire the envelope |
| 7 | **pitcher** | window size | shift amount + wet | global | L/R window detune | enlarge the window |
| 8 | **replayer** | tape speed and direction, never stopped | record/lock | feedback into the tape | L/R speed detune | fill the tape |
| 9 | **shifter** | shift, down below centre and up above | dry/wet | global | a different shift per channel | resync both channels |

### 1. delay (green)

A clean stereo delay, 1.15 s at the far left down to 2 ms at the far right.
Feedback is taken *before* the filter, as on the hardware, so a repeat is
filtered once on its way out and the tail does not darken pass by pass. The
context menu's **filter inside the feedback** changes that, and turns the mode
into a dub delay.

The tape glides towards the time knob instead of jumping to it, over about
50 ms. Moving the knob therefore bends the pitch of whatever is already in the
delay, the way changing a tape echo's motor speed does: down as you lengthen
it, up as you shorten it, and further the faster you move. It applies to the
clock snap below as well, so stepping between divisions bends rather than
cuts.

With a clock at **clk** the time knob snaps to the nearest division of it —
1/256 through 32 bars, whichever of them fall inside the delay's own range —
and the display says which. That is the module's own clock, the one the
freezer, the slicer, the LFO and the pattern all follow. A clock at the
**trig** input snaps the delay too, which is where the hardware takes one, but
only while **clk** is carrying none: patching a rhythm into trig should stutter
the mode, not redefine what a bar is. With neither, nothing snaps and the knob
is free.

Short times turn the delay into a comb filter, and with feedback up it becomes
a tuned resonator you can play from the time knob.

### 2. flanger (cyan)

A delay whose time is swept by a sine. **amount** sets how far it sweeps and
how much of it you hear: around the middle, with no feedback, it is a stereo
chorus; with feedback it is a flanger; at the extremes the sweep is deep
enough to be pitch modulation. The depth scales the tap's position, so it
glides towards the knob rather than stepping with it. **stereo** detunes the
two modulators, which is what opens the image.

### 3. freezer (blue)

Captures a chunk of audio and loops it. Three things freeze a new chunk:
arriving in the mode, moving **amount** away from zero, and a **trig**. A
clock at **clk** is deliberately not one of them — that would make the mode a
rhythmic re-sampler rather than a freezer.

**time** sets the loop length, and it moves it *live*, on the audio already
held: to the left it is a division of the tempo, so the freeze is rhythmic; to
the right it shrinks until the loop is short enough to be a pitch, and you are
freezing the timbre rather than the bar. Turning it from one end to the other
takes a frozen bar down to a few milliseconds without ever re-capturing, which
is the mode's best gesture.

A freeze catches as much history as it has, not only the length you asked for,
which is what leaves the knob somewhere to go. The one time that bites is
arriving in the mode: it freezes as soon as it has the length the knob is
asking for, so there is nothing older to lengthen into yet. Give it a second
and hit **trig** and it takes the whole buffer, after which the knob has its
full range. The longest divisions need a buffer to fit in — 16 steps at 120 BPM
is two seconds, so at the hardware's 1.15 s they clamp; the context menu's
larger buffers hold them.

The loop is the *end* of what was caught: shortening keeps the audio nearest
the freeze point, which is what you had just heard, rather than the oldest of
it. **feedback** here does not run the global loop — it lets new audio bleed
into the frozen buffer, thickening it.

### 4. panner (white)

Amplitude modulation in opposite phase on the two channels. Slow, it is an
autopanner. Fast, it crosses into audio rate and becomes stereo ring
modulation. **amount** clips the modulating sine towards a square, so the pan
goes from a sway to a hard alternation. **trig** throws it to the other side:
the modulator goes to the peak on that side and *glides* there over about
25 ms, capped at a quarter of its own period so it stays out of the way up at
ring-modulation rates. The glide is the throw — a gain crossing the image in a
few tens of milliseconds is a sound moving, where the same distance in one
sample is only a click.

### 5. crusher (yellow)

Downsampling and bit mangling. **time** is the sample rate it decimates to,
from 200 Hz at the far left up to the patch's own sample rate at the far
right, where the decimator holds for exactly one sample and passes the signal
through untouched. **amount** deepens the effect and then folds in an XOR of
the sample with itself towards the top, which is where it stops sounding like
a bitcrusher and starts sounding broken. Feedback adds a distorted tonal bed
under everything: it thickens through most of the travel and then, near the
top, tips over into a howl of its own. That howl is pitched, and the pitch
follows the time knob — about 170 Hz with the rate at the bottom, 1.7 kHz at
the top.

Unlike the other modes, the crusher's loop closes around the sample-and-hold
rather than through the module's global one-sample path, and it is AC-coupled
on the way round. Both are needed. A memoryless loop around a saturator has
no pitch in it — under unity gain it is only a gain, and over unity its fixed
point moves to a rail and stays there, which is silence with an offset on it.
The hold gives the loop a delay and the AC coupling leaves it nothing to
latch onto, so it oscillates instead, at a frequency the coupling sets and
the crush rate scales.

amount is the crush depth, not a dry/wet: past the first tenth of its travel
the decimated path is all you hear. That first tenth fades the decimated path
in; the bit depth then runs twelve bits down to three across the rest of the
bottom half, and the top half holds it there and folds in the mangling. Only
the time knob will give the signal back, and only with amount low enough to
leave the bit depth alone — around a quarter of the way up it is still nearly
nine bits, which is inaudible next to what the rate is doing.

Sixteen bits down to two, which is where the depth used to run, spent the
knob's first quarter between -100 and -60 dB of error, none of which you can
hear, then did the whole audible job in the second quarter and hit its floor
at the middle.

The mangling works on the quantizer's own level index rather than on a
fixed-width word, so it always has the bits the crusher left to scramble, and
it carries the sign outside the operation. It has to: the XOR of two negative
two's-complement numbers is positive, so run on the signed sample it flipped
every negative one and put the waveform entirely above zero.

### 6. slicer (light green)

The pattern generator, applied to the amplitude of the signal. **time**
chooses which of the 32 rhythms does the chopping — the same table the
**rhythm** knob reads — and each hit fires a decay envelope whose length is
**amount**. Left is a second-long decay that only breathes; right is 60 ms,
which turns a drone into a rhythm part. Short slices get some of their
loudness back automatically: chopping a drone into a rhythm should not also
turn the volume down. **feedback** adds a chance of any step
inverting, so the pattern keeps changing, and **stereo** gives the two
channels different rhythms.

### 7. pitcher (red)

Pitch shifting up by sweeping a delay tap with a ramp — crude on purpose,
with the transient duplication that goes with it. **time** is the window
size: long, and it chops rhythmically; short, and it turns into formant
shift. **amount** is how far the ramp sweeps, which is the shift interval,
and also the dry/wet. **trig** briefly stretches the window.

Both knobs scale the tap's position, and so does the trig's stretch, so a grain
takes its window and its shift when it starts and keeps them until it ends.
Changing any of them mid-grain would drag the tap under the playhead — a jump
if it were sudden, a chirp if it were smoothed — and at a grain boundary the
crossfade already has that grain at zero. The ramp restarting is the mode's own
crudeness; a knob or a trig clicking is not.

### 8. replayer (orange)

A tape loop. **time** is the speed of the tape and the sign of it: backwards
to the left of centre, forwards to the right, for recording and playback
alike. **amount** decides what the tape is: fully right locks the buffer and
you hear the loop as it stands, and as you go dry more new signal is recorded
over it until the old audio is gone. **feedback** applies to the incoming
signal only, not to the output. **trig** fills the whole tape with new audio
at once.

### 9. shifter (pink)

The other pitch shifter — a crossfaded pair of taps, which avoids the
pitcher's stuttering at the cost of its bite. **time** shifts down below the
centre and up above it, with unity in the middle. Feedback with a small shift
is where it earns its keep: each pass shifts again, so the tail walks away in
pitch. **stereo** gives the two channels different shifts, and a small
detune is a very wide unison.

## The filter

The **filter** knob is one control, but the nine modes are not built alike and
it cannot sit in the same place in all of them. Three of them produce a wet
signal distinct from the dry, so the filter goes on that; four process the whole
signal and have no separate wet, so the only place for it is the global feedback
loop; two have both.

| mode | on the effect's output | inside the feedback |
|------|:---:|:---:|
| delay, flanger, freezer | ● | |
| panner, crusher, slicer, replayer | | ● |
| pitcher, shifter | ● | ● |

What is the same everywhere: the dry signal is untouched, so at **amount** fully
left the filter does nothing at all, and at half you are mixing filtered effect
against unfiltered input.

Three context-menu settings change this.

- **Slope** — **12 dB/oct** is artifex's own: one state-variable section,
  gentle, and narrow enough in range that the ends of the travel leave the
  material audible. At the top of the highpass a 4 kHz component is down about
  4 dB and 10 kHz is untouched. **24 dB/oct** is the filter [vates](vates.md)
  carries: two sections, Butterworth-damped with the resonance in the second
  only, over a wider range of corners. The same 4 kHz tone goes down about
  55 dB, so the knob can take the material away at either end rather than
  merely thinning it. The shallow one is the default because it is the gentler
  tone control and that is what the effect path usually wants.
- **Filter the dry signal too** — moves the filter out of the mode and onto the
  module's output, where it catches dry and effect together, and the knob
  becomes a filter on everything you hear. Nothing is filtered twice: the mode's
  own filtering steps aside when this is on. It also puts the filter inside the
  global feedback loop for free, since that loop is taken after the output.
- **Filter inside the feedback** — the delay and the flanger keep their own
  feedback line and take it from *before* the filter, so each repeat is filtered
  once. Turn this on and the feedback goes through the filter as well, so every
  pass is filtered again and the tail darkens (or thins) as it decays. With a
  lowpass half-left and feedback at 0.8, repeats fall about 2.2 dB apart with
  this off and about 3.0 dB with it on. It is the difference between a delay
  with a tone control and a dub delay. The other seven modes already have the
  filter in their loop, so it does nothing there.

## The panel

### fx mode

A knob with nine positions, a display that names the mode, and a CV input
with its own attenuverter. Right-click the display to jump to a mode.

CV covers the whole list over ten volts, and wraps: eleven volts is mode one
again. That is the same rule bank and sample follow in vates, and it means an
LFO or the pattern CV sweeps the whole machine without your having to trim
it. Set the attenuverter to a small value and the same CV picks between two
neighbouring modes instead.

Mode changes from CV **wait for the next step of the clock**, as on the
hardware, so a modulated mode change lands on the beat instead of chopping a
sample in half. The knob and the display picker change modes immediately.
The waiting can be turned off in the context menu.

### time, feedback, amount

Each is a big knob with a modulation input and an attenuverter, laid out as a
triangle underneath it. **time** has two inputs instead of one:

- **free** modulates the time parameter continuously, as any CV input would.
- **step** is sampled and held on each step of the clock, so modulation
  arrives quantized to the beat. Patch the pattern **cv** output here for
  stepped time changes locked to the rhythm.

Both share the one attenuverter, and both add to the knob.

### filter and stereo

Two knobs, each with a plain CV input. The filter is bipolar around an open
centre; stereo runs from mono at zero to as far apart as the mode allows.

### in, out and gain

**L** and **R** inputs, with **L** normalled to **R** so a mono source fills
both. The **gain** trimpot beside them runs to +12 dB, and it matters more
here than on most modules: the feedback loop reacts to how hard you drive it.
The two lamps go red at 5 V, which is where the effect starts folding the
signal rather than where the rail is — so they light while you can still do
something about it.

**level** sets the output, and **env** is an envelope follower on the input —
patch it to the feedback or amount modulation input for ducking, which is
what the hardware suggests and what it is best at. It reads 10 V at that same
5 V clip point, so a lit lamp and a full-scale envelope mean the same thing,
and the follower keeps its range over the part of the gain knob you use.

### lfo

Triangle and pulse as on the hardware, plus a saw, and it is the same LFO
vates has, down to the **pwm** trimpot: it skews the triangle, and since the
pulse is high exactly while the triangle rises, that skew *is* the pulse's
duty cycle. The saw stays a plain phasor. The switch chooses sync or free: synced, the rate knob picks a
division of the clock and the phase is locked to the pattern, so the saw is a
usable bar phasor; free, it runs 0.01–20 Hz.

**reset** restarts it. **mod** is an attenuverted rate input. Patching the
pulse output into the mod input reshapes the triangle, exactly as the
hardware's patch-programming tips describe.

### clock and pattern

**tempo** sets the internal clock in BPM; a signal at **clk** takes over, and
the module returns to its own clock two seconds after that signal stops.
**clk** out passes the running clock on.

The pattern generator runs sixteen steps of gate and CV, always on the clock.
**rhythm** chooses one of 32 patterns — twenty-two written by hand and ten
euclidean distributions E(k, 16) — with a CV input that covers the whole list
over ten volts and wraps. The euclidean half skips six densities, because at
sixteen steps those *are* patterns the first half already holds: E(1) is the
downbeat alone, E(2) half notes, E(4) four on the floor, E(6) tresillo, E(8)
eighths, E(16) sixteenths.

The two three-position switches edit the pattern as it plays:

| switch | up | middle | down |
|--------|----|--------|------|
| **gate ptrn** | randomize this step | leave it alone | invert this step |
| **cv ptrn** | randomize this level | leave it alone | invert this level around 5 V |

Each switch is normalled to the input below it: patch a gate or CV there and
it takes over, above 3.2 V for up and below 1.6 V for down. **They are
pencils, not filters** — a switch writes into the pattern one step at a time,
so returning it to the middle does not restore what was there. To get the
untouched rhythm back, select it again with the rhythm knob.

**reset** returns both sequences to step one.

### the display

One, centred above everything else. The left half names the mode and its
number; the right half reads the time parameter in whatever unit the mode uses
— a delay time in milliseconds, a clock division when it is synced, a frequency
for the panner, a rhythm number for the slicer, an interval for the shifter.
Right-click anywhere on it to jump straight to a mode.

It is written in the mode's own colour, the one in each mode's heading above.
The hardware carries them on a single RGB LED, and they are the fastest way to
know what the box is doing without reading anything: with the mode under CV you
can see it change across the room.

## Context menu

- **Buffer** — 1.15 s (the hardware's), 2.5 s or 5 s. It sets the longest
  delay, the longest freeze and the length of the replayer's tape.
- **Mode changes wait for the clock** — on by default, as on the hardware.
- **Input** — stereo, or sum to mono (the hardware's advanced settings).
- **Pattern inputs read a Rack voltage window** — 0 V neutral, above +1 V
  randomize, below −1 V invert. Off, it uses the hardware's 1.6 V/3.2 V
  window instead.
- **Honour the external clock** — off, the module ignores **clk** and stays
  on its own tempo.
- **Feedback safety** — a soft limiter in the loop, on by default. Off, the
  loop can run away, which is a legitimate thing to want.
- **Slope**, **Filter the dry signal too**, **Filter inside the feedback** —
  see [the filter](#the-filter).

## What was left behind

MIDI, in both its forms: the TRS jack and the USB port, with the note-per-mode
mapping, the CC table and the clock priority rules. Rack has its own MIDI
modules and its own clock, and none of that would have been the module.

The headphone output, the input jack detection, tap tempo (the tempo is a
knob you can see), the advanced settings mode, the memory reset and the test
mode — all of them exist to work around a panel with two buttons.

Kept, because nothing else in Rack does it quite this way: nine effects that
share one feedback loop, three knobs that change meaning without changing
their character, and a pattern generator wired into an effect rather than
into a voice.
