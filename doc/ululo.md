# ululo

**Feedback guitar: an electric guitar held up to a screaming amplifier.**

*ululo* is Latin for "I howl". The signal topology follows Nathaniel Virgo's
SuperCollider [Guitar feedback emulation](https://sccode.org/1-U),
reimplemented from scratch: the amplifier's output travels through the air
(a short delay), excites six comb-filter "strings", and the ringing string
sum passes through tone filters and a saturating amp stage whose output
closes the loop. Above a certain **gain** the system takes off on its own
and howls; the pitch it howls at is decided by the strings, the amp
distance, and the tone control fighting each other.

With nothing patched in, just turn **gain** up past 12 o'clock and wait a
moment (a faint internal noise floor seeds the loop). Everything else is
sculpting.

## Controls

| control | function |
|---------|----------|
| **gain** | feedback amount (0–200%). The instrument's throttle: below ~100% fed sounds decay, above it the loop self-oscillates |
| **dist** | distance between guitar and amp: the feedback path delay, 5–100 ms. Changes which harmonics win |
| **decay** | string sustain (comb feedback). High values ring long and favor harmonic screaming |
| **tone** | amp lowpass, 400 Hz–10 kHz. Darker settings tame the shriek into a moan |
| **drive** | amp saturation. Also the loop's limiter, so more drive = more compression and grit |
| **whammy** | bends all strings down, up to one octave. Sweep it while howling |
| **in lvl** | level of the **in** jack fed into the strings |

## Inputs and outputs

| jack | function |
|------|----------|
| **gain** (CV) | ±5V added to the gain knob (0.2/V) |
| **v/oct** (poly) | retunes the six strings. Unpatched: standard tuning E2 A2 D3 G3 B3 E4. With a polyphonic chord (e.g. from interea), strings take the chord's notes; if the chord has fewer than six notes the remaining strings repeat it an octave up |
| **wham** (CV) | 0–10V added to the whammy knob |
| **in** | external audio into the strings (guitar, drums, anything). Set **in lvl** up and **gain** low for a resonator; both up for chaos |
| **out** | mono output. The signal is bounded by the amp stage, roughly ±5V |

## Tips

- Patch interea → v/oct and play chords on pure feedback; change chords and
  the howl re-converges to the nearest strong harmonic.
- **dist** is a hidden pitch control: at high gain, sweeping it makes the
  system jump between modes like real amp feedback does.
- As a processor (in lvl up, gain ~50%): sympathetic-string shimmer on
  anything percussive.
- Everything in the loop is deterministic; the same knobs re-converge to the
  same scream.

## Attribution

Topology after Nathaniel Virgo's "Guitar feedback emulation" (sccode.org);
original implementation.
