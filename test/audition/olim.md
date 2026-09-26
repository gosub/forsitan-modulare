# olim audition

Rebuild and install before a session, then restart Rack. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py olim 1.1
python3 tools/audition/audition.py olim --list
```

---

## 0. The bench

olim is eight read heads over one buffer: **Time** places the last one,
**Spread** crowds the other seven toward now or toward it, and the sliders
mix the dry signal and the heads. Every head refades to its place five times
a second, so nothing olim does ever bends pitch.

Two kinds of item: plain ones are tests, **Decide -** ones are open questions
of taste resting on numbers the harness prints.

Anything that yields a number is not here. `./test/smoke_olim` checks where
an echo lands and that it is inverted, the VCA normalling and clamp, R
normalled from L, NaN at the input, the clock cable, the Memory swap and
TIME clamped to it, and that a slider move lands over the next fades, not
at once. `./test/olim_probe heads` prints the head positions,
`loop` the level per pass against **Feedback**, `clicks` the roughness of
every sweep.

```python
o = vcv.module("olim")
out = vcv.module("Audio 2")
o["out l", "out r"] >> out["output 1", "output 2"]

def drums():
    d = vcv.source("drums", loop=1, play=1)
    d["left", "right"] >> o["in l", "in r"]
    return d

def noise():
    n = vcv.module("Noise")
    n["pink noise"] >> o["in l"] + o["in r"]
    return n

def notes(release=0.5):
    """Four short notes, then four rests, two steps a second: C, E flat, G
    and the C above over 1.5 s, and the rests leave the echoes alone. The
    pitch says which note an echo belongs to. CV 2 gates SEQ3's trigger, so
    only steps 1 to 4 play. A longer `release` gives a loop something to
    build on."""
    seq = vcv.module("SEQ3", tempo=1, run=1, steps=8,
                     cv_1_step_1=0, cv_1_step_2=0.25, cv_1_step_3=0.5833,
                     cv_1_step_4=1, cv_2_step_1=10, cv_2_step_2=10,
                     cv_2_step_3=10, cv_2_step_4=10)
    gate = vcv.module("VCA-1")
    seq["trigger"] >> gate["channel"]
    seq["cv 2"] >> gate["cv"]
    env = vcv.module("ADSR", attack=0.1, decay=0.5, sustain=0, release=release)
    gate["channel"] >> env["gate"]
    osc = vcv.module("VCO")
    seq["cv 1"] >> osc["pitch"]
    amp = vcv.module("VCA-1")
    osc["triangle"] >> amp["channel"]
    env["envelope"] >> amp["cv"]
    amp["channel"] >> o["in l"] + o["in r"]
    return seq

def clock(hz=2.0):
    c = vcv.module("LFO", freq=vcv.hz(hz, "LFO"), offset=1)
    c["square"] >> o["clock"]
    return c

seq = notes()
```

---

## 1. The heads

- [x] 1.1. Defaults: the four notes, and behind each one eight echoes
      spread evenly over the two seconds after it.
- [x] 1.2. **Spread** fully left: the echoes crowd right behind each note
      and thin out toward two seconds.
      `o.set(spread=0)  # -100% on the display`
- [x] 1.3. **Spread** fully right: one early echo, then a cluster close to
      two seconds.
      `o.set(spread=1)  # +100% on the display`
- [x] 1.4. Sweep **Spread** through noon by hand: a stretch in the middle
      changes nothing. That is the flat spot, not a fault.
- [x] 1.5. Open the VCA to about 20%: **Time** swings between 1 and 4 s
      under the notes, and the echoes smear and re-grab, never bending pitch
      and never clicking.
      `vcv.modulate(o["time cv"], rate=0.05, offset=False)  # +-5 V`
- [x] 1.6. Only **Head 8** up: one echo, two seconds after each note.
      ```python
      o.set(head1=0, head2=0, head3=0, head4=0,
            head5=0, head6=0, head7=0, head8=1)
      ```

---

## 2. Feedback

```python
o.set(feedback=0.5)   # on the arc: 1.00x on the display
```

- [x] 2.1. Only **Head 8** up, stop SEQ3 after a cycle: the phrase neither
      fades nor builds. Sound on sound, for as long as it is left.
      ```python
      o.set(head1=0, head2=0, head3=0, head4=0,
            head5=0, head6=0, head7=0, head8=1)
      ```
- [x] 2.2. All eight heads at 100%, stop SEQ3 after a cycle: the echoes
      blur into each other and fade within about ten seconds. Not a fault:
      the loop averages the heads, and only a single head holds at the arc.
      ```python
      o.set(head1=1, head2=1, head3=1, head4=1,
            head5=1, head6=1, head7=1, head8=1)
      ```
- [x] 2.3. Only **Head 8**, **Feedback** past the arc: the phrase builds to
      the ceiling (-6 dB on Rack's meters, olim's 5 V) with a little grit.
      Up to 2x it gets there faster and wanders more, not louder.
      ```python
      seq = notes(release=0.8)
      o.set(head1=0, head2=0, head3=0, head4=0,
            head5=0, head6=0, head7=0, head8=1)
      o.set(feedback=0.85)  # 1.63x on the display
      ```
- [x] 2.4. Only **Head 8**, **Time** very short, **Feedback** at the top: a
      pitched, buzzing howl held under the ceiling, an octave below what the
      delay alone would ring at (the inverted loop takes two passes a cycle).
      ```python
      o.set(head1=0, head2=0, head3=0, head4=0,
            head5=0, head6=0, head7=0, head8=1)
      o.set(time=0.0354, feedback=1)  # 0.0100 s, 2.00x: a 50 Hz howl
      ```
- [x] 2.5. All eight heads, **Time** a quarter second, **Feedback** at the
      top: no howl, the fan breaks into a high hiss held under the ceiling.
      Not a fault: the hardware's limiter at work. At a long **Time** the
      same fan only fades.
      `o.set(time=0.177, feedback=1)  # 0.25 s, 2.00x on the display`

---

## 3. Clock

```python
clock(2.0)["square"] >> seq["clock"]   # the notes on the same clock
```

- [x] 3.1. Turn **Time**: it jumps in doubles and halves, and its tooltip
      reads in clocks. With **Spread** at noon every echo is on the beat.
      Past 8 clocks the echoes land on later repeats of the phrase (it
      repeats every 8); stop SEQ3 there and they spread out instead.
- [x] 3.2. **Spread** off noon: the inner echoes leave the beat, the last
      one stays on it.
      `o.set(spread=0.8)  # +51% on the display`
- [x] 3.3. Pull the clock cable: **Time** is free again at once, back to two
      seconds.

---

## 4. Short times

```python
noise()
o.set(head1=0, head2=0, head3=0, head4=0,
      head5=0, head6=0, head7=0, head8=1)
o.set(time=0.025, dry=0)   # 0.0050 s on the display
o.set(feedback=0.385)      # 0.95x, just under the arc
```

- [ ] 4.1. Noise rings as a pitched buzz, a plucked string held open: 100 Hz,
      an octave under what 5 ms suggests (the inverted loop).
- [ ] 4.2. Open the VCA to about 20%: the pitch sweeps an octave either way,
      in steps a fifth of a second apart rather than gliding. Not a fault.
      `vcv.modulate(o["time cv"], rate=0.2, offset=False)  # +-5 V`

---

## 5. VCAs and memory

- [ ] 5.1. Only **Head 4** up. Open the VCA: its echo pulses with the LFO,
      and its slider sets the depth.
      ```python
      o.set(head1=0, head2=0, head3=0, head4=1,
            head5=0, head6=0, head7=0, head8=0)
      vcv.modulate(o["head4 vca"], rate=1.0)
      ```
- [ ] 5.2. Menu, Memory, 20 s: the output drops out for a moment and the
      echoes start again from an empty buffer. Not a fault.

---

## 6. Heads presets

```python
clock(2.0)["square"] >> seq["clock"]
o.set(time=0.75)   # 8 clocks: each head an eighth of TIME
```

- [ ] 6.1. Menu, Heads presets, Tresillo, then Rotate left a few times from
      Heads transform: the rhythm walks across the bar a head at a time.
- [ ] 6.2. Any preset, then Ctrl+Z once: all eight sliders go back to where
      they were, and **Dry** never moved.
- [ ] 6.3. Tresillo, then each mutation a few times: Mutate keeps the
      rhythm and moves the accents, Mutate pattern moves one echo a step,
      Mutate wide starts to blur it.
