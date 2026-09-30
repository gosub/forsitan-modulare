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
scaling, NaN, the buffer swap and every preset. `./test/spira_probe spiral` measures the
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

- [ ] 1.1. Defaults: every two seconds a 250 ms piece of the break repeats
      and fades, over the break going on. This is the plain stutter.
- [ ] 1.2. SPIRAL left: every repeat shorter and higher, until within two
      seconds the circle rises into a pitched buzz and is gone.
      `s.set(spiral=-0.5, fade=0)   # x0.84 a lap, 1.6 s a circle`
- [ ] 1.3. TAPE to 0 on the same: the laps still shrink, but the pitch stays
      the break's own; a roll that speeds into a buzz.
      `s.set(spiral=-0.5, fade=0, tape=0)`
- [ ] 1.4. SPIRAL right: every repeat longer, slower and lower, until the
      circle is a slow dark smear.
      `s.set(spiral=0.5, fade=-1, tone=-0.7)   # x1.19 a lap`
- [ ] 1.5. ANCHOR on the roll of 1.3, from 0 to full: the roll closes in on
      the start of the window, then on its end.
      `s.set(spiral=-0.5, fade=0, tape=0, anchor=1)`
- [ ] 1.6. **Decide -** circles sum at unity and a soft limiter holds them
      under 10 V. At RATE 2 Hz with FADE 0, eight long circles overlap and
      the limiter works. Keep unity, or scale the circles down as more of
      them sound?
      `s.set(rate=0.6234, fade=0, size=0.8, mix=1)   # 2 Hz, 1.3 s laps`

## 2. The lap

- [ ] 2.1. SHAPE left: every repeat has an attack and a decay, so the
      repeats are plucked notes rather than a stutter.
      `s.set(shape=-0.7, spiral=-0.2, fade=-1)`
- [ ] 2.2. SHAPE right, reversed: every repeat swells backwards and stops.
      `s.set(shape=0.6, soft=0.6, direction="Reverse")`
- [ ] 2.3. Ping-pong with SPREAD full: the laps go forward and back and
      trade sides, with no click at the turnarounds.
      `s.set(direction="Ping-pong", spread=1, size=0.6157)   # 400 ms`
- [ ] 2.4. A cloud: short soft laps, many circles, scattered. No single
      repeat is recognisable.
      ```python
      s.set(size=0.299, soft=1, rate=0.9164, jitter=0.6, reach=0.3,
            spread=1, fade=-6)   # 60 ms laps, 12 circles a second
      ```
- [ ] 2.5. TONE left: each turn darker. Right: each turn thinner.
      `s.set(tone=-1.5, fade=-1)`

## 3. The line

- [ ] 3.1. HOLD (the button): the input is cut off and the last eight
      seconds loop, the light on. Press again: the live break returns
      without a click.
      `s.set(mix=0)   # the line alone`
- [ ] 3.2. HOLD with REACH full: circles are born anywhere on the held loop,
      not just behind the playhead.
      `s.set(reach=1, rate=0.6234, jitter=0.4)   # 2 Hz`
- [ ] 3.3. RATE at zero: nothing new. Each BIRTH press grows one circle and
      one flash of the button; the laps that follow do not flash it.
      `s.set(rate=0, spiral=-0.3, fade=0)`
- [ ] 3.4. V/OCT out into a sine: the sine climbs with each converging
      circle, an octave each time the laps halve, and stops two octaves up.
      ```python
      vco = vcv.module("VCO", freq=vcv.hz(110))
      s["voct"] >> vco["1v/octave pitch"]
      vco["sine"] >> out["output 1"] + out["output 2"]
      s.set(spiral=-0.5, fade=0, rate=0.3)
      ```

- [ ] 3.5. The ring over SPIRAL: each birth lights the next light round,
      left to right, and a light dims with its circle's fade.
      `s.set(rate=0.51, fade=-4)   # 1 Hz`
- [ ] 3.6. SPIRAL left: the lights run orange-red. Right: blue. Centre:
      yellow.
      `s.set(rate=0.51, fade=-2, spiral=-0.6)`

## 4. The menu

- [ ] 4.1. One long circle (RATE off, SIZE 2 s, FADE 0), then turn SPIRAL:
      the sounding circle follows at its next lap. Menu, keep the settings
      they were born with, and again: it does not, only the next BIRTH.
      With the menu on and SPIRAL swept, the ring shows several colours.
      `s.set(rate=0, size=0.8843, fade=0)   # 2 s`
- [ ] 4.2. The preset browser: eight presets, each different at once, none
      of them the plain stutter of 1.1. **scatter** with HOLD pressed wanders
      a frozen loop.
