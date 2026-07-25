# quadrare

**A Walsh–Hadamard codec with the transform domain brought out on jacks.**

*quadrare* is Latin for *to square, to make fit*. Walsh functions are square
waves, and the module squares your audio into them.

It cuts the incoming signal into consecutive blocks, runs a fast
Walsh–Hadamard transform on each one, hands you the lowest sixteen
coefficients on sixteen sliders and thirty-two jacks, and transforms whatever
comes back into audio again. Touch nothing and the round trip is transparent
to float precision: what goes in comes out, and **res** sits at zero.
Everything the module does, you do in the coefficient domain.

It is not a spectral analyzer. A Walsh coefficient describes the *sign
structure* of a short block, not a stable frequency band, and the
reconstruction is built from square waves rather than sinusoids. Expect
blockiness and grit, not a smooth EQ.

## The sliders are bipolar: down is not off

This trips everyone up once, so it is worth saying first.

Each slider runs from **−1 at the bottom, through 0 at the center, to +1 at
the top**, and it starts at the top.

| slider position | gain | what you hear |
|---|---|---|
| top | +1 | the coefficient, unchanged |
| upper middle | +0.5 | half level |
| **center** | **0** | **the coefficient removed** |
| lower middle | −0.5 | half level, phase inverted |
| bottom | −1 | full level, phase inverted |

Sliding everything to the **bottom** does not mute the module. It inverts the
signal, and an inverted signal sounds exactly like the original. Output level
against slider position is a V, not a ramp:

```
 out
100% |*                             *
     |  *                        *
 50% |     *                  *
     |        *            *
  0% |___________*______*____________
     -1        -0.5    0    +0.5    +1
```

To remove a coefficient, put its slider in the **center**, or press the
**zero button** under its column, which lands there exactly.

One control does attenuation, muting and inversion, which is why there are no
separate mute and invert buttons. The cost is that the off position is in the
middle of the travel rather than at the end, which is what the buttons are for.

The light in each slider handle shows that coefficient: green when positive,
red when negative, brighter as it grows. Its sign changes with the audio from
block to block, so the light tells you how much energy is there, not whether
you have inverted it.

## The zero buttons

The row of buttons under the columns exists because the useful position on a
bipolar slider is the one in the middle, which is the hardest to hit by hand.
Each button steps its own slider around three landmarks:

```
press once   ->   0     removed
press again  ->  -1     full level, inverted
press again  ->  +1     back to unity
```

From any partial position the first press lands on **0**, so a button is
always one press from silence whatever the slider was doing. Nothing else
moves: a button only ever touches its own column.

Holding a chord and punching coefficients in and out is the fastest way to
hear what each one is contributing.

## What the sliders reach: the window

Coefficient *k* sits at

```
f = k · fs/2n        n = size, so 93.8 Hz apart at n = 256 and 48 kHz
```

The sixteen sliders own coefficients 0 to 15, so they cover **0 to 8·fs/n**.
At the default size of 256 that is **0 to 1500 Hz at 93.8 Hz per slider**:
fine control over the bass and low mids. Everything above the window is
handled as one lump by the **above** switch.

**size** chooses how far you zoom in:

| size | per slider | window | latency |
|---|---|---|---|
| 16 | 1500 Hz | 0 – 24 kHz | 0.67 ms |
| 32 | 750 Hz | 0 – 12 kHz | 1.3 ms |
| 64 | 375 Hz | 0 – 6 kHz | 2.7 ms |
| 128 | 187.5 Hz | 0 – 3 kHz | 5.3 ms |
| **256** | **93.8 Hz** | **0 – 1.5 kHz** | **10.7 ms** |
| 512 | 46.9 Hz | 0 – 750 Hz | 21.3 ms |

At **16** the window is the whole spectrum, sixteen coefficients wide: the
sliders are literally the entire transform, **above** does nothing, and the
module is at its coarsest and grittiest. Every step down zooms further into
the low end, halves the block rate, and doubles the latency.

Selectivity is soft. Muting one slider drops a sine at that slider's own
frequency by 2 to 3 dB, and its neighbours by around 1 dB. A sine is not a
Walsh function, so its energy spreads across many coefficients; you are
sculpting a basis that does not line up with pitch. That smearing is the
instrument, not a shortcoming.

## Controls

| control | function |
|---------|----------|
| **0 – 15** | sixteen bipolar coefficient gains, −1 to +1, default +1. See above |
| **zero** | one button per column, cycling that slider **+1 → 0 → −1 → +1** |
| **above** | switch, in the bottom row: **pass** everything outside the window through, or **mute** it |
| **size** | 16 to 512 samples per block, zooming the window as tabulated |
| **keep** | retain only the *k* largest-magnitude coefficients, from all of them down to one |
| **quant** | coarsen every coefficient to a grid, from off to 2 levels |
| **level** | output gain, 0 to 2, unity at noon |
| **dry/wet** | crossfade between the latency-matched dry signal and the reconstruction |

| jack | |
|------|--|
| **in** | audio in |
| **coeff out** | sixteen mono outputs, one per slider, carrying its coefficient |
| **coeff in** | sixteen mono inputs, the matching returns |
| **comp** | polyphonic, sixteen channels: each coefficient's own contribution as audio |
| **res** | residual, the delayed dry minus the reconstruction |
| **out** | audio out |

## above

The sliders reach sixteen coefficients. At size 256 the other 240 carry
everything from 1500 Hz up, and this switch decides what happens to them.

- **pass**: they go through untouched. The module reshapes the low window
  and leaves the rest of your signal alone. This is the default and the
  normal setting: put every slider in the center and you are left with a
  highpass at the window edge.
- **mute**: they are zeroed, so the module becomes a lowpass at the window
  edge with sixteen Walsh sliders inside it. Rejection is around 22 dB, not
  clean: Walsh functions are not sinusoids and a high sine leaves traces in
  low-sequency coefficients.

At size 16 there is nothing outside the window and the switch does nothing.

## keep and quant

These are the lossy stages of a transform codec, and they are what makes
*quadrare* a codec rather than a filter bank. Both act on **every**
coefficient, not just the sixteen in the window, so they still bite when
**above** is set to pass.

**keep** is rank thresholding: it keeps the *k* biggest coefficients in each
block and throws the rest away. This is how transform compression actually
works. Wind it down and the signal collapses onto its strongest Walsh
components, shedding detail in a way that sounds nothing like filtering:
sparse, blocky, oddly synthetic. It is scale-invariant, so it behaves the
same however hard you drive the input.

**quant** coarsens every coefficient onto a grid before reconstruction. The
grid is scaled to each block's peak coefficient, so this too is independent
of input level. Walsh-domain bit reduction does not sound like PCM
bitcrushing: the error spreads across the whole block as square-wave
structure rather than arriving as per-sample hash.

They stack. **keep** low with **quant** high is the module at its most
destroyed.

## The coefficient jacks

This is the point of the module. Each column's **coeff out** carries its
coefficient, and **coeff in** takes it back. Every jack is **mono**, at every
size, so there is no polyphony to reason about.

Patch a **coeff out** straight into its **coeff in** and nothing changes: the
insert is exact, to float precision. Put anything in between (a slew, a
sample and hold, a waveshaper, an offset, another quadrare) and you are
processing the transform domain directly.

The **context menu** decides what happens to the coefficients you *don't*
drive, once you drive at least one:

- **Overlay** (default): coefficients you drive come from outside, the rest
  stay internal. Best for partial patching.
- **Replace**: coefficients you drive come from outside, the rest go to zero.
  Turns the inverse transform into a decoder for whatever you feed it.

Coefficients outside the window follow the **above** switch in both modes.
Slider gains apply *before* the substitution, so a coefficient you drive from
outside is not multiplied by its slider: **coeff in** is a true insert return,
not a VCA.

### As a synthesizer

Nothing requires audio at **in**. With **Replace** selected and nothing
patched to the input, drive the **coeff in** jacks and the inverse transform
becomes a Walsh oscillator: the sixteen voltages you feed it are the waveform,
rebuilt as stacked square waves and refreshed every block.

## Latency

The module analyzes a block at one boundary and reconstructs it at the next,
so it runs **2·size samples** behind, from 0.67 ms to 21.3 ms.

The extra block is what makes the coefficient jacks exact. Rack copies cable
voltages once per frame and steps modules in arbitrary order, so a **coeff
out** patched back to **coeff in** is a feedback cable and cannot return a
block's coefficients within that same block. Waiting a full block guarantees
the values coming back match the vector they came from. The dry path is
delayed to match, whether or not anything is patched, so **dry/wet** always
lines up and **res** always nulls.

Changing **size** flushes the pipeline: expect a click and up to one block of
silence. It is a control to set, not to sweep.

## res and comp

**res** is the delayed dry minus the reconstruction: what the module threw
away. With everything at default it sits at zero. Remove a coefficient and its
content appears there. It is a complement, not a difference signal: patch it
alongside **out** and you have the signal split in two.

**comp** breaks the output into the sixteen exposed coefficients'
contributions as *audio*, one per polyphonic channel. Each is a single Walsh
function scaled by its coefficient, so each channel is a square wave whose
amplitude follows the analysis. With **above** muted they sum exactly to the
wet signal; with **above** passing they sum to the windowed part of it. Feed
it to pavo for a spread, or take individual channels for per-band routing.

## Tips

- Leave **size** at 256 to start. 16 is the most extreme setting, not the
  neutral one.
- For a first patch, put every slider in the center with **above** on pass.
  You get a highpass; now raise sliders one at a time and hear each 94 Hz
  slice come back.
- **above** on mute turns the module into a Walsh lowpass you can play from
  the inside. Sweeping **size** moves its corner, if you can live with the
  click.
- **keep** at its minimum with **dry/wet** part way back is a good ambient
  texture: the strongest Walsh components ghosting under the dry signal.
- Patch **coeff out** of one column through a slew limiter into its **coeff
  in**. That coefficient can no longer jump between blocks, which smears it in
  time in a way no filter does.
- Cross-patch two quadrares at the same size: take **coeff out** from one into
  the other's **coeff in** and each coefficient gets the other signal's value.

## Design notes

Why the module is shaped the way it is, including the roads not taken. None
of this is needed to play it.

**The sliders own the lowest sixteen coefficients, not sixteen bands.** The
first design gave each slider a band covering a slice of the whole spectrum,
and it was unplayable in a way no tuning could fix. Coefficient *k* sits at
`k·fs/2n`, so sixteen equal bands always cut 0–24 kHz into 1500 Hz slices
*whatever the size*: slider 0 permanently owned everything below 1500 Hz,
which is where most music lives, and larger sizes only subdivided inside each
band. Quadratic bands would have distributed them better but forced every
jack to a different polyphonic width, which is invisible from the panel.
Owning the low sixteen outright fixes both at once: sixteen times the low-end
resolution at size 256, and every jack mono at every size, so the polyphony
problem stops existing rather than being traded against. The cost is that the
sliders no longer reach above the window, which is what **above** is for.

**Bipolar sliders with zero buttons, rather than a detent.** One control
doing attenuation, muting and inversion is worth the awkwardness of putting
the useful position in the middle of the travel. A center detent was
considered and rejected: it fights you when you want a value *near* zero. The
buttons make the middle a single press instead, and can reach ±1 as well,
which a detent cannot.

**The extra block of latency is the price of an exact insert.** Analyzing at
one boundary and reconstructing at the next costs 2·size instead of size, and
the alternative was not free: reading **coeff in** at the same boundary that
wrote **coeff out** picks up a vector one block stale, since Rack copies cable
voltages once per frame. A plain patch cable would then quietly stop being an
identity, the wet path would run a block behind the dry, **res** would never
null and **dry/wet** would mix misaligned copies. Waiting the extra block is
what makes those three properties hold.

**quant's grid is relative to each block's peak**, which keeps it
scale-invariant like **keep** and means neither stage cares how hard you drive
the input. An absolute grid was the other option, and it would have made input
level musically meaningful again, but only by reintroducing a drive control
that a purely linear path had made redundant.

**Modifying coefficients changes gain at the block rate**, which generates
sidebands at multiples of that rate, and they fold back. The aliasing is the
sound, not a defect: at size 16 the block rate is 3 kHz and the module is a
grit box, while at 512 it is 94 Hz and behaves far more like a spectral
filter. This is why size feels like a character control rather than a quality
setting.

**freeze was cut** late, after play-testing. Holding the coefficient vector
and rebuilding from it produces exactly one periodic waveform at the block
rate, so it was a static tone with nowhere to go: a 3 kHz whistle at size 16,
a 94 Hz drone at 512, and nothing in between worth reaching for. Its panel
slot went to **above**, which earns it.

**comp survived a similar cull.** Sixteen channels of polyphony is a lot of
jack for something the **coeff out** row arguably already exposes, but it
answers a different question: what a coefficient *sounds* like, rather than
what its value is.
