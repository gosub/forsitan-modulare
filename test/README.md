# test — offline harnesses

These are standalone programs that exercise DSP code outside of Rack.
They link against `libRack` from the SDK, so `RACK_DIR` must point to it
(same as the plugin build).

```
cd test
make
./draen_sweep            # both banks
./draen_sweep hyf        # one bank: draen | hyf
```

## draen_sweep

Renders every dræn engine at amp = 1 across octaves 27.5 Hz → 3520 Hz
(27.5·2^k, k = 0..7) with a fixed seed, 5 s warmup and an 8 s measurement
window, and prints CSV:

```
bank,engine,hz,dcL,dcR,rms_ac,peak,nans
```

- `dcL`/`dcR` — per-channel mean: any value above ~0.005 is a DC leak.
- `rms_ac` — RMS after removing the mean, pooled over both channels.
  The bank targets roughly 0.15–0.3 at amp 1 (sparse percussive engines
  sit lower by design); a large swing across the octaves for one engine
  means its perceived level depends on hz and its makeup gain needs work.
- `peak` — max absolute sample; watch for values well above 1.
- `nans` — count of non-finite samples (must be 0).

## perge_render

A utility (not a self-running check) that feeds an audio stream through
`Perge::process()` and writes the result, for A/B-ing the module against real
audio outside Rack. It reads/writes **raw 32-bit float** (do WAV conversion in
the caller, e.g. `ffmpeg`/`soundfile`), so it carries no bundled test asset.

```
perge_render <in.f32mono> <out.f32stereo> [KEY=value ...]
```

`KEY`s are param names (MIX, TEMPO, PITCH, SUSTAIN, GLITCH, LOFI, RVRB,
FILTER, SENS, THRESH, ATTACK, RELEASE, MOD, DECAY, SPREAD, INFX) or the
switches/menu members `repeatsMode` / `clockMult` / `altRouting` / `grainCap`,
plus `seed`, `tiltStart` / `tiltEnd`, `freezeAt` (seconds). Input is ±1 (driven
at ±5 V internally); it appends a 6 s tail so repeats and reverb ring out.
