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

32HP (162.56mm), 16 vertical columns, one per exposed coefficient. Each
column is:

```
VCVLightSlider<GreenRedLight>   bipolar gain, with the coefficient lit in the handle
COEFF OUT jack                  mono, that coefficient
COEFF IN jack                   mono, that coefficient
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
AUDIO IN | SIZE | KEEP | QUANT | LEVEL | DRY/WET | FREEZE | COMPONENTS | RESIDUAL | AUDIO OUT
```

The **above** switch (CKSS) sits on its own at the left, under the COEFF IN
row, since it governs everything the sliders do not reach.

`panel_audit.py` gained `RECT_OVERRIDE` for `VCVSlider` (6.72 × 25.92mm) and
`CKSS` (4 × 10mm), and badges gained `box=WxH` so the output row can sit on
one full-width field instead of sixteen 14mm badges.

## The slider window

The sixteen sliders own the **lowest sixteen Walsh coefficients** outright,
one each, in sequency order. There is no band split: slider *k* is
coefficient *k*.

This replaced an earlier design in which the sliders were sixteen bands
covering the whole spectrum. That design was unplayable, and unfixably so.
Coefficient *k* sits at `k·fs/2n`, so sixteen equal bands always divide
0–24 kHz into 1500 Hz slices *whatever the size* — band 0 permanently owned
everything below 1500 Hz, which is where most music lives. Larger sizes only
subdivided within each band and could not help. Unequal (quadratic) bands
would have fixed the distribution but forced each jack to a different poly
width, which is invisible from the panel.

Owning the low sixteen coefficients directly fixes both at once:

- **Playable.** At n=256 the sliders are 93.8 Hz apart across 0–1500 Hz,
  about sixteen times the low-end resolution of the band design.
- **Every jack is mono**, at every size. The poly-width problem does not get
  traded against, it stops existing.

The cost is that the sliders no longer reach above the window. Everything
above moves together, through the **above** switch.

## SIZE, as a zoom

Because only the lowest sixteen coefficients are ever exposed, size no longer
changes the jacks at all. It chooses how far into the low end the sliders
zoom: coefficient spacing is `fs/2n`, so the window spans `8·fs/n`.

| N | per slider @48k | window | latency (2N) |
|---|---|---|---|
| 16 | 1500 Hz | 0–24 kHz | 0.67 ms |
| 32 | 750 Hz | 0–12 kHz | 1.3 ms |
| 64 | 375 Hz | 0–6 kHz | 2.7 ms |
| 128 | 187.5 Hz | 0–3 kHz | 5.3 ms |
| **256** (default) | **93.8 Hz** | **0–1.5 kHz** | **10.7 ms** |
| 512 | 46.9 Hz | 0–750 Hz | 21.3 ms |

n=16 is the degenerate case where the window is the whole spectrum and there
is nothing above it, so the **above** switch is inert and the spec's
transparency / mute / invert cases hold exactly. Every step down zooms
further into the bass and doubles the latency.

Size changes flush the pipeline, so expect a click and up to one block of
silence.

Measured selectivity at n=256, muting one slider against a sine at that
slider's frequency: the diagonal drops 1.8–3.3 dB while neighbours drop
0–1.5 dB. Soft, because a sine spreads across many coefficients of a
square-wave basis, but each slider does own its own region.

## The above switch

A two-position panel switch over every coefficient outside the window:

- **Pass** (default): they go through untouched, so the module shapes the low
  window and leaves the rest of the signal alone.
- **Mute**: they are zeroed, making the module a lowpass at the window edge
  with sixteen Walsh sliders inside it. Rejection is about 22 dB rather than
  clean, since Walsh functions are not sinusoids.

KEEP and QUANT act on **all** coefficients, not just the window, so the codec
stages still degrade the full-range signal and the pass-through region has
something to do.

## COEFF OUT / COEFF IN

**Mono** jacks, one per slider, carrying that slider's signed coefficient
scaled `1/N` for volts as the spec's section 10 specifies. Identical at every
size, so there is no poly width to reason about at all.

A plain OUT→IN cable is identity. Feeding COEFF IN with no audio present
drives the IFWHT as a standalone Walsh synthesizer.

`Overlay` (default) and `Replace` from spec section 7 are kept, as a context
menu item. With mono jacks they distinguish what happens to the *undriven*
exposed coefficients once anything is patched:

- Overlay: a driven jack replaces its coefficient, undriven ones stay internal.
- Replace: as soon as any jack is driven, undriven coefficients go to zero.
  This is what makes the inverse transform usable as a standalone decoder
  (spec 7.2).

Coefficients above the window follow the **above** switch in both modes.

Internal slider gains apply before the external substitution, so a replaced
channel is not multiplied by its slider (spec 7.3 unchanged).

### Two-block pipeline

Found during implementation, and it overrides the spec's section 9. Rack steps
modules in arbitrary order and copies cable voltages once per frame, so a
COEFF OUT → COEFF IN patch is a feedback cable and cannot deliver a block's
coefficients back within that same block. Reading COEFF IN at the boundary
where COEFF OUT was written would pick up a vector one block stale, and the
"plain cable is identity" property would quietly fail: the wet path would run
a block behind the dry, so RESIDUAL would be non-null and DRY/WET would mix
misaligned copies.

So the module analyzes at one boundary and reconstructs at the next:

```
boundary j    analyze block j -> C_j, publish C_j on COEFF OUT
              read COEFF IN (holds C_{j-1}), substitute into C_{j-1},
              inverse transform -> plays during block j+1
```

By the time COEFF IN is read, a full block has passed since the matching
COEFF OUT was written, so any cable delay is long gone and the returned values
always pair with the vector they came from. Latency is `2N` samples, constant
whether or not anything is patched, and the dry path is delayed to match.
`test/smoke_quadrare.cpp` verifies this with a deliberately one-frame-delayed
loopback in both Overlay and Replace.

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
module is a grit box; at N=512 it is 94 Hz and it behaves much more like a
spectral filter. FREEZE at N=16 is a 3 kHz whistle; at N=512 it is a 94 Hz
drone with timbre.

The sliders are bipolar, so the bottom of the travel is −1 (full inversion),
not 0. Mute is the *center*. Sliding everything down leaves the signal
audibly unchanged, which surprised the author during play-testing. The V-shape
was measured and confirmed intentional; the decision was to document it rather
than add a detent. Say so prominently in the manual.

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
