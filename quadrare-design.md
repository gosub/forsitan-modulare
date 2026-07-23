# quadrare — design decisions

Working document for the Walsh codec module. Supersedes the original
`forsitan_walsh_codec_implementation_spec.md` wherever the two disagree.
Not the user manual; that becomes `doc/quadrare.md` once the module exists.

## Identity

A patchable Walsh–Hadamard codec. Audio in, FWHT, per-band manipulation
(internal or patched), IFWHT, audio out. Transparent when untouched; the
character comes from what you do to the transform domain, and from the fact
that the transform domain is exposed as real jacks rather than hidden behind
an interface.

The name is `quadrare`, the infinitive: *to square, to make square, to make
fit*. Walsh functions are square waves, and the transform squares the signal
into them. The infinitive is deliberate, breaking the first-person pattern of
`scando` / `ululo` / `lustro`.

## Panel

32HP (162.56mm), 16 vertical columns, one per band. Each column is:

```
VCVLightSlider<GreenRedLight>   bipolar gain, with the coefficient lit in the handle
COEFF OUT jack                  poly, that band's bins
COEFF IN jack                   poly, that band's bins
```

`VCVSlider` measures 19.8426 × 76.535 px = **6.72 × 25.92 mm**, so 16 of them
fit easily. The jacks set the column pitch: 8mm jack + 1.5mm clearance means
9.9mm pitch, 158mm of span, 5.5mm side margins at 32HP.

`VCVLightSlider` carries a light in the handle, which serves the spec's
bipolar per-coefficient LED with no separate LED row. Green positive, red
negative, brightness on |c|, smoothed and updated at reduced rate.

The utility and audio elements share one row near the bottom (32HP is wide
enough that 10 elements sit at ~15mm pitch):

```
AUDIO IN | OUT LEVEL | SIZE | KEEP | QUANT | DRY/WET | FREEZE | COMPONENTS | RESIDUAL | AUDIO OUT
```

`panel_audit.py` does not know slider geometry yet and will need
`VCVSlider` (6.72 × 25.92mm) added before it can check this panel.

## Band split

16 bands of **equal width**: band `k` covers bins `[k*N/16, (k+1)*N/16)`, so
every band holds `N/16` bins and every jack carries `N/16` poly channels.

Uniformity is a hard requirement, not an aesthetic preference. A split with
unequal band widths (the quadratic one considered first, which bought a
log-ish frequency spread) gives jack 0 one channel and jack 15 sixteen. That
is invisible from the panel, and it makes the OUT jacks unmergeable with each
other. Padding short jacks to 16 channels with zeros was also rejected: jacks
where 15 of 16 channels do nothing are more confusing than variable width, not
less.

The cost is that the frequency split is **linear**. Band `k` starts at
`f_k = k * fs/32`, or `k * 1500 Hz` at 48 kHz, so slider 0 owns everything
below 1.5 kHz and the other fifteen divide 1.5 kHz to Nyquist. This is
top-heavy for most musical material and it is the accepted price of uniform
poly width.

One good property survives: `f_k = k * fs/32` has no `N` in it, so **the band
frequencies do not move when SIZE changes**. Slider 5 is always about 7.5 kHz.
SIZE changes only the resolution inside each band, the block rate, and the
latency.

At N=16 every band is exactly one bin, so the panel *is* the transform: 16
sliders, 16 signed Walsh coefficients, one each, all jacks mono. That is the
spec's 16-point expansion target, realized as the base case.

## SIZE

`16 / 32 / 64 / 128 / 256`, snapped 5-position knob. 16 bands times the
16-channel poly cap makes 256 the natural ceiling.

| N | ch per jack | block rate @48k | latency | bin spacing |
|---|---|---|---|---|
| 16 | 1 | 3 kHz | 0.33 ms | 1500 Hz |
| 32 | 2 | 1.5 kHz | 0.67 ms | 750 Hz |
| 64 | 4 | 750 Hz | 1.3 ms | 375 Hz |
| 128 | 8 | 375 Hz | 2.7 ms | 187 Hz |
| 256 | 16 | 187 Hz | 5.3 ms | 94 Hz |

SIZE does not change which frequencies the sliders address, only the block
rate and the resolution within each band. That block rate is nonetheless the
module's biggest sonic parameter: N=16 is a grit box whose FREEZE is a 3 kHz
whistle, N=256 is a spectral processor whose FREEZE is a 187 Hz drone.

Size changes apply at the next block boundary, same rule as FREEZE. The poly
channel count is worth printing on the panel beside the SIZE knob.

N=8 is dropped: 8 bins cannot fill 16 columns.

## COEFF OUT / COEFF IN

Per-band poly jacks carrying that band's bins, signed, one channel per bin,
scaled `1/N` for volts as the spec's section 10 specifies. All 16 jacks carry
the same number of channels, `N/16`, so they are interchangeable and mergeable.

This is the whole point of the 16-column layout: **every coefficient is
exposed, bit-exact, in both directions**, so none of the compromises that a
single 8-channel poly port would have forced are needed. No level/shape
encoding, no representative bins, no averaging, no Coefficient/Level mode
switch. A plain OUT→IN cable is identity. Feeding COEFF IN with no audio
present drives the IFWHT as a standalone Walsh synthesizer.

`Overlay` (default) and `Replace` from spec section 7 are kept, as a context
menu item, now operating per channel within each band jack:

- Overlay: present channels external, absent channels internal.
- Replace: present channels external, absent channels zero.

Internal slider gains apply before the external substitution, so a replaced
channel is not multiplied by its slider (spec 7.3 unchanged).

## KEEP and QUANT

These replace `INPUT LEVEL`, which on a purely linear path was mathematically
the same knob as `OUTPUT LEVEL`. They are the codec's lossy stage and they are
what makes the name honest.

- **KEEP**: rank thresholding. Retain only the `k` largest-magnitude
  coefficients of the block, zero the rest. `k` from 1 to N. Classic transform
  coding; at low `k` the signal collapses to its strongest Walsh components.
  Scale-invariant by construction.
- **QUANT**: quantize each coefficient to a coarse grid before the inverse.
  Walsh-domain bit reduction, which smears into blocky square-wave artifacts
  and sounds nothing like PCM crushing.

Both act per bin, below the band level that the jacks address. Order in the
chain: FWHT → slider gains → KEEP → QUANT → COEFF OUT / COEFF IN substitution
→ IFWHT.

**Open:** QUANT's grid is currently specified as absolute, which makes input
drive level meaningful again and would argue for restoring INPUT LEVEL as a
drive control. The alternative is to make the grid relative to the block's
peak coefficient, keeping it scale-invariant like KEEP and leaving INPUT LEVEL
dropped. Defaulting to relative until there is a reason not to.

## Kept from the original spec, unchanged

- Non-overlapping blocks, no windows, no overlap-add. Buffering written so OLA
  could be added later without restructuring.
- `RESIDUAL OUT`: `dry_delayed - wet`, independent of DRY/WET position.
- `COMPONENTS OUT`: poly audio, per-band time-domain contribution, summing to
  the wet signal before output gain. With 16 bands this is 16 channels, at the
  poly limit.
- `DRY/WET`: linear crossfade against a latency-matched dry.
- `FREEZE`: sample-and-hold of the whole coefficient vector, applied at block
  boundaries. Sliders stay live; COEFF IN still read every block.
- `OUTPUT LEVEL`, 0–2, default 1. No mandatory limiter or soft clip.
- Sequency ordering on the panel, natural order internally, separated by a
  permutation table.
- `1/N` port scaling, non-normalized forward transform, `1/N` on the inverse.
- Bypass connects AUDIO IN to AUDIO OUT with no block latency.
- JSON persistence: FREEZE, Overlay/Replace, SIZE. Frozen vector not saved.
- The full test list from spec section 17, plus round-trip at several sample
  rates and a check that COMPONENTS sums to wet.

## Sonic reality check, for the docs

Worth stating plainly in `doc/quadrare.md` rather than discovering by
surprise: modifying coefficients changes gain at the block rate, which
generates sidebands at multiples of the block rate that fold back. That
aliasing is the sound, not a defect. At N=16 the block rate is 3 kHz and the
module is a grit box; at N=256 it is 187 Hz and it behaves much more like a
spectral filter. FREEZE at N=16 is a 3 kHz whistle; at N=256 it is a 187 Hz
drone with timbre.

The band split is linear, so slider 0 covers everything below 1.5 kHz. On
bass-heavy material most of the action is on the first two or three sliders,
and the upper columns work on air and noise. Say so in the manual rather than
letting people discover it.

## Open items

- ~~Novelty check against the current VCV Library.~~ Done 2026-07-23: no
  Walsh–Hadamard transform module in the library.
- QUANT absolute vs relative grid (see above).
- `panel_audit.py` needs `VCVSlider` geometry.
- Latin glossary row for `readme.md`.
- Whether `COMPONENTS OUT` at 16 channels is worth its jack, given that the
  per-band COEFF OUT jacks already expose the transform domain. It answers a
  different question (what does this band sound like, vs what is its
  coefficient), so keeping it for now.
