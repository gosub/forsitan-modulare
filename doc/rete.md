# rete

**Feedback integrator network — a self-oscillating chaotic instrument.**

*rete* is Latin for "net". The module is an implementation of the *feedback
integrator network* topology described by Nathan Ho in
[Feedback integrator networks](https://nathan.ho.name/posts/feedback-integrator-networks/),
itself inspired by Giorgio Sancristoforo's Bentō synthesizer.

Eight signals circulate in a loop, one sample per pass:

1. **leaky integrators** (one per node)
2. a fixed **random 8×8 mixing matrix**
3. first-order **DC-blocking highpass filters**
4. hard **clippers**, whose outputs feed back to step 1

The clippers keep everything bounded, the highpasses keep DC from
accumulating, and the matrix cross-couples all eight nodes. Depending on the
matrix and the controls, the network settles into steady oscillation, ticking
rhythms, screaming sweeps, or full chaos. With nothing patched in it plays
itself; a tiny internal noise floor keeps the zero state from being a dead
equilibrium.

## Controls

| control | function |
|---------|----------|
| **G1..G8** (top grid) | per-node gain into the matrix, each with a CV input below it (±5V, added to the knob) |
| **leak** | integrator leak. Left: leaky/thin, fast decay. Right: nearly pure integration, heavy sustained lows |
| **scale** | matrix drive, 1×–1000× (exponential). The single most powerful knob: it pushes the loop from silence through tones into saturated chaos |
| **in lvl** | level of the **in** jack into the network |
| **in** | external audio/CV excitation, injected into all nodes (alternating sign). Try an impulse train or a drum loop |
| **rnd** button / **trig** | re-roll the mixing matrix (new random seed). This is "next patch, please": every seed is a different instrument |
| **L / R** | the eight nodes spread across the stereo field, equal-power |
| **poly** | all eight node outputs as an 8-channel polyphonic signal (±5V per node) — feed it to pavo, a poly VCA, a poly filter... |

The matrix seed is saved with the patch, so a patch always reopens sounding
the way it was saved.

## Tips

- Modulate the **G** inputs with slow LFOs (Ho's own recommendation): the
  network drifts between behaviors instead of jumping.
- **scale** low + **leak** high gives slow, bassy, breathing drones;
  **scale** high turns the same seed into harsh digital noise.
- The 8 poly channels are eight *different* mixes of the same organism:
  splaying them (pavo) gives a wide, coherent stereo texture.
- If a seed lands on something boring, press **rnd**. There are 4 billion
  of them.

## Attribution

The topology and the interface approach (fixed random matrix, modulated
per-node gains) are from Nathan Ho's post; the implementation is original.
