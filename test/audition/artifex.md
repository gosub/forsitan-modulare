# artifex audition

Rebuild and install before a session, then restart Rack - it unpacks the new
`.vcvplugin` at launch, so a running Rack keeps the old one. Then open an item:

```
make && HOME=/home/gg/dl/audio/rackhome/ make install
python3 tools/audition/audition.py artifex 3.8.6
python3 tools/audition/audition.py artifex --list
```

The runner builds the scene, writes the patch to a scratch directory and
launches Rack on it, with the item's text in a Notes module beside the rack.
It needs `test/audition/config.json` - copy `config.example.json` and edit.

---

## 0. The bench

Four sources, switched constantly - most issues show on only one:

- **a drum loop** - transients, slicing, crushing, splices
- **a pad or drone** - filters, pitch shifting, freeze seams
- **a 220 Hz sine** - tuning, detune, aliasing, clicks
- **silence** - self-oscillation, runaway, DC, noise floor

The bench below builds all of that. Nothing in this file asks you to set a
knob before starting: the runner opens Rack with the item already patched, and
an item's own line says only what it changes.

Two kinds of item:

- **untagged** - a test. Do it, judge it, tick it. Everything here needs ears
  or eyes: a verdict a machine cannot give.
- **Decide -** an open question of taste, resting on numbers the harness
  produces. An empty box means the choice is still mine to guess at.

The bench below is the code every item starts from; a section adds to it and
an item changes it.

```python
fx = vcv.module("artifex", gain=1.0, level=0.8)
out = vcv.module("Audio 2")
fx["left", "right"] >> out["output 1", "output 2"]

def sine(hz=220, volts=5.0):
    s = vcv.module("VCO", freq=vcv.hz(hz))
    out = s["sine"]
    if volts != 5.0:
        a = vcv.module("VCA-1", level=volts / 5.0)
        s["sine"] >> a["channel"]
        out = a["channel"]
    out >> fx["left"] + fx["right"]
    return s

def drums():
    d = vcv.source("drums", loop=1, play=1)
    d["left", "right"] >> fx["left", "right"]
    return d

def drone(hz=110, detune=0.06):
    """Two saws a beat apart, for the pad the list asks for. Synthesized
    rather than sampled so it is the same drone on any machine."""
    a = vcv.module("VCO", freq=vcv.hz(hz))
    b = vcv.module("VCO", freq=vcv.hz(hz) + detune)
    m = vcv.module("Mixer", level=0.4)
    a["saw"] >> m["channel 1"]
    b["saw"] >> m["channel 2"]
    m["mix"] >> fx["left"] + fx["right"]
    return m

def clock(hz=2.0, to="clk"):
    c = vcv.module("LFO", freq=vcv.hz(hz, "LFO"), offset=1)
    c["square"] >> fx[to]
    return c

def scope(port="env"):
    s = vcv.module("Scope")
    fx[port] >> s["ch 1"]
    return s

scope("left")     # every item has one, since half of them say "look at it"
```

Anything that yields a number rather than a verdict is not in this list. It
lives in `test/artifex_probe measure`, which prints overwrite times, the
record head's whine, what self-oscillates and the CPU per mode. Measured by
hand a number is measured once, against whatever the build was that
afternoon, and is wrong by the next commit - the whine below was 8x by hand
and is -78 dB now, and nothing but the harness noticed.

---

## 1. Sanity - the module as a box

```python
drums()
fx.set(fxmode="delay", amt="50%")
```

- [x] 1.1. Mode 1, **amount** fully left: signal passes unchanged. No
      colouration, no delay, no level loss.
      `fx.set(amt=0)`
- [x] 1.2. **level** 0→1: smooth, no zipper, no gain jump.
- [x] 1.3. **gain** 0→4: quiet to loud. The **in** lamps go red only on real
      clipping, and per channel - drive L alone, R stays dark.
- [x] 1.4. L only: both outs identical. Patch R: the normal releases.
      ```python
      sine()
      fx["right"].unpatch()
      ```
- [x] 1.5. **Menu → Input → sum to mono** on hard-panned stereo: both outs
      collapse to the same signal.
      `fx.menu(monoInput=True)`
- [x] 1.6. **env out** on a drum loop: fast rise, smooth fall, 0 with no input,
      responds to **gain**.
      `scope("env")`
- [x] 1.7. **out lamps** track level and go dark when the sound stops.
- [x] 1.8. Odd state (mode 7, 5 s buffer, safety off, hardware CV window), save,
      reload: everything returns, no burst of noise.
      ```python
      fx.set(fxmode="pitcher")
      fx.menu(bufSeconds=5.0, limiter=False, hardwareCvWindow=True)
      ```

---

## 2. The display

One display under the title. Left half names the mode, right half reads the
time parameter, both in the mode's colour.

| # | mode | colour | # | mode | colour |
|---|------|--------|---|------|--------|
| 1 | delay | green | 6 | slicer | light green |
| 2 | flanger | cyan | 7 | pitcher | red |
| 3 | freezer | blue | 8 | replayer | orange |
| 4 | panner | white | 9 | shifter | pink |
| 5 | crusher | yellow | | | |

```python
sine()
fx.set(fxmode="replayer")
```

- [x] 2.1. Centred, both halves legible at default zoom. Turn **time**: updates
      smoothly, never collides with the mode name. Worst cases: `8 replayer`
      against `+12.0 semi`, then mode 9 at a large shift.
      `fx.set(fxmode="replayer")`
- [x] 2.2. All nine colours distinct on the near-black. 1 and 6 are the pair to
      check. Mode 5 should match the output badges and the logo exactly.
- [x] 2.3. The reading is the same colour at 70%: one display with two ranks,
      not two colours. Blue (3) and red (7) are where legibility fails first.
      `fx.set(fxmode="freezer")`
- [x] 2.4. Name, number and colour change together, immediately.
- [x] 2.5. The right-click picker opens from either half, marks the running
      mode, and moves the knob when you choose.
- [x] 2.6. LFO saw → **fxmode**, attenuverter full: sweeps all nine and wraps.
      Small attenuverter: alternates between two only.
      ```python
      vcv.module("LFO", offset=0)["saw"] >> fx["fxmode"]
      fx.set(fxmode_att=1.0)
      ```

---

## 3. The nine modes

Each mode opens with the code for its bench, and the items say only what they
change. Anything the code does not name is at the module's own default -
**feedback** 0, **stereo** 0, **filter** centre - over the file's **level**
0.8 and **gain** 1.0.

### 3.1 delay (green)

```python
drums()
fx.set(fxmode="delay", time=0.5, amt="50%")
```

- [x] 3.1.1. **time** right to left: 2 ms → 1.15 s, continuous, no zipper. The
      tape-style pitch bend should be smooth, not crunchy.
- [x] 3.1.2. Short end, **feedback** up: a tuned comb that tracks the knob
      musically over the top quarter.
      `fx.set(time=0.05, fbk="75%")`
- [x] 3.1.3. Clock into **clk**: the knob snaps to divisions and the display
      names them. Change tempo - the delay follows, the name stays.
      `clock()`
- [x] 3.1.4. A clock at **trig** snaps it too, but only while **clk** has none.
      With both patched, clk wins. With neither, the knob is free and the
      display reads ms.
      `clock(to="trig")`
- [x] 3.1.5. Unpatch **clk**: back to internal tempo after ~2 s, no click.
      `clock()`
- [x] 3.1.6. Long delay, **feedback** ~0.7, drum loop: repeats decay.
      `fx.set(time=0.9, fbk="70%")`

### 3.2 flanger (cyan)

```python
sine()
fx.set(fxmode="flanger", time=0.5, amt="50%")
```

- [x] 3.2.1. A chorus as it stands: gentle vibrato on the sine, no zipper.
- [x] 3.2.2. **feedback** up: jet flange. At maximum it should scream without
      destroying itself.
      `fx.set(fbk="90%")`
- [x] 3.2.3. **amount** at the extremes: deep sweep, pitch wobbling smoothly, no
      stair-stepping.
      `fx.set(amt="100%")`
- [x] 3.2.4. **time** to the fast end: the modulator reaches an FM-ish buzz.
      `fx.set(time=1.0)`

### 3.3 freezer (blue)

```python
drums()
clock()
fx.set(fxmode="freezer", time=0.5, amt=0)
```

- [x] 3.3.1. Move **amount** off zero: it captures at that moment, where you
      expect.
- [x] 3.3.2. **trig** takes a new chunk. Repeated trigs on the beat should feel
      like a stutter instrument.
- [x] 3.3.3. **time** moves the loop live, with no trig and without touching
      amount. Sweep right: the loop shortens continuously into a pitch - 1130 ms
      → 3.2 ms, 9 Hz → 308 Hz on a full capture. Sweep back and the bar returns.
- [x] 3.3.4. Left half with a clock: the length lands on exact divisions - 8, 6,
      4, 3, 2, 1. The longest clamp at the 1.15 s buffer; switch to 2.5 s and
      they open up.
- [x] 3.3.5. A freeze catches all the history it has. The exception is arriving
      in the mode, where it freezes as soon as it has the asked-for length - so
      sweeping left immediately may hit a wall. Trig, and the full range is
      there. Try both.
- [x] 3.3.6. **clk** does not re-capture, deliberately. With a clock running the
      chunk stays held until you trig or move amount.
- [x] 3.3.7. **feedback** bleeds new audio in: thickens, then replaces, no blow
      up.
      `fx.set(fbk="60%")`
- [x] 3.3.8. The loop seam on a pad: some click is expected, a hard pop is a
      bug.
      `drone()`

### 3.4 panner (white)

```python
sine()
fx.set(fxmode="panner", time=0.0, amt=0)
```

- [x] 3.4.1. Autopan, L and R opposite. Sum to mono - a sine autopan should
      partly cancel.
- [x] 3.4.2. **amount** up: the sway hardens to a square alternation. Listen for
      clicks at the switch points at maximum.
      `fx.set(amt="100%")`
- [x] 3.4.3. **time** to the top: ring modulation, sum/difference sidebands.
      Check for aliasing screech.
      `fx.set(time=1.0)`
- [x] 3.4.4. **trig** throws the pan across. On a sustained tone at a slow rate
      with **amount** high: every throw crosses the image, lands within ~25 ms,
      alternates sides, and does not click.
      `fx.set(amt="90%", time=0.1)`
- [x] 3.4.5. Take **time** up to ring-mod rates and trig again: the throw gets
      out of the way rather than smearing the modulation.
      `fx.set(time=0.95)`
- [x] 3.4.6. Trig the other three phase-resetting modes on a sustained tone and
      on a drum loop - **3.2 flanger** (turns the sweep round), **3.9 shifter**
      (squares R to L), **3.7 pitcher** (new window at the next grain boundary).
      Gesture yes, click no. The freezer and replayer swap the buffer under the
      playhead and are smoothed rather than avoided - judge those separately.

### 3.5 crusher (yellow)

```python
drone()
fx.set(fxmode="crusher",
       time=1.0,     # far right: the rate does nothing here
       amt=0,        # crush depth, not a mix: three bits by half travel
       level=1.0)
```

- [x] 3.5.1. **amount** to a quarter, sweep **time** the full width:
      destroyed at the left, grainy through the middle, genuinely clean at the
      right. No silent regions.
      `fx.set(amt="25%")`
- [x] 3.5.2. **time** back to the far right, sweep **amount** slowly 0 to
      half: clean to a tenth, faint grain by a quarter, obvious by a third,
      hard crunch at half. Steady the whole way - no stretch where turning it
      changes nothing.
- [x] 3.5.3. Carry the same sweep on from half to full. It should keep getting
      harsher, and brighter rather than just louder. Watch the scope: the
      waveform must keep crossing zero all the way to the top.
- [x] 3.5.4. **feedback** with a 2 V tone: thickens steadily, then above ~0.8
      tips into a howl that keeps going when you mute the input.
      ```python
      sine()
      fx.set(fbk="85%")
      ```
- [x] 3.5.5. Feedback at maximum, input muted, sweep **time**: the howl is
      pitched and tracks the rate, ~170 Hz at the left to ~1.7 kHz at the right.
      Centred on a scope at every rate - an offset instead of a tone means it
      has parked at a rail.
      `fx.set(fbk="100%", gain=0)`
- [x] 3.5.6. **trig** dips the rate, a momentary drop on sustained material.
      `fx.set(amt="40%")`

### 3.6 slicer (light green)

```python
drone()
clock()
fx.set(fxmode="slicer", time=0.0, amt="50%")
```

- [x] 3.6.1. Sweep **time**: 32 rhythms, the display's right half showing the
      number, changing at 32 distinct points.
- [x] 3.6.2. **amount** at 0 passes the drone through untouched; the first
      tenth of the travel fades the chopping in. Just past that it should be
      chopping fully, with a ~1 s decay that only breathes, down to 60 ms at
      the far right. Sweep the full range - perceived level roughly constant,
      short slices no quieter than long ones.
- [x] 3.6.3. **feedback**: chance of a step inverting. At ~0.5 the pattern keeps
      changing; at 1 it is near-fully inverted, not silent. With **stereo** at
      0 both channels must invert the same steps - sum to mono and nothing
      should change.
      `fx.set(fbk="50%", stereo=0)`
- [x] 3.6.4. **stereo** is a selector here, not a width knob: it steps the right
      channel 0 to 8 places further along the rhythm table, nine positions, so
      half travel is +4 and the top is +8. Walk all nine and check each is a
      different pairing. Do not expect the image to widen as you turn it -
      against four on the floor the widest is at a quarter, against son clave
      at three quarters. Some pairings put a very dense or very sparse rhythm
      on the right, which reads as one side chopping and the other holding.
- [x] 3.6.5. **trig** fires the envelope by hand.
      `fx.set(amt="60%")`
- [x] 3.6.6. Clicks at slice edges at the shortest decay.
      `fx.set(amt="100%")`

### 3.7 pitcher (red)

```python
sine()
fx.set(fxmode="pitcher", time=0.5, amt=0)
```

- [x] 3.7.1. Sweep **amount**: it is the shift. Crude on purpose - stutter and
      transient duplication are fine, a dead zone or drop-out is not.
- [x] 3.7.2. **time** long: rhythmic chopping. Short: formant shift on a voice
      or pad.
- [x] 3.7.3. **trig** briefly enlarges the window, a momentary smear.
- [x] 3.7.4. The display reads a plausible window size.

### 3.8 replayer (orange)

```python
sine(220, volts=2)
fx.set(fxmode="replayer",
       time=0.8333,      # +1.0x on the display: centre is a quarter speed
       amt="100%",       # fully right, the tape locked
       fbk="0%")
```

- [x] 3.8.1. Sweep **time** and check the display against what you hear:
      centre a quarter speed, the ends two octaves. No silence at dead centre.
      `fx.set(time=0.5)`
- [x] 3.8.2. Left backwards, right forwards. Cross the centre slowly on speech
      or drums: continuous, no click or jump.
      `drums()`
- [x] 3.8.3. **Clicks.** At several speeds: hit **trig** repeatedly; take
      **amount** to 0.9 and back; to 0.5 and back; jump **time**. No click, dip
      or bump, and a 2 V tone stays a 2 V tone. Speed changes glide over a few
      tens of ms - that is the tape motor, not a fault.
      `drums()`
- [x] 3.8.4. Same, overdubbing: **amount** 0.75. **trig**, retune the sine to
      110 Hz, **trig** again. Nothing once a lap.
      `fx.set(amt="75%")`
- [x] 3.8.5. **amount** fully right locks, and so does the last fiftieth of
      the travel. Easing down off the lock lets the input in gently, no cliff.
      Move further left: new audio overwrites until nothing of the original is
      left.
- [x] 3.8.6. Drop **amount** to 0.9 and leave it: nothing once a lap, from
      then on.
      `fx.set(amt="90%")`
- [-] 3.8.7. The locked loop sits at the level that went in - whatever amount
      you recorded at, and both kinds of material: non-repeating, and a
      sustained drone. Neither should drift over a dozen laps.
- [-] 3.8.8. Overdubbing a steady tone combs, and that is not a fault. Around
      half travel the two weights come out equal and the nulls go all the way
      down. Sweep for it, then move off it.
      `fx.set(amt="50%")`
- [-] 3.8.9. **feedback** applies to incoming signal only: lock the tape and
      turn it up. Little or no effect on what is playing.
      `fx.set(fbk="60%")`
- [-] 3.8.10. **trig** fills the tape over one lap, not instantly. Steady tone,
      **amount** fully right, trig repeatedly at several speeds: nothing during
      the fill or at the moment it ends.
- [-] 3.8.11. **Decide -** every fade in the mode is 10 ms, the overlap of a
      diagonal tape splice. On percussive material, does a **trig** land too
      soft? Gesture speed and splice length are one number and can be split.
      `drums()`
- [-] 3.8.12. **Decide -** the record head writes to one slot while the play
      head reads between two, leaving a tone at any speed but 1x. Run
      `artifex_probe measure` for where it sits, then open the **VCA** to sweep
      **time** through 0.85 and listen for it. An interpolated write would
      remove it and darken the tape away from 1x. Audible enough to be worth
      that?
      ```python
      fx.set(amt="50%", time=0.85, time_att=0.15)
      vcv.modulate(fx["free"], rate=0.05)
      ```

### 3.9 shifter (pink)

```python
sine()
fx.set(fxmode="shifter", time=0.5, amt="50%")
```

- [x] 3.9.1. Unity as it stands: sine in, sine out, no beating.
- [x] 3.9.2. Below centre down, above up. Sweep slowly: smooth and symmetric.
      Listen for crossfade warble rather than the pitcher's stutter.
- [x] 3.9.3. **feedback** returns the output to the input, where it is
      shifted again, and again. The mode is not a delay - it shifts by
      sweeping an 80 ms window, so each trip round the loop also lands up to
      80 ms later. **time** a few semitones above centre (the display reads
      the interval), **feedback** 0.6, sine held: a stack of intervals
      sounding *at once*, each layer another interval up and quieter than the
      last. A chord, not a series of repeats.
      `fx.set(fbk="60%")`
- [x] 3.9.4. Same on a drum hit, where the layers are spread in time rather
      than piled up: a fast cascade climbing away from the hit. Below centre
      it falls instead. Near the ends of the travel it is out of audible
      range within a few layers.
      ```python
      drums()
      fx.set(fbk="60%")
      ```
- [x] 3.9.5. **stereo**: a different shift per channel. Tiny = wide unison.
      Large = deliberately broken.
      `fx.set(stereo="20%")`
- [x] 3.9.6. **trig** collapses the stereo spread and lets it bloom back over 3 s:
      the image folds to the centre and opens out again. At **stereo** 0 there
      is nothing to collapse and it does nothing, correctly.
      ```python
      drone()
      fx.set(stereo="50%")
      ```
- [x] 3.9.7. **Decide -** the bloom is 3 s. The image is back within a quarter
      second; the rest is the pitch spread still opening under it. Too long,
      too short, or right?
      ```python
      drone()
      fx.set(stereo="100%")
      ```

---

## 4. The shared loop

Almost all of this is asserted on every build and none of it needs ears. The
suite checks the filter's reach at both slopes, both placement options, that
the dry is untouched, that stereo is mono at zero and that feedback stays
bounded; `artifex_probe measure` prints what the filter, stereo and feedback
knobs are each worth in every mode. Run those rather than turning knobs:

```
make -C test smoke_artifex && ./test/smoke_artifex | grep -E 'filter|stereo|feedback'
./test/artifex_probe measure
```

What is left is the part a number cannot answer.

```python
drums()
fx.set(fxmode="delay", time=0.8, fbk="60%", amt="80%")
```

- [x] 4.1. **Does the filter sound like a tone control?** Mode 1, long delay,
      feedback ~0.6, **amount** high, drum loop. Sweep it end to end. It has
      the reach (the suite says 39 dB); the question is whether the travel is
      usable all the way or bunched at one end.
      `fx.menu(filterInLoop=True)`
- [x] 4.2. **Decide -** the default is 12 dB/oct, a tone control that thins
      without removing. The menu's 24 dB/oct is vates' filter and takes the
      material away at both ends. Which should artifex ship as its default?
      `fx.menu(filterFourPole=True)`
- [x] 4.3. **Filter inside the feedback** (menu, delay and flanger) on a dub
      delay: long time, feedback ~0.8, lowpass half-left. Each repeat darker
      than the last, dissolving into mud. The numbers are checked; judge
      whether the dissolve is musical or just muddy.
      ```python
      fx.set(time=0.9, fbk="80%", filter=-0.5)
      fx.menu(filterInLoop=True)
      ```
- [x] 4.4. **Buffer sizes** (1.15 / 2.5 / 5 s) change mid-freeze and mid-tail.
      A glitch is fine, a crash or permanent silence is not.
      `fx.set(fxmode="freezer", amt="50%")`

## 5. Clock, pattern, LFO

All of this is now automated. The section is one piece of code shared with
vates, so `smoke_vates` exercises the section itself - the 32 rhythms and that
none repeat, the CV that selects from them, both pattern switches and both
gate windows, the LFO free and synced, its reset - and `smoke_artifex` checks
artifex's own wiring of it: the pulse high exactly while the triangle rises at
every width, pattern reset returning to step one, the external-clock menu
option, and an LFO attenuverter at zero being genuinely inert.

```
./test/smoke_vates   | grep -E 'rhythm|pattern|lfo|clock|gate|saw'
./test/smoke_artifex | grep -E 'lfo_|pattern_|external_clock|_cv_'
```

Nothing here needs ears. Two things are worth doing by hand once, because
they are about the module in a rack rather than about the code:

```python
drone()
fx.set(fxmode="slicer", amt="50%")
```

- [x] 5.1. **clk out** into another module's clock input: it should drive it
      without any fiddling with thresholds.
      ```python
      seq = vcv.module("SEQ3")
      fx["clk"] >> seq["clock"]
      scope("clk")
      ```
- [x] 5.2. An irregular or ratcheting external clock at **clk** - the tempo
      tracking should follow it rather than averaging it into mush.
      ```python
      # Random's trigger, with pulses dropped at random: a clock that keeps
      # its grid but not its regularity. A steady square tests nothing here.
      r = vcv.module("Random", internal_trigger_rate=vcv.hz(6, "LFO"),
                     trigger_probability=0.55)
      r["trigger"] >> fx["clk"]
      scope("clk")
      ```

---

## 6. Modulation

The routing is automated: mode CV wrapping and landing on clock steps, the
knob staying immediate, **step** against **free** landing quantized against
continuous, and the pattern's gate driving a mode's trig.

```
./test/smoke_artifex | grep -E 'mode_cv|mode_knob|step_cv|free_cv|pattern_gate'
```

What is left is what modulation is actually for, which no check can judge.

```python
drums()
fx.set(fxmode="delay", time=0.8, fbk="70%", amt="80%")
```

- [x] 6.1. **env** → **fbk**, attenuverter negative, mode 1, long delay, high
      feedback: repeats duck out of each hit and swell in the gaps. Then the
      same into **amount**. Does it breathe, or does it pump?
      ```python
      fx["env"] >> fx["fbk"]
      fx.set(fbk_att=-1.0)
      ```
- [x] 6.2. **Decide -** quantized mode changes (menu, default on) with fast
      mode CV and a slow clock: changes wait for the next step. Off, they are
      immediate and deliberately uglier. Is the default the right one?
      ```python
      vcv.module("LFO", freq=vcv.hz(3, "LFO"), offset=0)["saw"] >> fx["fxmode"]
      clock(hz=1.0)
      fx.set(fxmode_att=1.0)
      fx.menu(quantizeModeChanges=True)
      ```
- [x] 6.3. Pattern **cv** → **rhythm**, LFO saw → **stereo**, env → **filter**.
      Three cables, and it should never sound the same twice.
      ```python
      fx["cv"] >> fx["rhythm"]
      vcv.module("LFO", offset=0)["saw"] >> fx["stereo"]
      fx["env"] >> fx["filter"]
      fx.set(fxmode="slicer", amt="60%")
      ```
- [x] 6.4. **artifex + vates** sharing a clock (artifex **clk out** → vates
      **clk**): the two pattern generators and LFOs should agree - same
      rhythms, same phase - and stay agreed over a few minutes.
      ```python
      v = vcv.module("vates")
      fx["clk"] >> v["clk"]
      # both on one scope each, or there is no way to see them agree
      pat = vcv.module("Scope")
      fx["gate"] >> pat["ch 1"]
      v["gate"] >> pat["ch 2"]
      lfo = vcv.module("Scope")
      fx["tri"] >> lfo["ch 1"]
      v["tri"] >> lfo["ch 2"]
      ```

---

## 7. Stress and edges

Automated. The suite runs every mode with every knob at maximum on a hot
input and checks it stays finite, bounded, audible and free of DC; cycles the
modes with a long tail and the feedback up and measures the step at each
change; reads the display in all nine modes at 44.1, 48, 96 and 192 kHz and
requires the same numbers; round-trips the module's state the way Ctrl+D
does; and runs the replayer for two minutes with the feedback wandering,
checking the level neither fades nor climbs and that no DC piles up.

```
./test/smoke_artifex | grep -E 'extremes|cycling|display_reads|state_|duplicate|long_run|abuse|feedback_'
```

Three things are left, because none of them can be driven from a test binary:

```python
drums()
clock()
fx.set(fxmode="freezer", amt="50%")
```

- [x] 7.1. **Block size** 16 / 64 / 256 on the audio module in the rack - it is a
      control on the module, not an entry in Rack's engine menu. The slicing
      should land in the same place at all three, with no stutter or shift
      added by the bigger blocks.
      `fx.set(fxmode="slicer", amt="60%")`
- [x] 7.2. **Bypass** mid-tail, then un-bypass. Rack bypasses by routing the
      input to the output without calling the module, so only the host can do
      it. Confirm bypass passes audio and the module comes back alive.
      `fx.set(fxmode="delay", time=0.9, fbk="70%")`
- [x] 7.3. **Ctrl+Shift+D mid-freeze** - duplicate with cables, so the copy is
      patched in parallel and you hear both. The copy should start from its
      own silence and freeze its own fragment; if the two play the same
      frozen audio, or a knob on one moves the other, they share a buffer.
      `fx.set(amt="60%")`
