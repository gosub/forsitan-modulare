# lustro

**Scanned filter: scando's vibrating string playing a filterbank instead of
a wavetable.**

*lustro* is Latin for "I traverse, I survey". It reuses scando's
mass-spring string engine, but instead of scanning the string's shape into
an oscillator wavetable, the string becomes the moving control surface of a
resonant filterbank: sixteen bandpass filters process the input audio, and
each band's gain is the displacement of the string at one of sixteen points
along its length. The physics *plays the EQ*.

Pluck the string (**excite**) and the spectrum blooms, ripples along the
band stack as the wave travels the string, and decays with the physics.
Drive the string continuously (**strength**) and the filterbank breathes
forever. Displacement is bipolar, so a band can invert the phase of its
slice, adding hollow comb-like movement on wideband material.

At rest the string is flat and the wet signal is silent: the module only
speaks when the physics moves.

## Controls

### String physics (as scando)

| control | function |
|---------|----------|
| **stiff** | inter-mass spring stiffness: how fast ripples travel along the string |
| **damp** (+CV) | high: motion dies quickly (plucks become short spectral gestures); low: rings long, slightly self-oscillating at the very bottom |
| **rate** (+CV) | physics update rate, 500 Hz–8 kHz: how fast the spectrum evolves |
| **shape** | hammer shape (sine → saw → noise → dual pulse): the spectral *pattern* a pluck imposes |
| **strength** | continuous hammer drive: keeps the string (and so the filter) moving without triggers |
| **excite** | trigger input: hammer the string to the current shape |

### Filterbank

| control | function |
|---------|----------|
| **base** (+CV) | lowest band, 40 Hz–2 kHz |
| **spread** (+CV) | how many octaves the 16 bands cover above base, 1–6 |
| **res** | filter resonance (Q 2–40). High values turn plucks into ringing spectral chimes |
| **mix** | dry/wet |

## Tips

- Noise or a dense pad in, **excite** from a clock: rhythmic spectral
  plucks, each one decaying by physics rather than by envelope.
- **shape** toward noise + high **res** + a pluck: sixteen bands lit in a
  random pattern that untangles itself as the string settles.
- Narrow **spread** (1–2 octaves) around a mid **base** gives formant-ish
  vowel movement; wide spread is a full-spectrum animator.
- CV **base** with a slow LFO to slide the whole living comb up and down.
