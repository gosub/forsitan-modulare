# guttur

**Chaotic resonator drone: a Duffing oscillator screaming through 48
resonant filters.**

*guttur* is Latin for "throat" — the guttural voice — and puns on the
instrument it ports: **Gutter Synthesis**, Tom Mudd's chaotic
physical-modelling algorithm
([tommmmudd/guttersynthesis](https://github.com/tommmmudd/guttersynthesis),
GPL-3.0), also drawing on the SuperCollider port by Mads Kjeldgaard and
Scott Carver
([madskjeldgaard/guttersynth-sc](https://github.com/madskjeldgaard/guttersynth-sc)).

A forced, damped **Duffing oscillator** — a classic chaotic system — is
coupled to two banks of 24 resonant bandpass filters. The filters are not
an effect: their summed output *is* the forcing term fed back into the
Duffing equation, so the oscillator and the resonators form one coupled
feedback system.

```
duffX ──> [bank A: 24 bandpass] ──┬─> finalY ──> out (×0.125)
      ──> [bank B: 24 bandpass] ──┘     │
   ^                                    v
   └── distortion <── Duffing step <── sine forcing (or audio in)
```

The result is metallic, physical, scraping — bowed sheet metal, dragged
chains, feedback trombones — sound that seems to *touch* surfaces as it
moves. The internal forcing sine runs at **tone** × **rate** (the two
knobs multiply into one frequency); at the default ~700 Hz the engine
runs continuously. Turning **tone** or **rate** right down takes the
forcing below audio rate, and the system starts to **breathe** instead:
it surges into screaming resonance and collapses back between surges.
Below ~10 Hz the collapses become long silences, which is a setting, not
a fault.

At power-on (and on every **reset**) the chaos is ignited by a short
parameter kick — the same transient the SuperCollider version produces on
its first block — so the module always wakes up making sound.

## Chaos controls (big knobs, CV + attenuverter)

| knob | function |
|------|----------|
| **drive** | forcing amount (gamma, 0–10). More drive, more violence |
| **tone** | forcing sine frequency coefficient (exponential). Low = slow surging drones, high = growls and tones |
| **damp** | damping (exponential). High damping chokes the chaos; low lets it ring |
| **rate** | internal time step (dt, 0–10). Scales how fast the whole system runs; multiplies with **tone** to set the forcing frequency |
| **smooth** | chaos-state lowpass (0–5). Softens duffX; higher = darker, rounder (no CV) |

## Resonator controls

| knob | function |
|------|----------|
| **bank a / bank b** | factory bank per filter bank, 1–20: Tom Mudd's original preset frequency sets (measured metals, inharmonic clusters, near-unison swarms). Switching **glides** every filter to the new set — a playable morph (CV: 0–10 V spans the 20 banks) |
| **pitch** | global frequency multiplier 0.05–2, CV is 1 V/oct |
| **q** | master resonance for all 48 filters, 2.5–800 (exponential) |
| **spread** | per-filter random detune/Q scatter. 0 = exact presets; ~0.3 gives bank B the ±5 % shimmer of the SC example patch. The random pattern is seeded per instance, saved with the patch, re-rolled by Rack's Randomize |
| **gain a / gain b** | bank balance, 0–2 (CV-able) |
| **level** | drive into the filter sum (0–3.5). This is *inside* the feedback loop: it changes behavior, not just loudness |
| **dist** | distortion of the chaotic state: hard clip / soft clip / atan / atan-approx / tanh-approx. Shapes the *feedback*, so it changes the character of the chaos rather than adding fuzz |
| **filters** | switch the banks off for raw-Duffing mode: bounded chaos with hard resets — "snazzy clicks" |
| **reset** | button + trigger: zero the chaotic state and re-ignite. Percussive |

## Inputs and outputs

| jack | function |
|------|----------|
| **cv rows** | drive/tone/damp/rate (with attenuverters), bank a/b, pitch (1 V/oct), q, gain a/b |
| **in** | external audio replaces the internal sine as the Duffing forcing (auto-engages when patched): drums become chaotic resonance, voices become metal |
| **reset** | trigger input, same as the button |
| **out** | the filter-bank sum, DC-blocked, ≈ ±5 V (hard-limited ±10 V) |
| **duff** | the raw chaotic state (post-distortion), clamped and DC-blocked — a free chaotic modulation/aux source |

## Context menu

- **Distortion oversampling**: 1× to 16× (default 2×), anti-aliases the
  distortion stage.
- **Bank glide**: 20 ms / 100 ms / 500 ms / 2 s — how long bank morphs,
  pitch and q changes take to slew. Long glides make bank switching an
  instrument of its own.

## Tips

- Patch nothing and turn **q** up slowly: the same settings pass through
  drones, screams, and silence — the system is chaotic, not broken.
  If it goes quiet, hit **reset**.
- **tone** just above its default gives slow surge/collapse cycles;
  a square LFO into **bank a**'s CV rides the glide as a morphing sequence.
- Feed drums into **in** with drive around 2: gated metallic resonance.
  The input replaces the sine, so the module is silent between hits
  (except for the chaos ringing out).
- **duff** into a filter cutoff elsewhere closes the loop: the chaos
  modulates what it excites.
- Randomize (right-click) also re-rolls the **spread** scatter — with
  spread up, each randomize is a new instrument.

## Faithfulness notes

The per-sample loop is a line-by-line port of the SuperCollider version
(which follows the Java), including its historic quirks — the biquad
normalization indexes Q per *filter* while the gain terms index it per
*bank*, the Q array is shared between banks, the chaos "lowpass" is
really a differencing step, and the output taps the filter sum
*before* the distortion. These quirks are the sound and are kept.

Two deliberate deviations: filter tuning uses the Java's exact
`tan(π·f/Fs)` (the SC port feeds `π·f/Fs` into an approximation that
already multiplies by π, mistuning every filter a factor of π upward),
and the atan-approx distortion guards against its 0/0 at exactly zero
input (which could latch the module silent). The chaotic step itself is
sample-rate dependent (forward Euler, as in both originals); the forcing
clock is scaled so **tone** is sample-rate invariant, but texture will
still vary slightly with the engine sample rate.

## Attribution

Gutter Synthesis by Tom Mudd (GPL-3.0). SuperCollider/C++ port by Mads
Kjeldgaard and Scott Carver (GPL-3.0), whose oversampling classes are by
Jatin Chowdhury (ChowDSP-VCV). The 20 factory banks are the preset
frequency sets from Tom Mudd's original Max patch. This module is an
independent port, not affiliated with the authors.
