# rubigo audition

Rebuild and install before a session, then restart Rack. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py rubigo 1.1
python3 tools/audition/audition.py rubigo --list
```

---

## 0. The bench

rubigo is a percussion voice (oscillator and noise, a driven filter,
corrosion or rust, a clipping VCA, three decay envelopes) played by two
random generators: SKIPS decides which steps fire, STEP MOD what each step
carries. STEPS locks what was heard into a loop.

Two kinds of item: plain ones are tests, **Decide -** ones are open questions
of taste resting on numbers the harness prints.

Anything that yields a number is not here. `./test/smoke_rubigo` checks the
defaults as a running kick, which way up the switches read, the button, the
jacks, NaN, other sample rates, the loop surviving a save, the reasoned
random and every preset. `./test/rubigo_probe seq` checks the STEPS lock,
`clock` the ratios and the return, `voice` the decay times, PITCH, the
self-oscillation and the RUST rate, `random` the reasoned random against a
uniform draw.

```python
r = vcv.module("rubigo")
out = vcv.module("Audio 2")
scope = vcv.module("Scope")
r["audio"] >> out["output 1"] + out["output 2"]
r["audio"] >> scope["ch 1"]
r["stepmod"] >> scope["ch 2"]
```

---

## 1. The voice

- [x] 1.1. Defaults: a square kick, about two a second, every step firing.
      The trigger button flashes yellow on each hit, the light by TEMPO too.
- [x] 1.2. Saw instead: the same kick, brighter and buzzier at the start.
      `r.set(wave="Saw")`
- [x] 1.3. The manual's click: a short tick at the front of each kick,
      sharper with more resonance.
      `r.set(cutoff=0.25, res=0.5, cutoff_decay=0.02, cutoff_amount=1)`
- [x] 1.4. RUST at 0.9 on a high saw: harsh lo-fi, whistling aliases that
      move with the pitch. This is not a fault, it is the effect.
      ```python
      r.set(wave="Saw", pitch=0.7, pitch_amount=0, cutoff=1,
            rust=1, effect=0.9)
      ```
- [x] 1.5. The drone: TEMPO and the volume decay full, no skips. A steady
      tone that never dies away; the filter still moves step by step.
      ```python
      r.set(tempo=1, volume_decay=1, stepmod=0.5, dest="Cutoff", res=0.5)
      ```

## 2. The sequencer

```python
r.set(pitch_amount=0.7, stepmod=1, dest="Noise", cutoff=0.55)
```

- [x] 2.1. STEP MOD full on NOISE: kick and noise hits in a changing mix,
      never the same twice.
- [x] 2.2. STEPS to 16: a 16-step pattern that repeats exactly. Down to 8
      and back to 16: the first half stays, the second half is new.
      `r.set(steps=5)   # 16 steps`
- [x] 2.3. SKIPS at 0.5 on the 16 loop: gaps appear in fixed places. Nudge
      SKIPS slowly: hits come and go one at a time, the rest stay put.
      `r.set(steps=5, skips=0.5)`
- [x] 2.4. The resonant melody: a pitched line played by the filter alone,
      looping every 16 steps. Wider with STEP MOD up, narrower down.
      ```python
      r.set(steps=5, pitch_amount=0, cutoff=0, res=1, stepmod=0.5,
            dest="Cutoff", wave="Saw")
      ```
- [x] 2.5. CLOCK out into TRIG in, SKIPS at 0.6: every step now sounds, and
      a skipped step repeats the last sound instead of going silent.
      ```python
      r.set(steps=5, skips=0.6)
      r["clock"] >> r["trig"]
      ```

## 3. The clock

```python
lfo = vcv.module("LFO", frequency=2)   # 4 Hz, a 0..10 V square
lfo["square"] >> r["clock"]
r.set(tempo=0.5)                       # x1 on the display
```

- [x] 3.1. Four steps a second, in time with the LFO. TEMPO to the right:
      x2, x4, x8; to the left /2, /4, /8, as the tooltip says.
- [x] 3.2. Pull the cable out of CLK: the internal clock takes over at
      once, with no gap.
- [x] 3.3. Turn the LFO's FREQUENCY to its minimum instead: two seconds of
      silence, then the internal clock takes over. The pause is intended.
- [x] 3.4. **Decide -** the default TEMPO (0.2, 2 Hz) sits on /4 under an
      external clock, as on the hardware. Keep, or move the default to the
      x1 zone (0.5, 11 Hz on the internal clock)?

## 4. Processing and the menu

- [x] 4.1. A saw into IN, NOISE and the volume decay full, CUTOFF closed:
      the saw comes through filtered and stepped, a sound-effect device.
      ```python
      vco = vcv.module("VCO", frequency=-12)
      vco["sawtooth"] >> r["audio"]
      r.set(noise=1, tempo=0.275, volume_decay=1, stepmod=0.2,   # 4 Hz
            dest="Cutoff", res=0.5, cutoff=0)
      ```
- [ ] 4.2. A dead cable in IN, NOISE and RES full: only the filter's own
      ringing, stepped to a melody by the step mod on CUTOFF.
      ```python
      dead = vcv.module("VCA-1")
      dead["channel"] >> r["audio"]
      r.set(noise=1, res=1, cutoff=0.3, pitch_amount=0, stepmod=0.5,
            dest="Cutoff", steps=5)
      ```
- [ ] 4.3. **Decide -** is that ringing loud enough next to a kick? It is
      about 0.8 V rms at VOLUME 0.5 (`rubigo_probe voice`).
- [ ] 4.4. Menu, Effect, Flanger, on the rust side: the kicks swoosh. On
      corr: the same, subtler. The EFFECT knob sets the sweep speed.
      ```python
      r.menu(effect=3)   # Flanger
      r.set(rust=1, effect=0.3)
      ```
- [ ] 4.5. Menu, Effect, 2nd oscillator, EFFECT at 0 on corr: an octave
      below doubles the kick, fuller and lower.
      ```python
      r.menu(effect=1)   # 2nd oscillator
      r.set(effect=0, rust=0, wave="Saw")
      ```
- [ ] 4.6. Ctrl+R a few times: each press a different playing sound, kick,
      bass, noise, a resonant line or a drone, never silence.
- [ ] 4.7. The preset browser: eleven presets, each playing at once and
      different from the others. Loading one keeps the loop you had.
