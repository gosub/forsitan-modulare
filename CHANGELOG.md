# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/)
and this project adheres to [Semantic Versioning](http://semver.org/).

## [2.14.0] - 2026-08-06
### Added
  - **caligo**, a 24 HP port of **Greyhole**, Julian Parker's 2013 algorithm
    from the DEIND project (named after the Eventide effect of a similar
    name). Greyhole is filed under "reverb" and that is the wrong shelf: it
    is a long modulated echo with a dense allpass diffusion network sitting
    in the *forward* path of its feedback loop, so each pass is scattered by
    the whole network again and every repeat comes back smeared further than
    the last. Three cascaded four-deep nested allpass stages, 24 fractional
    delay lines on prime lengths indexed by size, with the diffusion
    coefficient alternating sign across the stages.

    The engine is written from the published algorithm rather than translated
    from either upstream, and is verified against a Faust build of
    `re.greyhole`: with the test impulse placed after the reference's own
    parameter smoothers settle, the first diffuser pass is sample-identical,
    and over four seconds of tail across four parameter sets the RMS envelope
    and the spectral tilt both track the reference within 0.6 dB.

    Ten knobs, each with its own attenuverter and CV input: **time**,
    **size**, **diff**, **feedback**, **damp**, **mod**, **rate**, **mix**,
    plus two the original does not have. **spin** is Greyhole's own
    channel-rotator angle, hardcoded to pi/2 upstream, brought out to a knob:
    full CCW gives two independent mono echoes, full CW the original's hard
    interleave at every one of the twelve levels. **drift** walks **size**
    with a slow bounded random walk, so the whole scattering pattern wanders.
    **frz** freezes the loop and **sct** reseeds the scattering constants,
    both with a gate/trigger input, and the seed is saved in the patch.

    The feedback loop comes out to **snd l/r** and **rtn l/r**, normalled
    through when unpatched: whatever is patched in there colours every repeat
    and compounds pass by pass.

    One departure is not optional. The diffuser's fractional delay is Hermite
    rather than the original's first-order allpass, because the allpass
    version crackles whenever a length moves: it takes an integer tap and a
    coefficient from the remainder, so each time a glide crosses an integer
    the tap steps, the coefficient jumps, and the filter's state belongs to
    the tap it just left. That is audible in the reference too - sweeping size
    0.8 to 2.0 at 0.1 Hz with a 200 Hz sine in, a Faust build of `re.greyhole`
    puts 25 dB more energy above 5 kHz than the same thing standing still.
    Hermite is exact at integer delays, which is where these lengths rest, so
    at rest it is bit-identical to the original, and while size moves it is
    66 dB quieter above 5 kHz.

    The remaining departures are off by default or neutral at the original's
    settings, so the **greyhole** preset is the reference algorithm: the
    delay reaches 16 s instead of a Faust buffer's 1.486 s and has an
    optional pitch-bending tape mode; the prime delay lengths are rescaled by
    SR/44100 so the diffuser keeps its duration across sample rates (measured
    at 70.56-70.57 ms of diffuser latency at 44.1, 48, 96 and 192 kHz, where
    a naive port halves it between 48 and 96); the loop gains a soft
    saturator, bit-exact below +/-5 V, and a DC blocker, so feedback past
    unity is bounded rather than undefined; and the dissolve crossfade scales
    with the delay time. Seven factory presets. Clock sync on antrum's ratio
    table. ~1.3% of one core.

  - **raucus**, a 10 HP model of the four-transistor **Big Muff Pi**, the
    USA V3 of 1976-77: a 16.7 dB input booster, two common-emitter stages at
    23 and 25 dB clipped by antiparallel silicon diodes in their
    collector-base feedback, the passive two-branch tone network, and a 13 dB
    recovery stage to make back what that network takes. Circuit values and
    stage figures follow ElectroSmash's analysis of the USA V3. Polyphonic,
    one pedal per channel, with the DSP core in `raucus_dsp.hpp` and no
    Rack in it.

    Two parts are solved rather than approximated, and both are audible.

    The tone stack is the whole loaded passive network as a single biquad
    from a nodal analysis: the 39k/10n bass leg, the 3.9n/22k treble leg, the
    100k pot between them, the driving stage's 15k source impedance and the
    volume pot's 100k load. The usual shortcut, a lowpass and a highpass
    mixed by the knob, cannot move the notch, and moving is what this network
    does: 1111 Hz with the pot centred, 276 Hz at full treble. Into an ideal
    source and a light load the same expression gives 7.4 dB insertion loss
    and -14.2 dB at 1042 Hz, against the published 7 dB and -13.5 dB at
    1 kHz.

    The clipper is the exact static solution of `v + Rf*Id(v) = w`. The 1 uF
    cap in series with each diode pair is a DC block and nothing more, its
    corner across 470k being a third of a hertz, so the stage is memoryless
    and the curve solves once into a table instead of being Newton-iterated
    per sample: 4096 points on a companded sqrt index, within 0.01 mV of a
    bisection solve, costing a sqrt and a lerp at run time. The shape is the
    point. 0.18 V out at 0.25 V in, 0.29 V at 1 V, 0.35 V at 3 V: it keeps
    compressing instead of flattening, which is where the sustain comes from.

    Added, and marked as such on the panel and in the manual: an input
    **gain** trim, because Rack's nominal +/-5 V is some 25 dB hotter than
    the pickup this was voiced for and a fuzz whose character is *where* its
    gain structure lands should not be fed that silently; a **bias** trim for
    the starved, gated, lopsided sound; a **mids** control, the tone-bypass
    mod, filling the scoop rather than denting it; CV on the three real
    controls; a diode menu (silicon 1N4148, germanium, LED, lifted); 1x to
    16x oversampling, default 4x. No historical-revision presets: the
    variants differ within a single named era, and this model cannot honestly
    claim one.

  - **tundo**, a parameterized digital drum voice built after the Noise
    Engineering Basimilus Iteritas Alter, from Noise Engineering's own
    published manuals. Six tonal oscillators plus a noise oscillator are
    stacked into a modal spectrum, summed, folded and re-enveloped.
    **spread** interpolates the partial ratios from the harmonic series to
    the prime series; **harm** fades in a second tone and then extends
    first the decays and then the amplitudes of the other four, so the
    spectrum collapses towards the fundamental as the hit dies;
    **morph** runs sine to triangle to saw to square; **fold** is the
    threshold-reflection folder with amplitude compensation, and its top
    quarter mixes in a pulse train fired at every peak and trough.
    Skin, Liquid and Metal modes, a Bass/Alto/Treble range switch, both
    taking CV that overrides the switch, and an envelope output.
    Every knob has its own attenuverter and CV input.

    The engine renders on its own clock at a power-of-two multiple of the
    fundamental and the host sees it through a zero-order hold, so its
    alias images land on harmonics of the note rather than smearing: that
    tuned grit is the sound being cloned. The context menu offers a clean
    4x oversampled PolyBLEP path instead, along with 16-bit output
    quantization, an extended spread law reaching down to a detuned
    unison, free-run at full decay, the Liquid pitch depth and the output
    swing. 14HP.

  - **cartilago**, a 12 HP modulator in the manner of the **Gristleizer**,
    Roy Gwinn's 1975 Electronics Today International design, by way of the
    Chris Carter modifications that made it a Throbbing Gristle sound. One
    LFO of four shapes, with **shape** sliding the triangle's peak from a
    falling ramp to a rising one and setting the pulse's duty, driving
    either a FET attenuator or a resonant filter.

    The attenuator is a JFET in a shunt divider rather than a multiplier,
    and everything interesting follows from that. It never closes: gain
    bottoms out around -26 dB, so the troughs of a tremolo keep a thin dirty
    signal alive. It distorts most in the middle of the sweep, where the
    channel is both conducting and seeing a real drain swing, measured at
    -23 dB of second harmonic there against -37 dB shut. And it ticks,
    because gate-drain capacitance injects the control edge into the audio
    path: on the pulse setting with nothing patched in, the module clicks in
    time. All three are kept; the tick can be turned off in the menu.

    **depth** runs to 125 % so the control flattens against its ends, and
    **v/oct** takes the LFO into the audio band, where the attenuator
    becomes a ragged ring modulator. That is the case a naive build fails,
    so the shapes are polyBLEP/polyBLAMP band-limited and the signal path
    oversampled, 2x by default: at a 1.2 kHz LFO that is -22 dB of alias
    energy raw against -36 dB band-limited and oversampled. The filter mode
    is a ZDF state-variable, bandpass by default and lowpass from the menu,
    swept 45 Hz to 3.8 kHz by the same control, whose saturator doubles as
    the resonance limiter and follows the **drive** knob. Polyphonic: one
    attenuator and one filter per channel, one LFO for all of them, as it is
    one modulator in one box.

  - **turba**, a 24 HP chaotic bank taking its architecture and its interface
    from **Skrewell**, John Nowak's sound generator in the REAKTOR factory
    library. Not a port and not for want of trying by other people: Skrewell's
    chaos lives inside REAKTOR's built-in filters, which cannot be opened, and
    every attempt to rebuild it in Max/gen~ or Pd has run aground on exactly
    that. What is taken is the structure the factory library manual describes.

    Eight parallel channels, each an oscillator into a feedback delay with a
    normalizer in the loop, mixed to stereo. Three topologies differing only
    in where the filter sits: inside the loop, in front of the delay, or
    absent, with a parabolic oscillator instead of the pulse. The channels are
    cross-coupled in a ring, each oscillator frequency-modulated by its
    right-hand neighbour's loop signal and amplitude-modulated by its
    left-hand one, so the eight loops are one system. There is no gate and no
    pitch input; like the original it simply runs.

    Every channel has its own value for each of eight functions, 64 in all,
    edited as eight bars in the edit area with Skrewell's three mouse
    behaviours: **draw** sets a bar, **wrap** shifts all eight and mirrors
    them back at the ends, **rand** jogs all eight at once. Beside it is a
    Lissajous of the output, as on the original panel.

    The four macro knobs are the part worth knowing about. They do not offset
    the bars, they **map** them, applying `v^γ` to all eight at once with
    `γ = 5^-knob`: centre is the identity, hard left crushes the bank so only
    the tallest bars survive, hard right lifts the whole thing. One knob asks
    how much of a parameter the bank gets, of eight channels at once, and
    keeps their order. **pitch** moves the measured centroid from 71 Hz to
    1385 Hz, **cutoff** from 83 Hz to 1862 Hz, **delay** slides all eight
    loops from 0.17 ms (comb, ring modulation) to 134 ms (echo).

    **flow** maps the FM and AM bars, sets the engine's inertia from 1 s to
    2.5 ms, and maps resonance **backwards**: right takes the Q down. That
    inversion is Skrewell's own, and it is the thing the Max porter found and
    could not explain, "the resonance parameter in a 2-pole filter being
    turned down… a pretty surprising behavior". It is reasonable in a
    feedback loop: high Q hands the loop gain in one narrow band and it rings
    there, orderly; open it out and the loop has broadband gain for the
    saturator to fold. Measured as a largest-Lyapunov estimate, there is a
    real bifurcation on the knob: 0/s below flow -0.5, 670/s above centre.

    Each channel also carries a **two-state switch**, which is what makes the
    bank evolve with nobody touching it. Once per pass of its own delay line,
    so every 30-300 ms at eight different rates, a channel latches one bit
    from the sign of another channel's loop signal, and that bit picks between
    two values of its filter cutoff. Nothing drifts and there is no LFO: the
    sound flips. It has to be the cutoff -- switching pitch or delay time
    instead measures *worse* than not switching at all, because those move the
    sound without moving where its energy sits. Off/light/normal/wild in the
    context menu.

    The default bank is set where the thing actually moves: delays 30-307 ms,
    every loop between 0.88 and 1.0 so it builds and collapses against the
    limiter, and the eight pitches inside a fifth so they beat slowly against
    each other rather than at audio rate. Those three, in that order, are what
    decide whether the bank wanders on its own; an earlier default at half the
    feedback with 2-40 ms delays measured 0.04 octaves of spectral wander over
    an untouched minute; with the new bank and the switch it measures 0.82,
    against 0.75 for a reference recording of Skrewell standing still.

    Two more menu options, both from colB's remark that Skrewell's sound owes
    something to it "being digital with aliasing and quantization": **raw
    oscillators** drops the band-limiting from the pulses, and **bit crush**
    quantizes each loop signal to 12, 10 or 8 bits. Both are honestly small.
    Raw moves the centroid 1052 -> 1092 Hz at the default bank and the
    spectral flatness 0.011 -> 0.013, and only really shows with the pitch
    macro up (flatness 0.041 -> 0.051), because most of this engine's aliasing
    never came from the waveform edges: exponential FM at audio rate throws
    sidebands past Nyquist whatever shape the oscillator is, and polyBLEP was
    never correcting those.

    The eighth per-channel function is **filter type**, a continuous low →
    band → high morph, not resonance. That is the ensemble's `lbh` parameter:
    reading each tone generator's own input list gives its eight bars exactly
    -- `F fm A am cut lbh DEL FB` for the multimode one, `F fm A am hp lp DEL
    FB` for the bandpass one, and `F fm A am DEL FB`, six of them, for the one
    with no filter, which is precisely carloskleiber's "8 (or 6) parameters of
    8 oscillators". Resonance has no bar in Skrewell -- `res` is an input the
    tone generator feeds its levers -- and it has none here either; flow sets
    it, as it already did.

    The channel is a **pair of levers**, sixteen loops in all. That comes from
    reading the ensembles rather than the forums: every tone generator in
    Skrewell holds exactly two `LEVER` macros with a `crossvoice` between
    them, and a LEVER is not an oscillator but a whole channel -- oscillator,
    filter, resonance, normalizer, delay, feedback. Both levers of a channel
    are driven by the same bar, the second offset a tritone up with a shorter
    loop. Pairs can be switched off, which halves the CPU and, awkwardly,
    evolves more: two chaotic loops summed into one voice average each other
    out, 0.82 octaves of wander against 0.34, and reweighting the crossvoice
    against the ring does not recover it (swept at six settings). On is
    denser, rougher and faithful; off moves more; there is no setting that is
    both.

    Superseded by the above and removed, **oscillator pairs** makes each channel
    two oscillators cross-FM'ing and cross-AM'ing each other rather than one,
    was a weaker approximation of the same idea -- a second oscillator inside
    one channel, sharing its filter and delay -- written before the ensembles
    were read.

    Two deliberate departures. The normalizer only turns a loop **down**;
    built as a true normalizer, holding every channel at one level, the
    macros stop changing how loud anything is (the RMS span of the cutoff
    macro goes from 0.05-1.06 to 0.50-1.45) and the crest factor drops from
    3.2 to 2.7. The timbre still responds either way, so this is the narrower
    claim: a bank where nothing can be quiet has one dynamic. And the
    additions a Rack module wants and the original has none of: an audio
    input into all eight loops, an attenuverter and CV per macro, a chaos CV
    out, and a rand trigger. 1.15 % of a core at 48 kHz.

### Fixed
  - **sylla**'s GEN light answering almost none of the presses it acted on.
    A sample renders in 0.01 to 15 ms and the light was on for exactly as
    long as the render, so every one of the 27 engines finished inside a
    single 60 Hz video frame and the interface usually looked after the
    light was already back off. It now stays lit for 120 ms, from wherever
    a render starts, so a press blinks once and a run of them reads as a
    steady busy.
  - **antrum**'s speed LED, and **perge**'s capt and clock LEDs, sitting
    hard against the controls they belong to: 0.28mm off the knob and
    0.22mm off the jacks, close enough to read as touching. All three now
    use the offsets the rest of the collection uses. `panel_audit.py` asks
    every LED for half a millimetre of clearance now, rather than merely
    no overlap, so this cannot pass unnoticed again.
  - **sylla** dropping a GEN that arrived while the worker was still
    rendering the previous sample. Since a render is milliseconds long this
    only bit a fast clock into GEN IN, where it silently ate triggers. The
    last one is now held and started as soon as the worker is free: late
    rather than lost.
  - **textor** clicking, in both texture and rhythm mode. The loom had four
    places where a sample value could step. Every fragment that runs past
    the end of the two-second cloth wraps to the other end, where the
    waveform is unrelated, and since fragments are long and start anywhere
    most of them wrap: that one clicked several times a second, all the
    time. The other three are occasional: a voice stolen mid-note when all
    sixteen are busy, a fresh capture replacing (or RESET erasing) the
    cloth under sounding voices, and the delay tap moving to a new time or
    a new element on a reroll — that last one clicking a whole delay time
    *after* the reroll, because the step went into the delay line and came
    back out. Each now gets a short fade instead of a step, under a
    **Declick** context-menu option that defaults on: the cloth ends fade
    as they are read (the window widening with playback rate, so the seam
    always takes the same few ms whatever the pitch), the loom steals the
    quietest voice rather than the oldest, cloth changes wait ~12 ms for
    what is sounding to fade out, and the delay bus fades both what it
    reads and what it writes across a tap change. Measured with the new
    `test/textor_probe`, which highpasses the output at 8 kHz where nothing
    the engine plays belongs: the loudest transient drops from 0.87 V to
    0.013 V and the count of audible ones from 5.6 per second to none, in
    every scenario. Turning the option off restores the old behavior
    exactly.

## [2.13.3] - 2026-08-05
### Fixed
  - **imber** crashing Rack while it generates its sample bank, on Linux,
    which 2.13.2 addressed only in part. A thread running at realtime
    priority may burn only so much CPU between blocking system calls, and
    the kernel enforces that with a signal that kills the process without
    printing anything: no error, no stack trace, a log that stops
    mid-line. The reporter's desktop session sets the allowance to 200 ms
    and a bank costs around 390 ms of solid CPU, so it never finished.
    2.13.2 stopped the render inheriting the audio thread's realtime
    priority, which is the right fix and remains in place; this release
    adds the belt to those braces. The render now pauses briefly between
    buffers, which restarts the kernel's count and leaves about 2 ms of
    realtime CPU against that 200 ms allowance however the thread came
    up. A whole bank takes the same time it always did.
  - **imber** and **sylla** no longer take Rack down when a render cannot
    be allocated. Both start their worker from the audio thread, and both
    let a failure to allocate memory or to start a thread escape as an
    exception, which terminates the host rather than the module. A
    refused render now leaves whatever was already playing alone, says so
    in the log, and waits to be asked again rather than retrying on the
    next sample.
  - **imber** drew its clock and FX markers from uninitialized memory
    until the engine had run a frame, so they scattered at random for an
    instant when the module was added, and stayed scattered for as long
    as the engine was stopped.

## [2.13.2] - 2026-08-05
### Fixed
  - **imber** could take Rack down with it while it generated its sample
    bank, on Linux, both when the module was added from the browser and on
    every reseed. Nothing was wrong with the render: imber and **sylla**
    start their worker from `process()`, which is the audio callback
    thread, and that thread is realtime under JACK or PipeWire. A new
    thread inherits the scheduling policy of the thread that created it,
    so the worker came up realtime too and then computed for hundreds of
    milliseconds without ever blocking, which is exactly what the
    realtime-time limit those audio stacks install exists to stop: the
    kernel answers an overrun by killing the process. A full bank is 192
    buffers and around 50 MB, well past the usual 200 ms allowance, while
    sylla's single buffer stayed under it and so appeared to work. Both
    modules now ask for ordinary priority for their worker instead of
    inheriting. The render is the same work at the same speed, and it can
    no longer be killed by, or starve, the audio thread. Reported by
    jakulley on the [VCV community
    forum](https://community.vcvrack.com/t/forsitan-modulare-imber-causing-crashes/26023).

## [2.13.1] - 2026-07-30
### Added
  - **limen** speaks **protocol version 2**. A client could add twenty
    modules but not say where any of them went, so a generated patch landed
    in a pile: `list_modules` and `get_module` now report each module's
    `pos` in Rack grid coordinates and its `hp` width, `add_module` takes an
    optional `x`/`y`, and `move_module` moves one that already exists. Both
    placements take a `mode` for collisions (*nearest*, the default, which
    never disturbs anything else; *force*; *squeeze*; or *strict*, which
    refuses rather than landing somewhere else) and report the position
    actually reached. `save_patch` and `save_patch_as` keep the result,
    which until now needed the user to reach for Rack's file menu; loading
    deliberately has no command, since it would replace the limen module
    mid-request. `batch` runs an array of ordinary requests and returns
    their replies in order, instead of a round trip per module, per cable
    and per parameter, with `count`/`failed`/`stopped` saying how far it
    got. Clients accept a protocol *range*, so tools built for 2 still
    drive an older Rack, where the new commands simply answer *unknown
    cmd*.
  - Every module's manual page opens with a picture of its panel. Eight of
    the twenty-five had one; the rest never did.
  - `tools/release/gen_screenshots.py` and `tools/release/gen_collection.py`
    generate everything in `img/`, so no image is made by hand any more.
    The panel shots come from Rack's own renderer against a throwaway user
    dir holding this plugin alone, cropped exactly by construction; the
    collection shot arranges all 25 panels through limen, choosing the row
    split that best fills the screen, then zooms to fit and grabs it.

### Changed
  - **bulla** is described as *inspired by* Rob Hordijk's Blippoo Box
    rather than as being it. The module reimplements from scratch the
    structure Hordijk published, by way of olaf's SuperCollider realization;
    the old wording claimed more than that. The manual is also corrected on
    three counts, reported by Dave Benham: it said there was no V/oct here
    while listing four 1V/oct inputs (they exist, and they are an addition
    of this module, since the Blippoo Box has no inputs at all), it called
    the runglers 8-step shift registers when only the three newest bits ever
    reach the DAC, and it never said the **rung** jack carries both runglers
    summed. A new *differences from the Blippoo Box* section lists these and
    the digital shortcuts, so nobody has to read the source to find out what
    is and is not faithful.

## [2.13.0] - 2026-07-27
### Added
  - **antrum**, a new module: a feedback delay network reverb built after
    the Make Noise / SoundHack Erbe-Verb, from Tom Erbe's ICMC 2015 paper
    *Building the Erbe-Verb: Extending the Feedback Delay Network Reverb
    for Modular Synthesizer Use* and the hardware's manual, with the block
    layout of davemollen's GPL-3.0 dm-Reverb. Four delay lines whose times
    are mutually prime and scale together from one **size** control, from
    1 to 500 ms — a coffin to the heavens without ever changing algorithm,
    so sweeping it is a mass of coordinated doppler shifts: walls moving
    when slow, percussive when fast, FM at audio rate. Around the loop sit
    a unitary Hadamard matrix, an allpass diffuser and a one-pole
    absorption filter per branch, and a 3rd-degree Chebyshev fold driven
    by the network's own energy, so **decay** can reach 120% and sustain
    forever while the saturation and the absorption filters decide what
    "forever" sounds like. **absorb** folds diffusion and damping into one
    knob as the hardware does — diffusion over the first third, then the
    filters closing. **depth** is bipolar over modulation *type*: cyclic
    multiphase sine vibrato counter-clockwise, ergodic grain clouds
    scattering the room dimensions clockwise, and octave-up shimmer
    folded into the last stretch (unlinkable from the context menu). A
    **pre-delay** of 7 to 500 ms plays forwards or backwards, latched by
    button or momentarily by gate, and a **clk** input snaps pre-delay and
    modulation speed to ratios of the patch tempo. An analog tilt filter
    model shapes the output after the loop, and the network's own energy
    leaves as a 0-10 V CV, ready to be patched back into decay or size.
    Every one of the eight knobs has its own attenuverter and CV input.
    Eight factory presets transcribe the manual's *Emulating typical
    reverb rooms* table: coffin, room, plate, hall, heaven, ambient,
    reverse and shimmer.
  - **draen**: right-click the engine display to pick an engine by name.
    37 detents per bank is a lot of turning when you already know which
    drone you want; the list shows the current bank with the active engine
    ticked, and the bank switch rides at the top of the same menu. The
    same list is an **Engine** submenu on the panel's own menu, matching
    how sylla does it.

### Fixed
  - **Panel titles** are one size again. The title cap height was chosen
    by panel width, 3.2mm up to 90mm and 2.8mm above it, which left perge,
    guttur and quadrare wearing a smaller title than their narrower
    siblings for no reason visible on the panel. Every panel now uses
    3.2mm, and a panel that genuinely has no room says so on its own
    layout line: imber, whose buttons sit 1.25mm under the title, and
    vestigia, whose display band starts at y=9.

## [2.12.1] - 2026-07-25
### Added
  - **sylla**: the knob now selects the **engine**, one generator per
    position, and is renamed from *family* to match. It used to select a
    family and roll one of its members at every render, so the knob did not
    determine the sound: liking what you just heard and pressing GEN for
    another take on it usually moved you to a different engine instead. Now
    GEN only changes the seed. 27 engines with a weighted *random* as the
    last stop, grouped and named by family (*drone, pad, air, bell, pluck,
    phrase, dust, broken, micro*) and ordered bed to point, so the knob is
    still a gesture; the same list is in the context menu for when 28
    detents is more than you want to count through.
  - **sylla**: the generators are regrouped along with it. *ambient* is
    gone, having been a level and a register rather than an excitation,
    which is why it sounded like a blend of its neighbours: its tape pad
    *was* the pad generator with wow instead of static detune, its wash
    *was* filtered-noise drone over a quiet chord bed, its chime *was* the
    bell routine an octave up at half the level. The *fragment* grab bag of
    six unrelated recipes splits across pluck, phrase and dust; *glitch* and
    *skip* join as *broken*; *karplus* is named for what it sounds like
    rather than for who invented the algorithm. The two sustained groups
    now divide by excitation rather than register, *drone* oscillator-fed
    and *air* noise-fed, which is why comb-fed noise sits in air: measured
    spectral flatness put it at 0.11 with ~650 partials, against four drone
    siblings at 1e-5 with five to ten.
  - **sylla**: three new engines for the gaps that exposed. A sustained
    **vowel drone** (a glottal pulse train through three formants morphing
    between two vowels) joins *air*; a noise-excited **struck body** (metal
    and wood mode sets, gritty attack, short dry ring) joins *bell*, where
    every other strike was a clean additive sine tail; and a
    **stretched-partial stack** joins *drone*, carrying eight to fourteen
    partials at a shallow tilt, detuned off the integer series so the stack
    beats against itself. The four older drone generators all fall off as
    1/h squared over three to six harmonics and read as near-sines, with 2
    to 7% of their power above the fundamental against the new one's 35%.
  - **sylla**: **Random pool** in the context menu picks which engines the
    *random* position may land on, all of them by default. The two
    self-finishing one-shots are not listed, staying out of the roll as they
    stay out of imber's bank, and disabling everything falls back to the
    full pool rather than rendering silence. The pool feeds the seed, so
    changing it re-renders the current sample only when the knob is on
    *random*. v1's random is untouched: it derives its family from the seed
    exactly as it always did.
  - **sylla**: **Generator selection** in the context menu switches back to
    the v1 family knob of 2.9, where each of ten positions rolls a member.
    Patches saved before this load on it automatically and sound exactly as
    they always did, since a patch stores only a seed and reproduces solely
    under the selection that rendered it.
  - **sylla** and **imber**: selectable **root** and **scale** in the
    context menu, from the same fifteen scales the pages64 modules use.
    Pitch was hardcoded to minor pentatonic on D, so two syllas could never
    sit in different keys and a patch was locked to D minor forever. Chord
    voices follow the scale (m7 on the minor modes, maj7 on major and
    lydian, dominant 7th on mixolydian and hijaz, minor-major 7th on
    harmonic minor). The default reproduces the old hardcoded tuning
    exactly, so every saved seed still comes back as itself. On sylla,
    changing any generator setting re-renders the current seed, so what you
    hear is what a reload brings back; on imber it rebuilds all 192 buffers
    from the same seed, the old bank playing until the new one lands. The
    tuning is a setting rather than rolled material, so on imber it
    survives *Ephemeral*.
### Changed
  - Every documentation URL in `plugin.json` now points at the release's
    **tag** instead of the `master-v2` branch: the plugin `manualUrl`, all
    24 module `manualUrl`s and `changelogUrl`, 26 in all. Rack's library
    serves whatever URLs the installed build declares, so a branch link
    showed someone running an older version the manual for whatever was on
    master that day, describing controls their build did not have.
    `tools/release/sync_version.py` rewrites all 26 from the `"version"`
    field and checks them against the newest `CHANGELOG.md` heading, so they
    are never hand-maintained.
  - Module descriptions in `plugin.json` are one-line summaries again, as
    the SDK asks for. Rack shows this field as the hover tooltip in the
    module browser and does not wrap it, so the longest ones (up to 509
    characters, three sentences of feature list) stretched across the
    screen and could not be read. Longest is now 110 characters, mean 82,
    down from 509 and 182. Feature enumerations, jack lists and narrow
    asides went; what each module *is*, and whose design it ports or
    clones, stayed.
  - **Releasing** is documented in `RELEASING.md`, replacing the stale
    `checklist.md`, and quadrare's design notes moved from a working
    document in the repo root into `doc/quadrare.md`.
### Fixed
  - **sylla**: looping no longer puts a hole in the sound once per lap.
    The playhead jumped from the window end back to the buffer start,
    landing in the 25 ms fade every generated buffer carries at both ends,
    which dropped the output to 0.2-1% of its level at every wrap: an
    audible throb, worst on exactly the sustained material you would want
    to loop. Loops now run their laps past the buffer's fade-in and
    crossfade the head back in under the tail with equal-power gains, the
    way imber's read heads already did. One-shot playback still keeps both
    buffer fades, where they are the sample's own attack and release.

## [2.12.0] - 2026-07-23
### Added
  - **vestigia**, a new module: a stereo memory effect built from the
    *Vestigium* design document. An endless tape loop is continuously
    rewritten under one of three memory modes — **oblivion** (the
    present replaces the past), **remanence** (it rewrites but leaves a
    30% trace) and **sediment** (it accumulates through soft saturation
    and a DC blocker). A parallel block-based activity map, with
    hysteretic per-block gating, tracks where meaningful sound actually
    lives, so the recollection engine only ever recalls regions
    containing audio, never silence. Three recollection modes decide
    *when* fragments return: **listen** (event-centered, transient
    triggered, recent), **breathe** (rate follows the input envelope
    and density) and **dream** (recall rises as the input falls quiet,
    older and reversed and longer). Two playback heads replay the
    stored audio (up to four in High quality); each memory deteriorates
    a little more every time it returns. Macro controls for memory
    horizon, recall rate, age (band/rate/bit/jitter degradation), smear
    (all-pass diffusion), forget (memory persistence — integrity decay
    and recall wear) and temper (instability, which also scatters the
    recall timing), plus per-recollection direction and a **harmony**
    control that pitch-quantizes recalled fragments from consonant
    (unison and octaves, locked in tune with the source) through fifths
    and thirds up to a fully inharmonic detune, and equal-power mix and
    output. Playback is anti-click throughout: pitch is quantized rather
    than freely detuned, heads are never stolen mid-note, dropouts and
    fades ramp, the nearest-neighbour interpolation only appears at high
    age, and recall stays a guard band behind the write head. CV for
    all ten continuous controls (macros, direction, mix, output,
    harmony); freeze, event and clear with their
    gate/trigger inputs; event, envelope and chaos outputs; and a linear
    tape display of stored energy, the write head and recall flashes.
    The feedback path is bounded by soft saturation, DC blocking and
    non-finite guards. Beyond the MVP it implements the design
    document's later-version features: a memory-descriptor pool with
    per-region integrity (decaying as the write head passes back over a
    region) and wear-per-recollection (each replay adds permanent
    degradation), sonic-similarity weighting in breathe mode, Hermite
    interpolation that morphs toward nearest-neighbour with age,
    dropouts at high age, a modulated all-pass diffusion network with
    stereo crossfeed, cross-fed feedback, and event-centred / region /
    sub-region selection with mode-dependent pre/post-roll padding.
    Every continuous control has CV, and a MEMORY OUT taps the recalled
    signal pre-smear. A **src** switch chooses what the recall engine
    listens to (dry / mix / wet — wet is self-triggering), and an **fb**
    trimpot feeds the wet output back into the record path up into
    bounded self-oscillation. The full context menu is present: quality (Eco/Standard/
    High descriptor pool), buffer size (4/8/16/32 s), remanence
    retention, sediment input amount and character (soft/tape/fold),
    freeze behaviour, mono output, random seed with reseed, chaos-out
    stepped/bipolar, safety toggles, and bypass/patch memory options.
    The audio buffer is saved with the patch only when opted in;
    otherwise just the seed persists, keeping behaviour reproducible.
    Ships the seven factory presets from the document. 26 HP.

## [2.11.0] - 2026-07-23
### Added
  - **quadrare**, a new module: a Walsh-Hadamard codec with the
    transform domain brought out on jacks. Audio is cut into
    non-overlapping blocks, transformed into Walsh coefficients,
    handed to you, and transformed back; left alone the round trip is
    transparent to float precision and **res** sits at zero. Walsh
    functions are square waves, so a coefficient describes the sign
    structure of a short block rather than a frequency band, and the
    reconstruction is stacked squares rather than sinusoids: expect
    blockiness and grit, not a smooth EQ.
    Sixteen bipolar sliders own the lowest sixteen coefficients
    outright, one each, and **size** decides how far into the low end
    that window reaches: the whole spectrum at 16 samples per block,
    down to 0-750 Hz at 512, with 0-1500 Hz at 93.8 Hz per slider in
    the middle. Because only sixteen coefficients are ever exposed,
    every **coeff out** and **coeff in** jack is mono at every size.
    Patch one straight into the other and nothing changes, exactly;
    put a slew, a sample and hold or another quadrare in between and
    you are processing the transform domain itself. Driving **coeff
    in** with no audio at all turns the inverse transform into a Walsh
    oscillator. The **above** switch passes or mutes everything
    outside the window, which is either transparency or a lowpass at
    the window edge. **keep** retains only the largest coefficients
    and **quant** coarsens them onto a grid: the two lossy stages of a
    real transform codec, both acting on the whole block, and
    together the module at its most destroyed. **comp** breaks the
    output into its sixteen contributions as audio, **res** carries
    what was thrown away, and a row of buttons under the columns steps
    each slider through +1, 0 and -1, because on a bipolar slider the
    mute position is in the middle of the travel where a hand cannot
    find it.
    The module runs two blocks behind rather than one. Rack copies
    cable voltages once per frame and steps modules in arbitrary
    order, so a **coeff out** patched back to **coeff in** is a
    feedback cable and cannot return a block's coefficients within
    that same block; waiting a full block is what keeps the insert
    exact.

### Fixed
  - **MMCCCXCIX**: the forsitan logo was 0.06 mm off the panel's
    centreline, and the **out** badge 0.2 mm off from the jack and
    label it contains. Both are now where the panel grammar says, which
    also opens the logo-to-badge gap from 1.43 mm to 1.69 mm
  - **scando**: the **out** badge sat 0.04 mm too close to the
    bottom-right screw. The jack could not move — it is aligned with
    the four CV inputs on its row — so the badge and its label went up
    0.35 mm together, which keeps the 1 mm text padding and leaves
    ~1.1 mm to both the screw and the jack above
  - **perge**: the **mode** label sat 0.3 mm below its switch against
    a 1 mm minimum. The panel audit had been modelling a 4x10 mm slide
    switch as a radius-2.3 circle and could not see it; it now carries
    true rectangular bounds for switches and sliders, which is what
    turned this up

## [2.10.0] - 2026-07-23
### Added
  - **vespae**, a new module: the Wasp filter — the state-variable
    filter Chris Huggett designed for the 1978 EDP Wasp, in its
    Doepfer A-124 form. Built from CD4069 CMOS inverters instead of
    op-amps and run from a single unipolar supply, both to save money,
    and dirty for exactly those reasons: the inverters clip
    asymmetrically about a switching threshold that is not quite
    mid-supply, the OTAs saturate and drag the cutoff down with the
    signal, and a diode pair across the resonance network clamps the
    feedback once it gets loud. **lp**, **bp**, **hp** and **notch**
    come out simultaneously, and a fifth output **mix** is the
    A-124's own: a passive pot crossfading the lowpass and highpass
    nodes, with a CV input as on the A-124-2. It is not just a fader
    between two sounds — blending a lowpass and a highpass always
    leaves a null, and it slides from above the cutoff, through it at
    the centre, to below it, which is the manual's "asymmetrical /
    symmetrical / asymmetrical notch". An LFO on the mix CV is a
    passable phaser, and one you can move the cutoff under.
    **drive** is the Wasp's own level pot (unity at noon, where a
    ±5 V signal sits right at the rails);
    **grit** is the supply headroom, morphing between the soft
    tanh-dominated compression of a roomy rail and the hard slam of a
    mean one, and deciding how long the diode clamp lets the resonance
    run. The circuit's quirks are kept, not smoothed: maximum Q falls
    as the filter opens (≈ 10.3 at 640 Hz, ≈ 3.9 at 10 kHz), the
    resonance path is a frequency-dependent shelf rather than a plain
    gain, and the mix null smears from −70 dB to −10 dB as you drive
    it. Self-oscillation is a deliberate addition: the A-124 manual
    is explicit that the hardware cannot do it, but the last tenth of
    the **res** travel cancels the residual damping so this one sings,
    tracking 1 V/oct and drifting flat as it gets loud.
    Two trimpots are mods rather than emulation: **bias** walks the
    CD4069's switching point further off mid-supply so the two halves
    of the wave clip at different levels and the rasp turns
    even-harmonic (even/odd harmonic ratio 0.07 → 2.7 driven hard,
    with the level moving under 2 %), and **hiss** raises the inverter
    noise inside the loop, which does nothing to a loud signal but
    wanders the operating point of a loop that is close to oscillating
    (non-harmonic energy 0.4 % → 7.7 % just short of
    self-oscillation). They act in opposite regimes on purpose.
    Oversampling 1×–16× in the context menu, default 2×, about 1 % of
    one core. Topology, component values and nonlinearity shapes from
    Köper, Holters, Esqueda and Parker, "A Virtual Analog Model of the
    EDP Wasp VCF" (DAFx-22); see doc/vespae.md for what is modelled,
    simplified and deliberately changed
  - **tabes**: **dub**, an overdub button and gate with a **lvl**
    trimpot. The live input is added at the write head, so layers can
    be built up over the loop while the whole stack keeps decaying.
    The dub is laid down clean and only starts dulling on the passes
    that follow; it is added before the tape's saturation bound, so
    stacked layers compress into the ceiling instead of clipping.
    As on real tape, recording also partly erases what is already
    there, and the amount is the record level itself rather than a
    separate control: **lvl** low is gentle sound-on-sound, noon is
    the classic self-limiting balance, full is a punch-in replace,
    zero is rehearse. Punch in and out ride a ~10 ms ramp so no step
    is baked into the tape, and the input is monitored while dubbing
    in the default monitor mode. Inert while recording and on blank
    tape. **splice** is unchanged and still restores the original
    take, so it discards every dub along with the wear

## [2.9.0] - 2026-07-20
### Added
  - **vorax**, a new module: feedback drone synthesizer, a port of
    Synthux Academy's Audrey II (MIT firmware by Nick Donadson /
    Infrasonic Audio) — a Karplus-Strong string fed inaudible white
    noise self-excites inside a feedback loop of overdrive, LPF/HPF,
    ReverbSc reverb and a 1–100 ms "body" delay (right channel offset
    4 samples for stereo width); a tape-style echo outside the loop
    (50 ms–5 s, each repeat bandpassed and soft-clipped, feedback to
    1.5) with a half-time doppler switch; audio input into the loop,
    CV over pitch (V/oct), feedback gain, LPF cutoff and echo time
  - **textor**, a new module: one-knob loop weaver, a behavioral clone
    of the Fieldtone Weaver Modular, its engine tuned against signal
    analysis of published demos — captures two seconds of audio and
    weaves it into a loop of three elements (warp/weft/fleck, level
    knobs and gate outputs each): periodic clock-divider-like strands
    with per-roll random tempo (~45–300 ms steps) and loop span, soft
    asymmetric fragment envelopes, semitone-quantized pitch (random or
    sympathetic-root, panel switch), a nearly mono field with slow
    spatial drift, per-repeat jitter/mutation so loops evolve (menu:
    frozen / slow / fast, default slow), and on
    most rolls (about two in three) one element carrying decaying
    delay repeats; every movement
    of the weave knob weaves a new loop (reset and record zones at the
    knob's start, no undo), a capture landing on a silent loom starts
    playing by itself, the weave input rerolls on any CV change, a
    clock input paces the steps, and the hardware's
    restart-on-every-reroll knob-sweep behavior is a context-menu
    option (off by default); texture/rhythm switch re-renders the same
    weave; the sample is deliberately not saved with the patch, only
    the weave's seed
  - **imber**, a new module: generative rain, inspired by Giorgio
    Sancristoforo's Haiku (architecture, timing model, effect designs
    and voice behavior reconstructed by reverse-engineering; sound
    material tuned by ear, procedurally generated at seed time by a
    worker thread, never loaded from disk) — 8 looping sample players
    on a 2D field where position is routing: a player sounds only when
    a clock is within REACH (5 divisions 2n–32n, all drunk-jittered,
    the bound division sets how fast its loop window churns) and picks
    up every effect within reach (rev/lpf/hpf/bpf/bit/dly/grn/rvb, in
    Haiku's fixed order); clocks and FX live in two morphable rolled
    constellations (reroll/nudge buttons + triggers, stratified
    placement); aligned players couple (sync read heads / sync loop
    windows / diagonal jump swaps, COUPLE macro); per-player X/Y/CHG
    knobs + ½/1/2 speed and division-colored activity LED, poly X/Y CV
    (channel N → player N); skip voice (CD-skip material on the 8th
    grid) and micro voice (tiny one-shots, INSTAB macro for the
    change/variability/division-drift trio); master chain of tube
    warmth → bitcrush → gated noise inject → tape wow/flutter/age →
    soft limiter at −1 dBFS; RND (constellations + all faders except
    VOL), RESEED (new 64+64+64 bank in the background), CLR; five
    jittered gate outs, skip/micro outs, field display with bank
    progress; bank seed + constellations saved with the patch unless
    the Ephemeral menu option is on
  - **sylla**, a new module: random sample generator and player, the
    standalone voice of imber's generator library — FAMILY snap knob
    (drone, pad, fragment, bell, ambient, glitch, karplus, skip,
    micro), GEN button/trigger renders a brand new sample in a worker
    thread (busy LED, the old sample keeps playing), SPEED 0.1–2×
    with 1 V/oct CV, LEN play window, loop/one-shot and trig/gate
    modes, free-running loop when unpatched, EOC trigger out; only
    the seed is saved, a reload regenerates the identical sound
  - **guttur**, a new module: chaotic resonator drone, a port of Tom
    Mudd's Gutter Synthesis (GPL-3, also drawing on the SuperCollider
    port by Mads Kjeldgaard and Scott Carver, whose oversampling
    classes are Jatin Chowdhury's from ChowDSP-VCV) — a forced damped
    Duffing oscillator whose forcing loop runs *through* two banks of
    24 resonant bandpass biquads, so oscillator and resonators are one
    coupled chaotic system; the historic quirks are kept faithfully
    (Q indexed per filter in the biquad normalization but per bank in
    the gain terms, one Q array shared between banks, the chaos
    "lowpass" that is really a differencing step, the output tapped
    from the filter sum before the distortion), while the SC port's
    fasttan mistuning (a factor of pi) and the atan-approx 0/0 at zero
    are not; five chaos knobs (drive/tone/damp/rate/smooth) with
    attenuverted CV, resonator macros over the 20 factory banks from
    the original Max patch (per-bank select with glided morphing,
    pitch with 1 V/oct, master Q, seeded per-filter spread scatter
    saved with the patch and re-rolled by Randomize, bank gains,
    level), runtime-switchable distortion (five types, oversampled,
    1x-16x menu), filters-off raw-Duffing mode, reset button/trigger
    that re-ignites the chaos, audio input that replaces the internal
    sine forcing when patched, and a DUFF output carrying the raw
    chaotic state as a modulation source

## [2.8.0] - 2026-07-16
### Added
  - **perge**, a new module: stereo dynamic sampler and multi-effect,
    a from-scratch homage to the AC noises / BunkerNoise CONTINUA pedal —
    threshold-gated capture with sensitivity, attack and release shaping
    the dynamics-driven repeats (plus a capture gate forcing captures by
    hand); tempo knob (CV addable) or external clock
    (with multiplier menu); random per-repeat octave/fifth pitch shifts; sustain
    with a freeze zone (plus freeze button/gate); bipolar glitch/dimension
    (tempo accelerations vs. up to three layered samples), lofi/crush,
    reverb/smear (with decay) and LP/HP tilt filter; stereo spread;
    standard/reverse/tail repeats modes (panel switch); a grain-cap menu (on by default)
    that keeps each repeat to one tempo interval, or off to replay whole
    captured notes; an in-fx knob routing dry signal into the FX
    section; momentary tilt warble (button/gate); the tempo grid resyncs
    on each new capture, to the note end (default: the first repeat lands
    one interval after the note) or the note start, or off (menu)

## [2.7.6] - 2026-07-15
### Fixed
  - tabes: **eoc** and **ramp** now follow the audible head chain instead of
    the write head. With **overlap** up, a new head starts every
    loop − overlap samples, so the heard repeat is shorter than the tape
    rotation; eoc fires at each heard restart and ramp cycles once per
    repeat, instead of drifting against the audio. At overlap 0 nothing
    changes. **age** still steps once per full tape rotation (the aging
    pass), so that clock is not lost.
  - tabes: pressing **rec** with **overlap** up no longer clicks. The
    playback→monitor crossfade snapshot now retraces the overlap head chain
    (both heads, equal-power blend) the same way splice does, instead of
    reading from the write head's position.

## [2.7.5] - 2026-07-15
### Added
  - ululo gains CV inputs for **dist**, **decay** and **tone** (0.1/V, full
    knob sweep over 10V, added to the knob and clamped). dist CV moves the
    player in front of the amp with a doppler-like smear, decay CV palm-mutes
    or frees the strings, tone CV is a wah. The three jacks form a second CV
    row on the same 8HP panel (knob rows tightened to fit); the new inputs
    are appended after the existing ones, so saved patches keep their cables.
    drive stays knob-only: inside the self-limiting loop it mostly changes
    loudness, and in the metaphor it's an amp knob you set once.

## [2.7.4] - 2026-07-15
### Added
  - tabes grows to 12HP and gains four things: a **loop overlap** control —
    two play heads take turns playing the loop straight through, each new one
    starting *overlap* before the last ends and crossfading (equal power) where
    they meet, so at zero they play back to back and at the max the next repeat
    begins when the current head is halfway (the tape still ages underneath);
    an **FX send/return** whose returned signal is re-recorded onto the tape
    and so compounds pass over pass (a reverb blooms, a shifter spirals),
    bounded so a hot effect saturates into a drone instead of exploding, with a
    **send** mix knob (default 50%) + CV; a **ramp** output giving the play head
    as a loop-locked 0–10V saw (a phasor synced to eoc, clean of wow); and a
    **wow CV** input to match the existing decay CV. New I/O is appended, so
    existing tabes patches keep their mapping. The FX loop only engages when
    both **send** and **return** are patched, so a stray return can't overwrite
    the tape.
### Fixed
  - tabes: splicing no longer clicks. Restoring the pristine tape is a source
    switch (the aged read jumps to the fresh one in level and timbre), and the
    old ~2 ms additive bridge only cancelled the amplitude step, leaving a
    slope/timbre transient that could also be clipped by the output clamp. It
    now crossfades over ~10 ms between a snapshot of the aged output and the
    restored pristine tape, the same technique used for the record seam.

## [2.7.3] - 2026-07-14
### Added
  - tabes is now polyphonic: the audio in/out carry a poly cable, so a
    2-channel signal records and plays back as a coherent stereo tape (up to
    16 tracks). One shared transport drives every track — same wow/flutter,
    dropouts, seam and rec crossfade — so stereo stays phase-locked in a way
    two mono tabes never could. The tape width is fixed when you record it
    (from the input's channel count) and the output follows it; hiss is
    independent per track. AGE and EOC stay monophonic.

## [2.7.2] - 2026-07-14
### Changed
  - tabes: the default monitor mode also passes the input through while the
    tape is empty (before the first recording, after "Clear loop", after a
    too-short recording), so the module is never a dead end in a chain; the
    menu entry is now called "While recording or empty"
### Fixed
  - tabes: pressing rec no longer thumps. The monitor/playback handoff is now
    a ~10 ms crossfade rather than a step: rec-start reads the loop's
    continuation from a snapshot (recording is overwriting the tape) and fades
    it under the input monitor; rec-stop fades the monitor into the loop. The
    old additive bridge cancelled the click but left a low-frequency pulse at
    each press (worst near a couple of volts against a half-volt signal); the
    crossfade stays near the signal's own baseline. Splice keeps the bridge.

## [2.7.1] - 2026-07-14
### Fixed
  - **tabes** is now clickless: stopping a recording crossfades the loop's
    head with the live input over the first ~10 ms of playback, so the seam
    no longer clicks when the end of the recording doesn't align with the
    beginning (the pristine copy gets the same treatment, so splice stays
    clean too); abrupt output source switches (record start, record stop
    with muted monitoring, splice on aged tape) are bridged with a short
    declick ramp

## [2.7.0] - 2026-07-11
### Added
  - **rete**, a new module: feedback integrator network (after Nathan Ho's
    topology) — eight leaky integrators into a fixed random 8×8 mixing
    matrix, DC-blocking highpasses and clippers in a one-sample feedback
    loop; per-node gain knobs + CVs, leak and matrix-drive controls, excite
    input, matrix re-roll button/trigger with the seed saved in the patch,
    stereo spread and 8-channel poly outputs
  - **ululo**, a new module: feedback guitar (after Nathaniel Virgo's
    "Guitar feedback emulation") — an amp-distance delay feeds six
    comb-filter strings, tone/highpass filters and a saturating amp stage
    close the howling loop; strings retunable by polyphonic V/oct (chords
    repeat an octave up on spare strings), whammy bend, external audio in
  - **tabes**, a new module: disintegration looper — the write head
    re-records a slightly worse copy on every pass (HF loss, saturation,
    level sag, hiss, age-dependent dropouts, wow/flutter); splice restores
    the kept pristine recording; AGE CV and EOC trigger outputs; input
    monitoring follows recording by default (menu: While recording /
    Always / Never)
  - **lustro**, a new module: scanned filter — scando's mass-spring string
    drives the band gains of a 16-band resonant filterbank processing
    external audio; pluck or drive the string and the spectrum moves with
    the physics
  - **bulla**, a new module: Rob Hordijk's Blippoo Box — two
    cross-modulating triangle oscillators, two runglers (shift registers
    with 3-bit DACs) and a twin-peak filter on the oscillators' comparator;
    rungler CV output, 1V/oct CVs for oscillators and filter peaks
  - test: `new_modules_smoke`, an offline harness driving all five new
    modules through process() and checking NaNs, levels, self-oscillation,
    loop decay/splice and pluck response
  - scando, lustro: EXCITE buttons on the panel — a manual hammer hit,
    same as a trigger on the exc jack
  - tools: `panel-editor/panel_audit.py`, a clearance/overlap checker for
    @layout panels (true widget sizes, circle geometry, real OCR-A widths)
### Changed
  - panel polish across the collection: consistent label offsets (jack
    +7.5mm, knob +8.5mm, big knob +11.5mm, button +7mm), output-level LEDs
    at the top-right corner of their output badge (one per badge on stereo
    pairs), corrected widget sizes in the panel editor, label nudges on
    scando/MMCCCXCIX/dræn, and pellicula's left-gutter text placement plus
    a badge-grey output bar

## [2.6.16] - 2026-07-10
### Added
  - test: `draen_sweep`, an offline octave sweep (27.5 Hz → 3.52 kHz) of both
    dræn engine banks reporting per-channel DC, AC RMS, peak and NaN counts
  - MMCCCXCIX: delta-sigma oversampling selectable from the context menu
    (1×/2×/4×/8×/16×, saved with the patch); the new default 8× sounds
    identical to the previous hardcoded 16× at half the CPU
### Changed
  - MMCCCXCIX: the delta-sigma loop and parameter handling were optimized
    (dead demod pole removed, cheaper TPDF dither, single-pass RAM bit
    exchange, coefficient math only runs while a knob/CV moves); combined
    with the 8× default the module's CPU use drops to roughly a third
### Fixed
  - dræn: a MinBLEP discontinuity landing within float rounding of a sample
    boundary read past the impulse table and permanently NaN'd the voice
    (the thx engine reliably died above ~1.7 kHz)
  - dræn: DC offsets in twelve dronecaster-bank engines (sunno reached -0.85
    at 27.5 Hz) removed with output DC blockers, leaving the SynthDef-faithful
    interiors untouched
  - dræn: the band-limited saw leaked DC proportional to its frequency (a
    minBLEP residual artifact), audible as offset on saw-heavy engines in
    both banks (wall, anthem, supersaw)
  - dræn: DC offsets across the hyf bank — feedback loops (mirror, wire,
    quill, rain, pulsework, hive), tracking lowpasses over noise (turbine,
    ember), waveshaping and near-zero PM sidebands (root, sputter, aster)
    now go through DC blockers; existing blockers relaxed to a 7.6 Hz cutoff
    so 27.5 Hz fundamentals keep their level
  - dræn: hyf engine levels no longer swing with pitch — engines whose
    loudness genuinely depends on hz (choir, breath, bowl, gong, tide, rain,
    mirror, pulsework, frost) get an octave-table makeup gain interpolated
    in log2(hz); quill's gain doubled (its old level was mostly the drift)

## [2.6.15] - 2026-07-09
### Added
  - limen: `get_module_info` protocol command — the metadata Rack shows in a
    module's right-click Info menu: model description, tags and links, plus
    the owning plugin's brand, version, license, author and URLs
### Changed
  - tools: the limen CLI clients (former `tools/cli/`) moved to their own
    repository, [gosub/limen-tools](https://github.com/gosub/limen-tools),
    renamed `limen` → `limen-cli`, with a new `info` command, Windows
    support, and CI-built binaries for Linux/Windows/macOS on its releases
  - tools: loose scripts organized into `typography/`, `panels/` and
    `patches/` subdirectories

## [2.6.14] - 2026-07-09
### Added
  - cumuli: super-slow mode in the right-click menu — both rates 100x slower
    (0.0001 V/s to 1 V/s, center default 0.01 V/s), knob tooltips rescale,
    saved with the patch
### Fixed
  - dræn: loading a patch with a saved engine now fades that engine in from
    silence, instead of playing engine 0 first and fading out of it

## [2.6.13] - 2026-07-09
### Added
  - dræn: a second engine bank — **hyf** (Old English for *hive*), 37 original
    drone instruments built on the same UGEN layer, selectable from the
    right-click "Engine bank" menu and saved with the patch; switching banks
    fades like an engine change
  - the hyf roster deliberately covers ground the dronecaster set doesn't:
    binaural beating (beam), Shepard tones (shepard), CZ phase distortion
    (phase), wavefolding (fold, corona), formant/vowel drones (choir, breath,
    eclipse), octave-up shimmer feedback (halo), bowed and plucked strings
    (wire, quill, rain), singing bowls and gongs (bowl, gong), and
    environmental textures (tide, ember, veldt, frost, turbine)
  - all 37 verified: level-matched to the first bank, no NaNs, feedback
    engines stable over 60 s at 40/440 Hz; the whole bank is light on CPU
    (every engine ≤ 1.4% of real time at 48 kHz)

## [2.6.12] - 2026-07-09
### Changed
  - dræn: DSP optimization pass, ~26% less CPU overall and the heaviest engine
    (hecker) cut from 8.8% to 2.7% of real time — filter and lag coefficients
    (biquad, Ringz, MoogFF, SVF, BAllPass, Lag/LagUD, Amplitude) now refresh on
    16-sample blocks instead of every sample (SC itself uses 64-sample control
    blocks, so this is finer-grained than the original), Env.perc advances its
    exponential shape incrementally, and hecker's pow/trig control mappings are
    evaluated at the same block rate
  - verified against the pre-optimization build: per-engine RMS unchanged
    within ±0.2%, all feedback engines stable over 60 s at 40/440 Hz

## [2.6.11] - 2026-07-09
### Added
  - dræn: the last four dronecaster engines, all @zebra — **unmemqua** (28-partial
    Klank torn by breathing filters), **uneablin** (cross-delayed PM sine ring
    with waveshape morphing), **unwealne** (wandering pulses octave-shifted
    through a 13-ratio bank), and **unreanth** (buffer-sequenced sine fades over
    ring-mod saws and a long pitch-smeared feedback delay) — **completing the
    full 37-engine dronecaster roster**
  - draen_ugens.hpp gains PitchShift (two-tap granular shifter), LagUD, and the
    distort / InsideOut / DiodeRingMod waveshapers; Ringz gains an SC-exact
    un-normalized mode (long partials ring louder, as in SC's Klank)

## [2.6.10] - 2026-07-09
### Added
  - dræn: two more dronecaster engines — **takita** (@sixolet, a self-clocked
    flip-flop drum language of resonant filtered clicks) and **twin pks**
    (oscillator-free tape-noise horror: compressed, band-passed, wow/fluttered,
    saturated and bit-crushed)
  - draen_ugens.hpp gains Phasor (resettable ramp) and Decimator (sample-rate /
    bit-depth reducer)

## [2.6.9] - 2026-07-09
### Added
  - dræn: three more dronecaster engines — **sunno** (Karplus-Strong doom
    guitars through crossover distortion and cascaded tanh stages), **nautilus**
    (@taubaland, Lorenz-driven sine-grain clouds), and **drumm**
    (@infinitedigits, crossfading bass/melodic layers in pumping Freeverb)
  - draen_ugens.hpp gains Pluck (Karplus-Strong), LorenzL (sub-stepped Euler with
    divergence guard), FBSineN, a high-shelf biquad, and crossover-distortion and
    sine-shaper waveshapers

## [2.6.8] - 2026-07-08
### Added
  - dræn: three more dronecaster engines — **eno** (@infinitedigits, Music for
    Airports: chorused chord saws, a Klank, and a comb-string "piano" walking
    Eno's note rows), **belong** (@infinitedigits, saws overdubbing a 16-beat
    tape loop, with a kick gated behind amp > 0.7), and **ruins** (@rplktr &
    @sixolet, self-clocked metallic FM hits in a long wow-and-flutter wash)
  - draen_ugens.hpp gains Decay2, Compander, a peaking-EQ and resonant-highpass
    biquad, and a GVerb approximation (8 damped combs, odd/even split to stereo)

## [2.6.7] - 2026-07-08
### Added
  - dræn: three more dronecaster engines — **gristle** (@infinitedigits, octave
    triangle-saws through a Greyhole cloud), **grove** (@sixolet, five self-gating
    pulsar-synthesis voices washed through Greyhole), and **shields**
    (@infinitedigits, double-combed saw pairs re-pitched off a slipping tape loop)
  - draen_ugens.hpp gains VarSaw, SetResetFF, Trig1, a curved Env.perc generator,
    a peak Limiter, the CombN-bank reverb block several engines share, and an
    approximation of Julian Parker's **Greyhole** (modulated allpass diffusers in
    a damped cross-fed stereo delay loop)

## [2.6.6] - 2026-07-08
### Added
  - dræn: four more dronecaster engines — **mt. zion** (@license, five S&H-wandering
    pulse harmonics), **mika** (@infinitedigits, allpass-retimed sine pings over a
    pulse+noise bass), **fieldsteel** (after Eli Fieldsteel's Tutorial 15, demand-
    picked band-passed saws with a resonant "marimba"), and **malone**
    (@infinitedigits, eight organ voices stepping a demand-sequenced chord table)
  - draen_ugens.hpp gains the demand-rate layer — Dseq, Drand, Dxrand and Dbrown
    generators polled on trigger edges — plus TExpRand, TDelay, CoinGate, an
    interpolated AllpassC and a midiratio helper

## [2.6.5] - 2026-07-08
### Added
  - dræn: two more dronecaster engines — **toshiya** (@infinitedigits, interval-
    jumping sines with a Klank resonator bank) and **magicicada** (@sixolet, a
    no-input-mixer feedback drone with crossfading delay banks)
  - draen_ugens.hpp gains Ringz (the resonator behind Klank), BrownNoise, a
    second-order BAllPass, a TPT state-variable filter (SVF), and an N-element
    SelectX crossfade

## [2.6.4] - 2026-07-08
### Added
  - dræn: three more dronecaster engines — **unrelacc** (@zebra, six Hénon-map
    chaotic oscillators in intervals), **dreamcrusher** (@infinitedigits, a
    no-input-mixer feedback drone), and **rehberg** (@infinitedigits, folded and
    DFM1-filtered tape-warble pulses drenched in Freeverb)
  - draen_ugens.hpp gains a faithful port of Jezar's public-domain **Freeverb**
    (8 damped combs → 4 allpasses per channel), plus HenonC, LFSaw, fold,
    Changed, Amplitude, OnePole, Balance2 and a DFM1 filter approximation

## [2.6.3] - 2026-07-08
### Added
  - dræn: four more dronecaster engines — **starlids** (@infinitedigits, PWM sub
    + 12 interval-stepping saws through a Moog ladder), **mt. lion** (@license,
    9 comb-resonated pulse voices driven by sample-and-held noise), **apparatus**
    (Josue Arias, clipped-triangle generators with mains hum and a crackle bed),
    and **eliane** (@sixolet, 7 sines phase-modulating in a feedback ring — an
    Éliane Radigue homage)
  - draen_ugens.hpp gains CombN, LFPulse, LFPar, Dust2, Crackle, and softclip /
    Rotate2 helpers; SC LocalIn/LocalOut is modelled as a one-sample feedback bus
### Fixed
  - draen_ugens.hpp: the per-voice RNG now avalanche-hashes its seed, so nearby
    seeds (s, s+7, …) decorrelate — xorshift alone gave correlated first outputs,
    which could e.g. clip all of Eliane's amplitude gates to zero (silence)
  - draen_ugens.hpp: combFeedback now handles negative decay times (negative
    feedback of equal magnitude), matching SC's comb behaviour

## [2.6.2] - 2026-07-08
### Added
  - dræn: two more dronecaster engines, both @infinitedigits — **coil**
    (12 Dust-triggered feedback-sine/noise voices through a long reverb) and
    **sachiko** (4 DPW-pulse voices modulated by slow triangle banks, into a
    global Moog ladder and reverb)
  - draen_ugens.hpp gains a reusable delay-line layer: interpolated delay lines,
    CombL/CombC feedback combs, Schroeder AllpassN, and a shared SchroederReverb
    (DelayN → 7×CombL → 4×AllpassN) — the reverb block copied across many
    dronecaster SynthDefs, now built once
  - draen_ugens.hpp also gains Impulse, Trig, TChoose, SinOscFB, a breakpoint
    EnvGen, an ASR attack env, a Moog ladder (MoogFF) and LeakDC
  - dræn: engine init now receives the sample rate, so delay-based engines size
    their buffers correctly and rebuild on sample-rate changes

## [2.6.1] - 2026-07-07
### Added
  - dræn: three more engines ported from dronecaster — **harm's way**
    (@moonblind, 16 amplitude-modulated harmonics), **thx** (@infinitedigits,
    the THX Deep Note sweep, with amp as the sweep position), and **hecker**
    (@infinitedigits, stereo banks of filtered white/pink noise)
  - dræn: per-engine makeup gain so engines are loudness-matched and switching
    between them no longer jumps levels
  - draen_ugens.hpp gains LFNoise2, WhiteNoise, PinkNoise, Dust, Latch, Lag
    (VarLag), BLowPass, and the Pan2 / SelectX / linexp / linlin / midicps
    helpers needed by the new engines

## [2.6.0] - 2026-07-07
### Added
  - dræn: drone synthesizer, a port of the dronecaster norns instrument
    (github.com/northern-information/dronecaster, GPL-3.0). A bank of drone
    engines is played from two controls, fundamental (hz) and level (amp), with
    a third selecting the engine; switching fades the current engine down and
    the next one up, mirroring dronecaster's SynthSocket
  - dræn: hz knob + CV (right-click switches the CV input between 1V/oct and
    linear 100 Hz/V), amp knob + CV, and an engine knob + CV with a runtime
    display of the selected engine's name
  - dræn: fade time selectable from the context menu (0.25 s .. 8 s)
  - dræn: initial engine roster — sine, square, triangle, supersaw — ported
    faithfully from the original SynthDefs, with author credits preserved
  - a reusable SuperCollider-UGEN DSP layer (src/draen_ugens.hpp) underpins the
    engines: SinOsc, LFTri, band-limited Saw/Pulse (via Rack's MinBLEP), the
    SC second-order filters over Rack's biquad, LFNoise0/1 and Splay — a
    vocabulary for future SC-to-C++ ports

## [2.5.1] - 2026-07-07
### Added
  - pellicula: "Shift all samples +8 / -8" context-menu actions advance or rewind
    every voice's sample selection by 8, wrapping at 64 — from the default 1-8 each
    click steps the whole module to the next contiguous bank, to audition a 64-sample
    kit eight sounds at a time

## [2.5.0] - 2026-07-07
### Added
  - pellicula: "exploded" 8-voice one-shot drum sampler in the spirit of the Erica
    Synths Pico DRUM sample-player engine, rebuilt clean-room from the published
    manual (12-bit / 44.1 kHz character, pitch, decay and level per voice)
  - pellicula: full control matrix — every voice has a knob and a CV input for
    sample-select, pitch, decay and level, plus a manual trig button and a trig input
  - pellicula: poly normalling — each input row has a poly jack (channel N drives
    voice N) alongside 8 mono jacks; a patched mono jack overrides its voice
  - pellicula: sample-select is 1V/oct semitone-quantized (0 V = the knob's sample)
  - pellicula: no samples are bundled — a global "kits folder" is set from the context
    menu, and each immediate subfolder is a selectable kit of up to 64 .wav files
    (mono/stereo, 8/16/24/32-bit int or float, any rate) ordered by filename; kits load
    on a background thread and swap in glitch-free, and the choice is saved with the patch
  - pellicula: per-voice outputs, an 8-channel poly output and a summed mix output
  - pellicula: per-voice choke groups and a 12-bit-playback toggle in the context menu
  - pellicula: 22HP matrix panel generated by tools/gen_pellicula_panel.py
### Fixed
  - panel-editor: resolve label characters through the font cmap so digits and
    punctuation bake correctly (previously only glyphs named by their character,
    i.e. letters, rendered)

## [2.4.0] - 2026-06-14
### Added
  - scando: scanned-synthesis oscillator (Verplank / Mathews / Shaw technique) — a
    fixed-end mass-spring string (the non-circular topology of Csound's scansyn /
    Qu-Bit Scanned) forms a slowly-evolving wavetable scanned at audio rate for a
    pitched, organically-shifting tone
  - scando: Mass, Stiffness, Damping, Centering and Hammer-shape controls reshape the
    string dynamics; Shape morphs the hammer between sine, saw, noise and dual-pulse
  - scando: Strength control drives the string continuously with the hammer shape for
    a self-sustaining tone; Update Rate sets the string's physics rate (~500 Hz–8 kHz)
  - scando: Fine tune (±7 semitones); Inject audio input with an In-Level attenuator
  - scando: EXCITE trigger hammers the string to the current shape (a pluck)
  - scando: 1V/oct pitch input and CV inputs for mass, stiffness, damping, centering,
    shape, strength and rate
  - scando: audio output with a self-levelling limiter, level LED, 16HP panel

## [2.3.1] - 2026-06-14
### Added
  - limen: `hello` command — protocol version and capability discovery
  - limen: `get_param` command — read back a single parameter's value and metadata
  - limen: window/view commands `set_fullscreen`, `zoom_to_modules`, and `quit`, for scripting patch screenshots
  - cli: Python limen client (`tools/cli/limen.py`) alongside the C client
  - cli: subcommands for the new protocol commands (`hello`, `param`, `fullscreen`, `zoom`, `quit`)
  - tools: `gen_patches.py` and a minimal `patches/limen.vcv` that launches Rack straight into a server-enabled, controllable state
  - tools: `gen_title_paths.py` and `measure_text.py` for generating OCR-A panel titles
  - docs: README section on controlling Rack externally via limen, including the loopback-only security model

### Fixed
  - alea: guard against undefined behaviour when no modules are available

### Changed
  - tools: reorganised `tools/` into `cli/` and `panel-editor/`, with a panel-editor README

## [2.3.0] - 2026-05-09
### Added
  - MMCCCXCIX: PT2399 delay chip emulation with feedback send/return loop
  - MMCCCXCIX: CV inputs for all parameters (time, feedback, mix, brightness, fb loop mix)
  - MMCCCXCIX: external feedback send/return loop with normalled bypass and blend control
  - MMCCCXCIX: soft compressor on wet output to limit self-oscillation amplitude
  - tools: panel-editor.py — browser-based drag-and-drop panel layout editor

## [2.2.1] - 2026-03-14
### Added
  - limen: `list_ports` command to query input/output port names by module id
  - limen: `list_models` command to enumerate available models (optional plugin filter)
  - limen: module filter and verbose mode (`outputModuleName`, `outputPortName`, etc.) for `list_cables`
  - cli: cable id prefix resolution for `disconnect`
  - docs: per-module documentation pages in `doc/`
  - docs: Latin naming section and About page in readme

## [2.2.0] - 2026-03-08
### Added
  - limen: TCP + JSON control interface for controlling VCV Rack from external tools
  - limen: right-click menu to enable/disable server and select TCP port
  - cli: limen command-line client (list modules/plugins, add/remove modules, connect/disconnect cables)

### Fixed
  - limen: Windows (win-x64) build compatibility via Winsock2 shim

## [2.1.0] - 2023-01-10
### Added
  - cumuli: reset input gate and button
  - cumuli: up and down buttons
  - cumuli: monopolar/bipolar selector

## [2.0.2] - 2023-01-01
### Fixed
  - cumuli: corrected z-order of output white outline

### Changed
  - forsitan: updated github action build script (arm64 support)

## [2.0.1] - 2022-02-07
### Added
  - readme: screenshots of v2.0
  - interea: bypass behaviour
  - interea, cumuli, deinde, pavo: labels to lights, inputs, outputs

## [2.0.0] - 2022-01-30
### Changed
 - forsitan: recompiled the plugin with Rack SDK v2.0.x

### Fixed
 - alea: modified module spawn code to compile with Rack v2.0.x

## [1.4.2] - 2021-05-16
### Changed
- alea: simplified panel svg code
- cumuli: simplified panel svg code
- deinde: simplified panel svg code
- pavo: simplified panel svg code

### Fixed
- plugin.json: corrected the link to the CHANGELOG on github
- changelog: fixed sub-headers indentation

## [1.4.1] - 2021-05-13
### Fixed
- interea: the selection of the chord quality when the harmonic option
is on is now based on the V/Octave input only, using thus the frequency
knob as the root note of the scale.
