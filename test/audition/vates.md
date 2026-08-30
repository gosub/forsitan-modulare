# vates audition

Rebuild and install before a session, then restart Rack. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py vates 2.1
python3 tools/audition/audition.py vates --list
```

---

## 0. The bench

vates ships no audio files, so there is nothing to configure: it builds six
banks from a seed when you place it. Its own pattern gate plays it, which is
how it plays itself on the hardware.

Two kinds of item: plain ones are tests, **Decide -** ones are open questions
of taste resting on numbers the harness prints.

Anything that yields a number is not here. `./test/smoke_vates` checks the
envelopes forward and reversed, the retrigger and end-of-sample guards, the fx
delay times at both tempos, the filter's reach and its silent crossing, every
bank and knob position, the whole clock/pattern/LFO section, and that nothing
is silent or unbounded.

```python
v = vcv.module("vates", level=0.8)
out = vcv.module("Audio 2")
v["left", "right"] >> out["output 1", "output 2"]

def scope(port="left"):
    s = vcv.module("Scope")
    v[port] >> s["ch 1"]
    return s

def plays():
    """The pattern gate into its own trigger: vates playing itself."""
    v["gate"] >> v["trig"]

def clock(hz=2.0, to="clk"):
    c = vcv.module("LFO", freq=vcv.hz(hz, "LFO"), offset=1)
    c["square"] >> v[to]
    return c

plays()
scope("left")
```

---

## 1. Sanity

```python
v.set(sample=0.0, length=0.6)
```

- [x] 1.1. Place it fresh: the banks build in about a tenth of a second and
      the display counts up. Sound within a second, and no burst of noise
      before it.
- [x] 1.2. **env out** on a scope alongside: 0-10 V, one shape per hit,
      0 between hits.
      `scope("env")`
- [x] 1.3. Odd state (bank 5, a rerolled kit, samples-per-bank 32), save,
      reload: the same kit and the same sound, not a fresh roll.

---

## 2. The kit

The six generated banks are drums, objects, grains, micro, tones, air. Each
holds one sample of each *kind* its generator knows, at eight pitches - never
one drum in eight sizes. Which kind lands on which position comes from the
seed, so it differs between banks and after a reroll.

```python
v.set(sample=0.0, length=0.6)
```

- [x] 2.1. Walk **sample** across a bank: eight distinct kinds of sound, not
      one sound in eight sizes.
- [x] 2.2. Walk the six banks with **bank up**: drums, objects, grains, micro,
      tones, air. Each recognisably the thing its name says.
- [x] 2.3. **Decide -** **reroll kit** draws forty-eight new sounds from a new
      seed, and re-deals which kind sits at which position, so a part does not
      survive a reroll - that is not a fault. Roll it a dozen times. Is the
      average roll good enough to be what a new user meets, or does it need
      curating?

---

## 3. length

One knob for the envelope and the direction: centre is the shortest envelope,
right grows a decay and plays forward, left grows an attack and plays
backwards.

```python
v.set(sample=0.35, length=0.6)
```

- [x] 3.1. Sweep **length** right from centre: the decay grows smoothly from a
      click to a few hundred ms and beyond.
- [x] 3.2. Sweep left from centre: the attack grows and the sample runs
      backwards, loudest at the end.
- [x] 3.3. Cross centre while it is playing: the direction changes between
      hits and never mid-sample.
- [x] 3.4. Reversed hits mirror the forward one: the same stretch of sample,
      backwards, ending on the transient. Sweep the left half - every setting
      should sound, not just the far end.
      `v.set(length=-0.4)`
- [x] 3.5. The three reverse options against the default, on a sequence.
      **decay instead** keeps the attack on the beat; **start at the end**
      sweeps the whole sample and goes quiet on long tails; **retrigger while
      swelling** stops the sequence dropping notes. Any of them worth being
      the default?
      ```python
      v.set(length=-0.6)
      v.menu(reverseDecays=False, reverseFromEnd=False, retriggerDuringAttack=False)
      ```
- [x] 3.6. On the **tones** and **air** banks, which sustain rather than
      strike: the same knob should read as a swell rather than a hit. At
      either end the envelope is held, so these banks drone instead of
      hitting - that is not a fault.
      `v.menu(bank=4)`

---

## 4. pitch

```python
v.set(sample=0.35, length=0.7)
```

- [x] 4.1. **pitch** across its travel: two octaves down to two up, smooth,
      and still in tune at both ends against the reference.
      ```python
      # the reference in one ear, vates in the other
      osc = vcv.module("VCO", freq=vcv.hz(261.6256))   # C4, where samples render
      osc["sine"] >> out["output 2"]
      ```
- [x] 4.2. A scale into **note** with the same stepped CV driving the
      reference: the two stay in unison all the way up, with no interval
      opening as it climbs.
      ```python
      v.menu(bank=4, scale=11)   # tones; chromatic, so the quantizer passes semitones
      v.set(sample=0.2, length=0.8, pitch_att=1.0)   # fully clockwise is 1V/oct
      seq = vcv.module("SEQ3", run=1, steps=8)
      clk = vcv.module("LFO", freq=vcv.hz(2.0, "LFO"), offset=1)
      # a note on every step, not only on the pattern's: the sequencer advances
      # on each tick and the voice has to fire with it, so the clock takes over
      # the trig jack from the pattern gate
      clk["square"] >> seq["clock"] + v["clk"] + v["trig"]
      for i, semi in enumerate([0, 2, 4, 5, 7, 9, 11, 12]):
          seq.set(**{"cv_1_step_%d" % (i + 1): semi / 12.0})
      osc = vcv.module("VCO", freq=vcv.hz(261.6256))   # C4, where samples render
      seq["cv 1"] >> v["note"] + osc["1v/octave"]
      osc["sine"] >> out["output 2"]
      ```

---

## 5. tone

**filter** is a resonant lowpass to the left and a resonant highpass to the
right, open at centre. **fx** is a tempo-synced delay to the left and a
chorus-into-flanger to the right, dry at centre.

```python
v.set(sample=0.2, length=0.7)
```

- [x] 5.1. Sweep **filter** end to end on a busy pattern: usable across the
      whole travel, resonance that sings without screaming.
      `v.set(rhythm=3)`
- [x] 5.2. **fx** left: a dotted-quarter delay on the left channel against a
      plain beat on the right, thrown side to side. The cross rhythm should be
      the point, not a smear. On one hit a bar, because a hit on every beat
      fills both channels with the same eighths and there is no 3:2 left to
      hear.
      `v.set(fx=-0.6, rhythm=12)`
- [x] 5.3. **fx** hard left: a tail of some ten seconds that thickens as it
      saturates rather than clipping the output.
      `v.set(fx=-1.0)`
- [ ] 5.4. **fx** right: sweep the whole half. A chorus in the first third, a
      flanger at the top, and two different effects rather than one getting
      stronger.
      `v.set(fx=0.2)`
- [ ] 5.5. Sweep **fx** across centre while a beat plays: it should fall out of
      the delay and into the flanger as one musical move.

---

## 6. Playing it

```python
v.set(sample=0.35, length=0.6)
```

- [ ] 6.1. A slow LFO into **sample**. In **play** its crossings fire hits of
      their own, off the grid, so the modulation is the rhythm; in **cue**
      only the pattern's hits sound and the LFO just chooses which sample.
      The knob fires in neither, so you can browse a kit without the module
      screaming - that is not a fault.
      ```python
      v.set(mode="play", sample_att=1.0)
      vcv.module("LFO", freq=vcv.hz(0.3, "LFO"), offset=0)["triangle"] >> v["sample"]
      ```
- [ ] 6.2. An external clock at **clk** with the rhythm knob moving: the
      pattern follows the tempo and the rhythms are worth having.
      `clock(hz=4.0)`
- [ ] 6.3. Your own kit: point the kits folder at a folder of wavs from the
      context menu. It loads without interrupting the audio, cuts into banks
      of eight, and the display names them `mykit 1/3`.
