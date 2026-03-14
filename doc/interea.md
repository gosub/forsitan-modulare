# interea

![interea](../img/interea.png)

*interea* transforms a V/Oct signal into a chord. It has four chord qualities (Maj7, Min7, Dom7, Half Dim), four inversions (root, first, second, third) and four voicings (close, drop 2, drop 3, spread). When the *harmonize* button is pressed, the frequency input is treated as a bassline and the chord is harmonized with the major scale.

Inspired by [Strum's Mental Chord](https://github.com/Strum/Strums_Mental_VCV_Modules/wiki/Chord) and the physical module [Chord v1 by Qu-Bit Electronix](https://www.modulargrid.net/e/qu-bit-electronix-chord).

## How to use

Connect the four outputs to the V/Oct input of four oscillators. You should now be listening to the classic C4 Major7 chord. Play with the *frequency*, *quality*, *inversion* and *voicing* knobs and inputs to play different chords.

- The *frequency* input is bipolar (±5V)
- The *quality*, *inversion* and *voicing* inputs are unipolar (0–10V)

When the *harmonize* button is on, the chord *quality* is chosen automatically from the Major scale and the *quality* knob and input are disabled. The notes of the chord will follow diatonic and modal interchange chords of the Major scale. See the [Qu-Bit Chord manual](https://www.qubitelectronix.com/s/Chord_Manual.pdf) for additional explanation.

## Bypass behaviour

The root pitch input is copied to all four chord outputs (root, 3rd, 5th, 7th).
