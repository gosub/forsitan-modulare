# aether audition

Rebuild and install before a session, then restart Rack. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py aether 1.1
python3 tools/audition/audition.py aether --list
```

---

## 0. The bench

aether is a transmission line with something wrong with it: audio becomes a
pulse train clocked by **carrier**, and a PLL clocked by **demod** recovers
it. The disagreement between the two clocks is the sound.

Two kinds of item: plain ones are tests, **Decide -** ones are open questions
of taste resting on numbers the harness prints.

Anything that yields a number is not here. `./test/smoke_aether` checks the
recovery gain and its bounds, that a lost lock kills the signal rather than
exploding, both clock outputs against their knobs, the external clock taking
over, the error comparator's swing and threshold, all three types alive, the
oversampling, and both mixes against arithmetic.

`./test/aether_probe` prints what the recovery is worth at each rate.

```python
a = vcv.module("aether", level=0.5, carrier=0.75, demod=0.75, tone=0.7)
out = vcv.module("Audio 2")
a["signal"] >> out["output 1"] + out["output 2"]

def scope(port="signal"):
    s = vcv.module("Scope")
    a[port] >> s["ch 1"]
    return s

def sine(hz=220, volts=4.0):
    o = vcv.module("VCO", freq=vcv.hz(hz))
    att = vcv.module("VCA-1", level=volts / 5.0)
    o["sine"] >> att["channel"]
    att["channel"] >> a["signal"]
    return o

def drums():
    d = vcv.source("drums", loop=1, play=1)
    d["left"] >> a["signal"]
    return d

scope("signal")
```

---

## 1. Nothing patched

With **in** empty the jack supplies a +5 V bias, so the transmitter runs at a
steady rate and the receiver chases it. **level** is a pitch control here, not
a volume control. This is the broken-radio mode.

```python
a.set(type="1 - exclusive-or, locks to harmonics", tone=0.85)
```

- [ ] 1.1. Move **carrier** and **demod** slowly against each other: where the
      receiver cannot lock, the beat between them is the output. Is the useful
      territory spread across the knobs or bunched at one place?
- [ ] 1.2. **level** with nothing patched: it moves the pitch, and does so
      over a range worth playing.
- [ ] 1.3. Type 3 in the same place: it should lock, slip, and false-lock at
      simple ratios, 2/3 turning up often.
      `a.set(type="3 - set-reset latch")`
- [ ] 1.4. Type 2, same place: it locks over the widest range, and where it
      loses lock it goes quiet rather than noisy. Quieter and duller than the
      other two here, correctly.
      `a.set(type="2 - phase-frequency, quiet when unlocked")`

---

## 2. Audio through it

The clean setting is about 15 dB down: a first-order converter at these rates
is worth about that, which is why the hardware is sold as a destruction box.

```python
sine(220)
a.set(type="2 - phase-frequency, quiet when unlocked", carrier=0.75, demod=0.75)
```

- [ ] 2.1. Both clocks matched and high: a 220 Hz sine comes back as a sine.
      Grainy, not broken.
- [ ] 2.2. **Decide -** run `./test/aether_probe` for what the recovery is
      worth at each rate, then listen at the top of both knobs. Is 15 dB down
      an acceptable "clean", or should the top of the range be cleaner than
      the architecture gives?
- [ ] 2.3. Bring **carrier** down alone: the sample rate falls, and the noise
      arrives as the character rather than as a fault.
      `a.set(carrier=0.35)`
- [ ] 2.4. Bring **demod** away from **carrier**: the recovery falls apart in
      a way worth using. Both directions.
      `a.set(demod=0.45)`
- [ ] 2.5. Drums instead of a sine, both clocks low: transients through a
      failing line. Judge it as an effect, not as a codec.
      ```python
      drums()
      a.set(carrier=0.3, demod=0.32)
      ```

---

## 3. tone

**tone** is one pole sitting in the PLL loop *and* on the output, so it is a
tracking control as much as a tone control: 60 Hz at zero, ~3 kHz at the top.

```python
sine(220)
a.set(type="1 - exclusive-or, locks to harmonics")
```

- [ ] 3.1. Sweep **tone** on type 1: low is a narrow loop and it will not
      capture at all, so the output goes quiet. High is bright and tracks.
      The quiet end is not a fault.
- [ ] 3.2. The same sweep on type 2: darker and slower to track at the bottom,
      but it keeps lock. The difference between the two types here is the
      thing to hear.
      `a.set(type="2 - phase-frequency, quiet when unlocked")`

---

## 4. The error output

A comparator across the input and the recovered output, thresholded by the
**error** knob. It is the output to take for drums.

```python
drums()
a.set(carrier=0.4, demod=0.4)
a["error"] >> out["output 1"] + out["output 2"]
scope("error")
```

- [ ] 4.1. **error** centred: its noisiest, and the closest thing here to a
      ring modulator.
- [ ] 4.2. **error** wound to either extreme: it squares up whichever signal
      has the bigger excursions, so the knob reads as a wet/dry made of square
      waves.

---

## 5. The clocks

**tx** and **rx** are the two clock outputs, and they keep running when
something is patched into the **clk** input beside them.

```python
a.set(type="1 - exclusive-or, locks to harmonics", tone=0.85)
```

- [ ] 5.1. **tx** and **rx** into a scope: both are wide-range squares that
      follow their knobs, and both keep running when a clock is patched in
      beside them.
      ```python
      s = vcv.module("Scope")
      a["carrier"] >> s["ch 1"]
      a["demod"] >> s["ch 2"]
      # the second half of the item: tx keeps running with this patched in
      ext = vcv.module("LFO", freq=vcv.hz(400, "LFO"), offset=1)
      ext["square"] >> a["carrier clk"]
      ```
- [ ] 5.2. **Playing the clock**: a 1 V/oct oscillator into the carrier's
      **clk**, nothing in **in**. You are playing the transmitter's rate, and
      the demodulator's **cv** is the timbre.
      ```python
      osc = vcv.module("VCO", freq=vcv.hz(110))
      osc["square"] >> a["carrier clk"]
      ```
- [ ] 5.3. Unpatched, each clock **cv** is fed the signal itself, so its
      attenuator is audio-rate FM depth. Turn the carrier's **cv** trimpot up
      with a sine in: it should sound like FM, not like a fault.
      ```python
      sine(220)
      a.set(carrier_cv=0.6)
      ```

---

## 6. The two mixes

Each output has its own dry/wet against the signal at **in**, so aether can
sit in an effect send alone. The dry side is the jack itself, before **level**.

```python
sine(220)
a.set(carrier=0.4, demod=0.42)
```

- [ ] 6.1. Sweep the **out** mix trimmer from wet to dry: it should read as a
      send control, arriving at the untouched input with nothing added.
      `a.set(out_mix=0.5)`
- [ ] 6.2. Both mixes default fully wet, and with nothing in **in** the dry
      side is silence. Confirm the broken radio only speaks at the wet end.
      ```python
      a["signal"].unpatch()      # the item is about an empty in jack
      a.set(out_mix=1.0)
      ```
