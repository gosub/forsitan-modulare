# spira audition

Rebuild and install before a session, then restart Rack. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py spira 1.1
python3 tools/audition/audition.py spira --list
```

---

## 0. The bench

spira loops short circles off its input while the input plays on. Between
laps a circle can shrink or grow (SPIRAL), by speed or by cutting (TAPE),
and fade, darken, pluck or swell. The question under every item: does it
sound like more than a stutter effect?

Two kinds of item: plain ones are tests, **Decide -** ones are open questions
of taste resting on numbers the harness prints.

Anything that yields a number is not here. `./test/smoke_spira` checks the
defaults, the direction switch, BIRTH and HOLD, the normalled input, the CV
scaling, NaN, the buffer swap, SKIPS against the button, the lights and
every preset. `./test/spira_probe spiral` measures the
lap geometry and FADE, `clicks` the seams in every lap mode, `line` HOLD,
RATE and REACH.

```python
s = vcv.module("spira")
out = vcv.module("Audio 2")
s["out l", "out r"] >> out["output 1", "output 2"]
d = vcv.source("drums", loop=1, play=1)
d["left", "right"] >> s["in l", "in r"]
```

---

## 1. The spiral

- [x] 1.1. Defaults: every two seconds a 250 ms piece of the break repeats
      and fades, over the break going on. This is the plain stutter.
- [x] 1.2. SPIRAL left: every repeat shorter and higher, until within two
      seconds the circle rises into a pitched buzz and is gone.
      `s.set(spiral=-0.5, fade=0)   # x0.84 a lap, 1.6 s a circle`
- [x] 1.3. TAPE to 0 on the same: the laps still shrink, but the pitch stays
      the break's own; a roll that speeds into a buzz.
      `s.set(spiral=-0.5, fade=0, tape=0)`
- [x] 1.4. SPIRAL right: every repeat longer, slower and lower, until the
      circle is a slow dark smear.
      `s.set(spiral=0.5, fade=-0.1291, tone=-0.7)   # x1.19 a lap, -1 dB`
- [x] 1.5. ANCHOR, on a roll cut from a 1 s window. Press HOLD, then BIRTH
      with ANCHOR at 0: the roll closes in on the first hit of the window.
      ANCHOR full and BIRTH again: on the window's end, another sound.
      ```python
      s.set(spiral=-0.5, fade=0, tape=0, anchor=0, rate=0, mix=0.85,
            size=0.7687)   # 1 s; RATE off, circles by the button only
      ```
- [x] 1.6. FADE full left: each circle is one lap and gone, a slice rather
      than a repeat. Full right: each circle swells turn by turn and then
      holds, loud, until it is replaced.
      `s.set(fade=-1)   # -60 dB a turn; fade=1 is +6 dB`
- [x] 1.7. LEVEL up, eight long circles piling up: the circles stand over
      the break, the output stays clean and the meter under 0 dB.
      Menu, Output, Saturate: the same pile thickens into grit.
      `s.set(rate=0.6234, fade=0, size=0.8, level=12)   # 2 Hz, 1.3 s laps`

## 2. The lap

- [x] 2.1. SHAPE left, on a steady saw, the circles alone: every repeat is a
      plucked note, rising as the spiral closes. SHAPE back to 0: one
      continuous tone again, the plucks gone.
      ```python
      vco = vcv.module("VCO", freq=vcv.hz(220))
      vco["saw"] >> s["in l"] + s["in r"]
      s.set(shape=-0.7, spiral=-0.2, fade=-0.1291, mix=1)   # -1 dB
      ```
- [x] 2.2. SHAPE right, reversed: every repeat swells backwards and stops.
      `s.set(shape=0.6, soft=0.6, direction="Reverse")`
- [x] 2.3. Ping-pong with SPREAD full: the laps go forward and back and
      trade sides, with no click at the turnarounds.
      `s.set(direction="Ping-pong", spread=1, size=0.6157, mix=0.85)   # 400 ms`
- [x] 2.4. SOFT, on one circle of a saw with vibrato, the circle alone.
      Press BIRTH. At 0: a jump in pitch at every lap, the loop's seam.
      Turn SOFT up: the jumps become glides, and at full the loop is one
      smooth wobble with no seam to hear.
      ```python
      vco = vcv.module("VCO", freq=vcv.hz(220))
      lfo = vcv.module("LFO", frequency=0.5)   # 1.4 Hz, against 2.5 laps a second
      lfo["sine"] >> vco["frequency modulation"]
      vco.set(**{"frequency modulation": 0.15})
      vco["saw"] >> s["in l"] + s["in r"]
      s.set(size=0.6157, fade=0, soft=0, rate=0, mix=1)   # 400 ms
      ```
- [x] 2.5. A cloud: short soft laps, many circles, scattered. No single
      repeat is recognisable.
      ```python
      s.set(size=0.299, soft=1, rate=0.9164, jitter=0.6, reach=0.3,
            spread=1, fade=-0.3162)   # 60 ms laps, 12 a second, -6 dB
      ```
- [x] 2.6. TONE left: each turn darker. Right: each turn thinner.
      `s.set(tone=-1.5, fade=-0.1291, mix=0.85)   # -1 dB`

## 3. The line

- [x] 3.1. HOLD (the button): the input is cut off and the last eight
      seconds loop, the light on. Press again: the live break returns
      without a click.
      `s.set(mix=0)   # the line alone`
- [x] 3.2. HOLD with REACH full: circles are born anywhere on the held loop,
      not just behind the playhead.
      `s.set(reach=1, rate=0.6234, jitter=0.4, mix=0.85)   # 2 Hz`
- [x] 3.3. RATE at zero: nothing new. Each BIRTH press grows one circle and
      one flash of the button; the laps that follow do not flash it.
      `s.set(rate=0, spiral=-0.3, fade=0)`
- [ ] 3.4. V/OCT out into a sine, TURN plucking it: eight voices, one per
      circle, a note at every lap, each climbing with its own converging
      circle, an octave each time the laps halve, up to two octaves.
      Voices overlap as circles do, and fall silent as theirs end.
      ```python
      vco = vcv.module("VCO", freq=vcv.hz(110))
      env = vcv.module("ADSR", attack=0, release=0.35)
      vca = vcv.module("VCA", **{"channel 1 level": 0.3})   # 8 voices summed
      s["voct"] >> vco["1v/octave pitch"]
      s["turn"] >> env["gate"]
      vco["sine"] >> vca["channel 1"]
      env["envelope"] >> vca["channel 1 linear cv"]
      vca["channel 1"] >> out["output 1"] + out["output 2"]
      s.set(spiral=-0.5, fade=0, rate=0.3)
      ```

- [ ] 3.5. A clock into BIRTH, RATE off, SKIPS half way: circles land on
      the clock's grid with holes in it, different every bar. The button
      still grows a circle every press.
      ```python
      clk = vcv.module("LFO", frequency=1)   # 2 Hz square
      clk["square"] >> s["birth"]
      s.set(rate=0, skips=0.5, size=0.299, mix=0.85)   # 60 ms laps
      ```
- [ ] 3.6. The ring over SPIRAL: each birth lights the next light round,
      left to right, and a light dims with its circle's fade.
      `s.set(rate=0.51, fade=-0.2582)   # 1 Hz, -4 dB`
- [ ] 3.7. SPIRAL left: the lights run orange-red. Right: blue. Centre:
      yellow.
      `s.set(rate=0.51, fade=-0.1826, spiral=-0.6)   # -2 dB`

## 4. The menu

- [ ] 4.1. One long circle (RATE off, SIZE 2 s, FADE 0), then turn SPIRAL:
      the sounding circle follows at its next lap. Menu, keep the settings
      they were born with, and again: it does not, only the next BIRTH.
      With the menu on and SPIRAL swept, the ring shows several colours.
      `s.set(rate=0, size=0.8843, fade=0)   # 2 s`
- [ ] 4.2. The preset browser: eight presets, each different at once, none
      of them the plain stutter of 1.1. **scatter** with HOLD pressed wanders
      a frozen loop.
