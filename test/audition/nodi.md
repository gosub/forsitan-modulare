# nodi audition

Rebuild and install before a session, then restart Rack. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py nodi 1.1
python3 tools/audition/audition.py nodi --list
```

---

## 0. The bench

nodi turns the motion of X into one of eight stages: a RISE stage fires when
X rises past its threshold, a FALL stage when X falls past it, and f(X) is
the active stage's slider. Unpatched, X is the internal ramp, so it is a
sequencer whose lower sliders place the steps in time.

Two kinds of item: plain ones are tests, **Decide -** ones are open questions
of taste resting on numbers the harness prints.

Anything that yields a number is not here. `./test/smoke_nodi` checks the
defaults as a running sequencer, the RAMP -> X normal, which way up the
switches read, polyphony, the group gates and the switch, NaN at every
input, other sample rates and the menus. `./test/nodi_probe cross` prints
when each stage fires, `length` the thresholds from the sliders, `ext` the
sixteen-step chain, `alias` the alias energy with and without rounding,
`shapes` the presets, the conversions and the quantizer.

```python
n = vcv.module("nodi")
osc = vcv.module("VCO")
env = vcv.module("ADSR", attack=0.02, decay=0.35, sustain=0, release=0.2)
amp = vcv.module("VCA-1")
out = vcv.module("Audio 2")
scope = vcv.module("Scope")
n["fx"] >> osc["pitch"]
n["gate"] >> env["gate"]
osc["triangle"] >> amp["channel"]
env["envelope"] >> amp["cv"]
amp["channel"] >> out["output 1"] + out["output 2"]
n["fx"] >> scope["ch 1"]
n["ramp"] >> scope["ch 2"]

# A major scale on the f(X) sliders, 0..2.5 V: a semitone is 1/30.
n.set(value1=0, value2=0.0667, value3=0.1333, value4=0.1667,
      value5=0.2333, value6=0.3, value7=0.3667, value8=0.4)

def voice(signal):
    """Anything into X in place of the ramp."""
    signal >> n["x"]

def graphic_vco():
    """The manual's graphic VCO at 110 Hz: every stage RISE over even
    lengths, a sine drawn on the f(X) sliders, f(X) straight to the output."""
    n.set(fast="Fast: 4 Hz to 12 kHz", rate=0.4139,   # 110 Hz on the display
          range="-5 to +5 V",
          dir1="Rise", dir2="Rise", dir3="Rise", dir4="Rise",
          dir5="Rise", dir6="Rise", dir7="Rise", dir8="Rise",
          value1=0.6722, value2=0.9157, value3=0.9157, value4=0.6722,
          value5=0.3278, value6=0.0843, value7=0.0843, value8=0.3278)
    amp2 = vcv.module("VCA-1", level=0.5)
    n["fx"] >> amp2["channel"]
    amp2["channel"] >> out["output 1"] + out["output 2"]
```

---

## 1. The sequencer

- [ ] 1.1. Defaults: a major scale climbs, two notes a second, and starts
      again on the root. The top lights step along; the lower sliders light
      one by one as the ramp passes them, and all go dark at the reset.
- [ ] 1.2. Raise stage 4's lower slider to the top: note 4 lasts twice as
      long and the notes after it are squeezed, the phrase no longer.
      `n.set(thresh4=1)`
- [ ] 1.3. Switch stage 5 to OFF: note 4 carries on through where 5 was.
      `n.set(dir5="Off")`
- [ ] 1.4. POS, lower sliders on a diagonal. Drag slider 3 above slider 5:
      note 3 now plays after 5, and nothing else moves.
      ```python
      n.set(mode=1,   # POS
            thresh1=0, thresh2=0.125, thresh3=0.25, thresh4=0.375,
            thresh5=0.5, thresh6=0.625, thresh7=0.75, thresh8=0.875)
      ```

---

## 2. Following another signal

```python
lfo = vcv.module("LFO", frequency=vcv.hz(0.25, "LFO"), offset=0)   # +-5 V
voice(lfo["triangle"])
```

- [ ] 2.1. A slow triangle at X: notes 2 to 8 climb with it and 8 holds
      all the way down. Note 1 sits at -5 V, the triangle's very turn, and
      may not sound. Not a fault.
- [ ] 2.2. Menu, Direction switches, All fall: now the scale plays on the
      way down, and the way up is silent.
- [ ] 2.3. Menu, Setups, Quantizer, random steps at X: C, E, G and the C
      above in any order, jumping either way, never another note.
      `voice(vcv.module("Random")["stepped"])`

---

## 3. Audio rate

```python
graphic_vco()
```

- [ ] 3.1. A steady 110 Hz tone with a stepped, reedy edge. Drag any top
      slider: the timbre changes as you draw, the pitch does not.
- [ ] 3.2. Open the VCA to about 30%: the pitch sweeps three octaves up and
      back, and stays clean. Menu, Anti-aliasing, Off: a whistle of wrong
      pitches rides the top of the sweep. Back to Auto, and it goes.
      `vcv.modulate(n["voct"], rate=0.1)   # 0..10 V, one octave a volt`
- [ ] 3.3. Menu, Setups, Waveshaper / bitcrusher, a 110 Hz sine at X: four
      levels, a hard square-ish crunch. Pull top sliders 2 and 7 down: it
      folds.
      `voice(vcv.module("VCO", frequency=vcv.hz(110))["sine"])`
- [ ] 3.4. **Decide -** Auto rounds only the steps closer than 2 ms, so with
      eight stages it hands over at 62.5 Hz. Sweep **Rate** slowly through
      40..100 Hz: is the handover inaudible, as it should be?

---

## 4. Groups, the switch and EXT

- [ ] 4.1. Gate A into the envelope instead of GATE: only notes 1, 4 and 7
      sound, the rest are silent steps.
      `n["gate a"] >> env["gate"]`
- [ ] 4.2. Open the VCA on THR A: notes 1, 4 and 7 lean early and late
      against the others; pushed off the end of the ramp, 7 drops out. Not a
      fault.
      `vcv.modulate(n["thr a"], rate=0.2, offset=False)   # +-5 V`
- [ ] 4.3. Menu, Setups, Temporal mixer. A saw, a square and a sine at
      three pitches into A, B, C, COM to the output: all three at once, with
      a buzz at 110 Hz. Lower sliders 1 to 3 act as their faders.
      ```python
      mix = [vcv.module("VCO", frequency=vcv.hz(f)) for f in (220, 330, 440)]
      mix[0]["sawtooth"] >> n["sw a"]
      mix[1]["square"] >> n["sw b"]
      mix[2]["sine"] >> n["sw c"]
      n["com"] >> out["output 1"] + out["output 2"]
      ```
- [ ] 4.4. **Decide -** Two nodi as one sixteen-step sequencer, the manual's
      patch: this one over -5..0 V, the second over 0..+5 V. Each handover
      has both active for 4 samples (`nodi_probe ext`): heard as a click in
      the pitch, or not at all?
      ```python
      m = vcv.module("nodi")
      zero = vcv.module("8vert", row_1_gain=0)   # 0 V from an unpatched row
      zero["row 1"] >> n["hi"] + m["lo"]
      # the scale carries on up an octave: 14 16 17 19 21 23 24 26 semitones
      m.set(dir1="Rise",
            value1=0.4667, value2=0.5333, value3=0.5667, value4=0.6333,
            value5=0.7, value6=0.7667, value7=0.8, value8=0.8667)
      n["ramp"] >> m["x"]
      n["fx"] >> m["y"]
      n["gate"] >> m["ext"]
      m["gate"] >> n["ext"] + env["gate"]
      m["fx"] >> osc["pitch"]
      ```

---

## 5. Menus and lights

- [ ] 5.1. Menu, f(X) sliders, any preset, then Ctrl+Z once: all eight top
      sliders go back together, and nothing else moved.
- [ ] 5.2. Menu, Threshold sliders, Swing (2:1), then Convert to positions:
      the rhythm does not change, the switch flips to POS and the lower
      sliders become a staircase.
- [ ] 5.3. **Decide -** The lower sliders glow faintly while X is above them
      and flash as they fire. Legible as a picture of X, or noise?
