# gradus

![gradus](../img/gradus.png)

**Eight steps into one CV. Every row is one knob and four moves: up by it,
down by it, straight to it, straight to minus it.**

*gradus* is Latin for a step, a stair, a rank. It is a discrete
[cumuli](cumuli.md): where cumuli ramps for as long as a gate is held open,
gradus moves only on an edge, and only by the amount dialled in. The output
walks a lattice of values you chose rather than sliding between them.

## The panel

Eight rows, numbered 1 at the top to 8 at the bottom. Every row is the same
five things, left to right:

| control | what it is |
|---------|-----------|
| **knob** | the row's value, 0V to 10V. A step size under **add**, a target under **jump**. |
| **switch** | **add** (up) or **jump** (down). |
| **+ button, + jack** | fire the row with the value as it stands. |
| **- button, - jack** | fire the row with the value negated. |

Each side's button and its jack are the same event: the button is there so
a row can be fired by hand, and the columns are marked **+** and **-** at
the top of the panel.

That makes one knob four moves:

| | **add** | **jump** |
|---|---------|----------|
| **+** | output += knob | output = +knob |
| **-** | output -= knob | output = -knob |

So a knob at 1V gives you up a volt, down a volt, snap to +1V and snap to
-1V, and the mode switch picks which pair of the four is live.

The knobs are square-law rather than linear: both stops are exact, 0V and
10V, but the sweep between them is stretched at the bottom where the useful
values live. A volt sits about a third of the way round and a tenth of a
volt a tenth of the way, instead of at 10% and 1% of a linear sweep, where a
semitone would be almost impossible to dial. The tooltip always reads the
volts, and typing a value in still works.

Out of the box the rows are a ladder from fine to coarse, 0.05V at the top
through 0.1, 0.25, 0.5, 1, 2 and 3 to 5V at the bottom, every switch on
**add**. That makes a fresh gradus a general-purpose nudger: eight sizes of
step, up or down, on any of sixteen buttons.

Below the rows, three things: **clip** sets the output's range, **reset**
sends the output to 0V from its own button or trigger input, and **out**
carries the running value, with a lamp that reads green above zero and red
below.

Reset is there so that going home does not cost a row. A row in jump mode
with its knob at zero would do the same job, but it would be a whole row
spent on the one value the module can always be asked for.

## What a trigger does

Triggers are edges, not levels. A gate held high is one step, not a stream
of them; to step again, the input has to fall and rise again. The threshold
is the usual 1V, with hysteresis down to 0.1V. A held button behaves the
same way, one step per press.

The output does not wrap at the ends of its range. A step that would
overshoot lands on the rail instead, so eight adds of 3V from zero leave
the output at 10V, not at 4V.

## When several triggers arrive together

Sides and rows that fire in the same sample are resolved by four rules, and
they are the reason gradus is a module rather than sixteen adders in a
row.

**A jump beats every add in that sample.** The relative moves are
discarded, not applied around it. A row in jump mode names an absolute
value, and that is where the output goes.

**Among simultaneous jumps, the last in reading order wins.** The rows are
read top to bottom and each row's plus side before its minus, and the last
jump seen is the one that lands. So the panel is a priority order: row 8
overrides row 5, which overrides row 1, and a row's own minus side
overrides its plus. Put the jump you want to be able to force at the
bottom.

**With no jump in the sample, every add is applied, signs and all.** Fire
rows 2, 3 and 7 together and the output moves by their sum, with each row
contributing plus or minus its knob depending on which side fired. Both
sides of one row at once in add mode cancel exactly, and the output holds.

**Reset beats all of it.** It sits below the rows, so reading order puts it
last of all: a reset landing in the same sample as any jump or any add
sends the output to 0V regardless. Held high it is still one event, so the
rows keep working underneath a reset gate that never falls.

"The same sample" is exactly that: one audio frame. Triggers from a common
clock divider or a logic module land together and are resolved by these
rules; triggers a millisecond apart are two separate events, and a jump
then simply overwrites what the adds just did.

## clip

The **clip** knob sets what range the output is held to:

| position | range |
|----------|-------|
| **0 to 10 V** | unipolar, the usual range for a modulation CV |
| **±5 V** | bipolar, the usual range for an audio-rate or panning CV |
| **±10 V** | bipolar, the full reach of the knobs (the default) |
| **no clip** | none at all: the value goes wherever the adds take it |

At ±10V every jump target is reachable, which is why it is the default: the
knob says 7 and the output goes to 7 or to -7, with no arithmetic in
between. Narrower settings still clip, so a jump to 7V under ±5V lands on
5V.

**no clip** is there for using gradus as a plain accumulator whose numbers
matter more than its voltage, feeding something that scales or wraps
downstream. It has no rails at all, so a long run of adds in one direction
will walk out of any range Rack cares about.

Changing the setting pulls the current value in immediately if the new
range is narrower. The output value is saved with the patch, and so, being
a knob, is the clip setting.

## Factory presets

Six, from the right-click menu, each one a use case rather than a variation
on a sound:

| preset | what it is |
|--------|-----------|
| **scale** | Eight jump rows on the degrees of a major scale, root to octave. A row per degree, and each row's minus side is the same degree below the root, so eight rows are sixteen notes. Trigger-addressed melody: patch eight gate outputs and the pattern picks the notes. |
| **arpeggio** | The same idea on chord tones: a minor 7th spread over two octaves. Retriggered patterns come out as arpeggios that never land off the chord. |
| **transpose** | Relative intervals on the top three rows (octave, fifth, semitone) and absolute jumps to one through five octaves below them. Sum a V/oct line with the output and the buttons transpose it, by ear or by trigger. |
| **binary** | Eight add rows of 0.039V doubling to 5.02V: powers of two that sum to exactly 10V. Fire any subset of the plus triggers **in the same sample** and the output is that eight-bit number. A shift register, a Turing machine's expander or a logic patch becomes a stepped voltage. Reset before each word. |
| **drift** | Six small add rows and two jump rails, clipped to ±5V. Random triggers wander the output; the rails snap it back when it strays. |
| **fader** | Three step sizes and five absolute levels, clipped to 0-10V. gradus played by hand, which is the module [cumuli](cumuli.md) was written for: a fader for a controller that has only buttons. |

## How to use

**A stepped voltage source.** Set a row to add with its knob at 1/12 V
(0.083) and its two sides are a semitone up and a semitone down; at 1V, an
octave either way. Two rows of different sizes make a drunk walk, and a
jump row snaps it home when the walk gets lost.

**A trigger-addressed sequence.** Eight rows in jump mode, sixteen
triggers from a sequencer's gate outputs, and gradus is a sixteen-value
lookup table addressed by whichever trigger fires: eight magnitudes, each
available positive or negative. The precedence rule decides what happens
when two fire at once, which is where the character is: a run of
overlapping gates plays the last row in reading order, not a mixture.

**An accumulator with presets.** Mix the modes. Rows 1 to 6 add various
amounts up and down, rows 7 and 8 jump to a floor and a ceiling, and reset
holds zero. The patch wanders, and one trigger puts it back on a known
value in a single sample, without waiting for a ramp.

**Rhythm into pitch.** Feed the trig inputs from a drum pattern's outputs,
kicks to the plus side and snares to the minus. The CV output then tracks
the pattern's *history* rather than its beat, and because the steps are
exact the pitches repeat exactly whenever the pattern does.

**By hand.** The buttons make gradus playable on its own: eight rows of two
buttons is a small instrument for pushing a drone's pitch around, or for
auditioning where a patch goes before wiring the triggers up.

**A staircase oscillator.** Drive the eight plus inputs from eight
phase-shifted clocks at audio rate, one per row, each firing a little after
the last: a shift register clocked fast, a phase-offset LFO bank squared up,
or eight outputs of a clock divider. gradus stops being a controller and
becomes an oscillator.

With every row on **jump** the eight rows are eight sample points and the
output is a waveform the knobs draw, one cycle per turn of the clocks, at
the pitch the clocks are running. Move a knob and you are drawing the wave
while it sounds. With every row on **add** and a trigger into **reset** at
the start of each cycle, the same eight clocks integrate into a staircase
ramp whose slope is whatever the knobs say, which is a sawtooth you can bend
into any monotone shape.

It aliases, as any stepped source does at audio rate, and that is part of
the sound. Eight steps is a coarse wave; the clip setting is what keeps it
from running away in add mode if the reset ever misses.
