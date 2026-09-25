# radix audition

Rebuild and install before a session, then restart Rack. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py radix 1.1
python3 tools/audition/audition.py radix --list
```

---

## 0. The bench

radix is an integer machine on its own clock: **Source**, **Law** and
**Table** pick one of 150 programs. The panel has no labels, so the items
name every control as its tooltip does; hover to find it.

Two kinds of item: plain ones are tests, **Decide -** ones are open questions
of taste resting on numbers the harness prints.

Anything that yields a number is not here. `./test/smoke_radix` checks all
150 programs reached by CV alone, the stepped rounding, V/oct, CV OUT's
range, OUT fed back into IN under every law, NaN at every input, the menu
state through a save, and that **Grit** is audible a quarter of the way up.
`./test/radix_probe measure` prints every program's level, DC and nearest
neighbour, `pitch` the pitch against the clock, `grit` what **Grit** adds in
dB.

```python
r = vcv.module("radix")
out = vcv.module("Audio 2")
r["audio"] >> out["output 1"] + out["output 2"]

def scope(port="audio"):
    s = vcv.module("Scope")
    r[port] >> s["ch 1"]
    return s
```

---

## 1. The program

The defaults are the plain end: **Source** on Param, **Law** on Add, **Table**
on Sine, **Param** at 128, a sine at C4.

- [x] 1.1. Sweep **Param**: the pitch bends up to half an octave either side
      of C4 and stays a clean tone throughout.
- [x] 1.2. Step **Table** through its six positions: six different waves,
      and Text is a staircase of the letters of "forsitan radix".
- [x] 1.3. **Source** on Self, then step **Law** through its five positions:
      five different kinds of instability, none of them a plain tone.
      ```python
      r.set(src="Self", clock=0.537)  # about 4 kHz: at 32 kHz it is too fine to hear
      ```
- [x] 1.4. **Source** on Counters, **Law** on Sync: a figure of timbres that
      loops every 256 cycles, about once a second at C4, the pitch steady.
      Each **Param** position is a different figure. The busiest corner.
      `r.set(src="Counters", law="Sync", table="Saw")`

---

## 2. Clock, bits and grit

- [ ] 2.1. Bring **Clock** down from 32 kHz with **Rate** still: the pitch
      stays, the sound turns to steps and aliases. That is the resolution
      falling, not a fault.
- [ ] 2.2. The same sweep with "Clock moves pitch" on in the menu: now the
      pitch falls with the clock, as on the hardware.
      `r.menu(clockMovesPitch=True)`
- [ ] 2.3. **Bits** down to 1: the sine turns to a square by audible steps,
      one per tick on the disc.
- [ ] 2.4. **Grit** swept on the default sine: audible from the first tenth,
      and it stays in tune all the way to fully corrupted.

---

## 3. Sequencing the program

```python
r.set(src="Self", table="Text")
```

- [ ] 3.1. Open the VCA: a slow ramp walks **Law** through its positions in
      order, one hard change at each step. The click at the change is meant.
      `vcv.modulate(r["law cv"], rate=0.1, shape="sawtooth")`
- [ ] 3.2. **Decide -** the same ramp on **Table** instead: is a hard jump
      between tables the right thing, or should a table change be the one
      place that is smoothed?
      `vcv.modulate(r["table cv"], rate=0.1, shape="sawtooth")`

---

## 4. Feedback

OUT patched back into IN, **Source** on Input: the delay-free version of the
howl olim was designed around.

```python
r["audio"] >> r["audio"]
r.set(src="Input", param=0)
```

- [ ] 4.1. Bring **Param** up from zero: from the plain sine into a loop
      that feeds on itself. Every **Law** gets there differently.
- [ ] 4.2. **CV OUT** into **Param CV** as well, **Param** low: the loop is
      now modulated by its own rungler, stepping rather than howling.
      `r["cv"] >> r["param cv"]`

---

## 5. Text

```python
r.set(table="Text")
```

- [ ] 5.1. Type another string into the menu and press Enter: the wave
      changes to its letters, and a single repeated letter is silence.
