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
TIME clamped to it. `./test/olim_probe heads` prints the head positions,
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

def sine(hz=220):
    s = vcv.module("VCO", freq=vcv.hz(hz))
    s["sine"] >> o["in l"] + o["in r"]
    return s

def clock(hz=2.0):
    c = vcv.module("LFO", freq=vcv.hz(hz, "LFO"), offset=1)
    c["square"] >> o["clock"]
    return c

drums()
```

---

## 1. The heads

- [ ] 1.1. Defaults: the dry loop, and behind it eight echoes spread evenly
      over the two seconds after each hit.
- [ ] 1.2. **Spread** fully left: the echoes crowd right behind each hit and
      thin out toward two seconds. `o.set(spread=0)`
- [ ] 1.3. **Spread** fully right: one early echo, then a cluster close to
      two seconds. `o.set(spread=1)`
- [ ] 1.4. Sweep **Spread** through noon by hand: a stretch in the middle
      changes nothing. That is the flat spot, not a fault.
- [ ] 1.5. Open the VCA: **Time** moves under the loop and the echoes smear
      and re-grab, never bending pitch and never clicking.
      `vcv.modulate(o["time cv"], rate=0.05)`
- [ ] 1.6. Only **Head 8** up, then flick it down and up: one echo at two
      seconds, and the slider lands in steps of a fifth of a second, not at
      once. Not a fault.
      `o.set(head1=0, head2=0, head3=0, head4=0, head5=0, head6=0, head7=0, head8=1)`

---

## 2. Feedback

```python
o.set(feedback=0.5)   # on the arc: loop gain exactly 1
```

- [ ] 2.1. Stop the loop after a bar: the echoes neither fade nor build.
      Sound on sound, for as long as it is left.
- [ ] 2.2. **Feedback** past the arc: it builds into a howl that stays under
      the clip, and the stereo image comes apart as it rises.
      `o.set(feedback=0.85)`
- [ ] 2.3. **Decide -** at the arc one head holds for good but eight heads
      fade by 4.4 dB over 40 passes (`olim_probe loop`), because the loop is
      divided by the sum of the sliders, as on the hardware. Keep it, or
      divide by their power sum so a full fan of heads holds too?

---

## 3. Clock

```python
clock(2.0)
```

- [ ] 3.1. Turn **Time**: it jumps in doubles and halves, and its tooltip
      reads in clocks. With **Spread** at noon every echo is on the beat.
- [ ] 3.2. **Spread** off noon: the inner echoes leave the beat, the last
      one stays on it. `o.set(spread=0.8)`
- [ ] 3.3. Pull the clock cable: **Time** is free again at once, back to two
      seconds. `o["clock"].unpatch()`

---

## 4. Short times

```python
sine(110)
o.set(time=0.03, feedback=0.5, dry=0)   # about 7 ms
```

- [ ] 4.1. A comb on the sine: open the VCA and its pitch walks in steps a
      fifth of a second apart rather than gliding. Not a fault.
      `vcv.modulate(o["time cv"], rate=0.2)`

---

## 5. VCAs and memory

- [ ] 5.1. Open the VCA: **Head 4** pulses with the LFO and its slider sets
      the depth. `vcv.modulate(o["head4 vca"], rate=1.0)`
- [ ] 5.2. Menu, Memory, 20 s: the output drops out for a moment and the
      echoes start again from an empty buffer. Not a fault.
