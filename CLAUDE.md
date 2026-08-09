# forsitan modulare — project notes for Claude

A collection of independent VCV Rack modules. Latin names, OCR-A typography,
yellow-on-dark panels.

## Versioning

**`RELEASING.md` is the authoritative release procedure** — follow it when
cutting a release. The essentials:

- **Minor version** (`2.x.0`): bump when adding a new module
- **Patch version** (`2.x.y`): bump for fixes and enhancements to existing modules
- Keep `plugin.json` `"version"` in sync with the git tag, and update
  `CHANGELOG.md`
- Every documentation URL in `plugin.json` (`manualUrl` at plugin level, one
  per module, and `changelogUrl`, 28 in all) points at the **version's tag**,
  never a branch: the library serves whatever the installed build declares, so
  a branch URL shows a 2.9.0 user the manual for today's master. Never
  hand-edit them, run:

  ```
  python3 tools/release/sync_version.py         # rewrite URLs to v<version>
  python3 tools/release/sync_version.py --check # verify, nonzero if stale
  ```

- **Release order**: bump `"version"`, update `CHANGELOG.md`, run
  `sync_version.py`, commit, then tag *that* commit and **push the tag**.
  Until the tag is pushed all 26 links 404. The tag only matters once a
  version is published; local development needs nothing.
- After publishing, post the update comment on
  [VCVRack/library#681](https://github.com/VCVRack/library/issues/681) — the
  library builds from source, so a tag alone does not ship anything.

## Documentation structure

- `readme.md` carries the plugin overview: the module table with one-line
  descriptions, the tools table, the "about the names" Latin glossary, and the
  license — no full module documentation.
- Each module's full documentation lives in `doc/<slug>.md` (lowercase, e.g.
  `doc/alea.md`, `doc/mmcccxcix.md`), linked from the readme table.
- `doc/experiments.md` is the exception: modules that were built, auditioned
  and *not* kept. They live on `exp/<name>` tags rather than branches, and
  that file records what they were and why they were dropped. Add an entry
  there (and mark the `ideas.md` entry in place) whenever one is archived.
- Each module entry in `plugin.json` has a `manualUrl` pointing to
  `https://github.com/gosub/forsitan-modulare/blob/v<version>/doc/<slug>.md`
  (tagged, see Versioning; `tools/release/sync_version.py` maintains them).
- `plugin.json` module `description`s are **one-line summaries**, as the SDK
  asks (`Model.hpp`: "A one-line summary of the module's purpose"). Rack shows
  the field as the module-browser hover tooltip and does not wrap it, so a
  long one stretches off screen. Keep under ~100 characters; the readme table
  may run a little longer, and behaviour details belong in `doc/<slug>.md`.
- When adding a module, do all of: create `src/<name>.cpp`, declare the model
  in `src/forsitan.hpp`, register it in `src/forsitan.cpp`, add the panel
  `res/<name>.svg`, add the `plugin.json` entry (with `manualUrl`), create
  `doc/<slug>.md`, and add the readme table row.

## Build

```
make                                      # build (RACK_DIR is set in .bashrc)
HOME=/home/gg/dl/temp/rackhome/ make install
cd ~/dl/audio/rack && HOME=/home/gg/dl/temp/rackhome/ ./Rack   # run Rack
```

- `make install` needs the fake `HOME` so Rack installs into
  `~/dl/temp/rackhome/.local/share/Rack2/plugins-lin-x64/` and the real home
  stays clean. Run Rack itself with the same fake `HOME` so it loads that plugin.
- Rack SDK is symlinked at `~/dl/audio/rack-sdk` → current SDK version.
- `make dist` packages `res/` and `LICENSE*` plus the built plugin.

## Module architecture

These are **independent modules** — there is no shared base class or expander
chain. Each module is a self-contained `src/<name>.cpp`:

- `src/forsitan.hpp` declares `extern Model*` for each module.
- `src/forsitan.cpp` calls `p->addModel(...)` for each one in `init()`.
- `src/callback_button.hpp` is a small shared widget helper.

| slug | description | width |
|------|-------------|-------|
| alea | random module adder | 15.24mm (3HP) |
| interea | chord generator (quality, voicing, inversion, harmonize) | 50.8mm (10HP) |
| cumuli | accumulator with up/down gates and rates | 25.4mm (5HP) |
| deinde | quad cascading addressable attack-hold envelope | 40.64mm (8HP) |
| pavo | polyphonic stereo spreader (Splay Ugen) | 25.4mm (5HP) |
| limen | TCP+JSON Rack control interface | 15.24mm (3HP) |
| MMCCCXCIX | PT2399 delay chip emulation with feedback send/return | 50.8mm (10HP) |
| scando | scanned-synthesis oscillator (mass-spring string scanned into a wavetable) | 81.28mm (16HP) |
| pellicula | exploded 8-voice drum sampler (Pico DRUM engine; poly-normalled sample/pitch/decay/level matrix) | 111.76mm (22HP) |
| draen | drone synthesizer (two 37-engine banks: dronecaster ports + hyf originals; played from hz/amp, fading engine select, bank via context menu) | 40.64mm (8HP) |
| rete | feedback integrator network (8 leaky integrators, random 8×8 matrix, self-oscillating chaos) | 50.8mm (10HP) |
| ululo | feedback guitar (six comb strings + saturating amp in a howling loop, poly V/oct retuning) | 40.64mm (8HP) |
| tabes | disintegration looper (per-pass tape aging, overdub, splice restore, loop overlap, FX send/return) | 60.96mm (12HP) |
| lustro | scanned filter (scando string drives the band gains of a 16-band resonant filterbank) | 50.8mm (10HP) |
| bulla | inspired by Hordijk's Blippoo Box (two cross-modulating oscillators, two runglers, twin-peak filter) | 50.8mm (10HP) |
| perge | stereo dynamic sampler and multi-effect (dynamics-gated repeats, freeze, glitch/dimension, lofi/crush, reverb/smear, tilt filter; AC noises CONTINUA homage) | 101.6mm (20HP) |
| vorax | feedback drone synthesizer (Audrey II port: self-exciting Karplus-Strong loop, reverb, degrading tape echo) | 50.8mm (10HP) |
| textor | one-knob loop weaver (Fieldtone Weaver clone: 2s capture rewoven per knob move, 3 elements with gates) | 50.8mm (10HP) |
| imber | generative rain (Haiku-inspired: 8 players on a 2D field, drunk clocks, morphing clock/FX constellations, procedural sample bank with selectable root/scale) | 182.88mm (36HP) |
| sylla | random sample generator/player (imber's generator library as a standalone voice; 27 engines one per knob position, v1 legacy families in the menu; selectable root/scale) | 40.64mm (8HP) |
| guttur | chaotic resonator drone (Gutter Synthesis port: Duffing oscillator coupled through 2x24 resonant bandpass filters, 20 morphing factory banks) | 121.92mm (24HP) |
| vespae | Wasp filter (EDP Wasp / Doepfer A-124 emulation: CMOS-inverter SVF on a unipolar supply, simultaneous LP/BP/HP/notch + the A-124 LP-to-HP mix pot with CV, drive + supply-headroom grit, bias/hiss mod trimpots) | 60.96mm (12HP) |
| quadrare | patchable Walsh-Hadamard codec (16 sliders + 32 jacks onto the transform domain, size-zoomable window, keep/quant lossy stages, residual + per-coefficient component outs) | 162.56mm (32HP) |
| vestigia | stereo memory effect (endless tape loop, oblivion/remanence/sediment memory modes, block activity map + descriptor pool with integrity/wear, listen/breathe/dream recollection modes, similarity weighting, age degradation, modulated diffusion + crossfeed, protected feedback; all macro CV, MEMORY OUT, full context menu, 7 factory presets) | 132.08mm (26HP) |
| antrum | Erbe-Verb clone (4-line FDN, size 1-500ms, cyclic/ergodic/shimmer depth, absorb = diffusion+damping, reverse pre-delay, clock sync, energy CV out; 8 factory presets from the hardware manual) | 101.6mm (20HP) |
| caligo | Greyhole port (long modulated echo inside 3x4-deep nested allpass diffusers, 24 delay lines on prime lengths; spin = the rotator angle, drift, scatter, freeze, dissolve/tape, patchable feedback loop; 7 factory presets) | 121.92mm (24HP) |
| raucus | Big Muff Pi USA V3 (four transistor stages, feedback diode clipping solved into a table, tone stack as one biquad from a nodal analysis; added gain/bias trims, mids, diode menu, oversampling) | 50.8mm (10HP) |
| tundo | Basimilus Iteritas Alter clone (six tonal oscillators + noise, spread from harmonic to prime ratios, harm decay/amplitude staging, morph, infinite folder with pulse train; skin/liquid/metal, bass/alto/treble, per-knob attenuverter + CV, env out; engine on its own power-of-two clock through a ZOH) | 71.12mm (14HP) |
| cartilago | Gristleizer (one band-limited LFO of four shapes into a shunt-FET attenuator that never closes and ticks, or into a ZDF state-variable filter; depth past 100%, v/oct into the audio band for ring modulation, oversampled) | 60.96mm (12HP) |
| turba | chaotic bank inspired by Reaktor's Skrewell (8 oscillator + feedback-delay channels cross-coupled in a ring, 3 topologies, 64 bars in an edit area, 4 macros that map rather than offset) | 121.92mm (24HP) |

## limen module

`src/limen.cpp` is a control interface, not a sound module:

- TCP server on `localhost:7000` (default), newline-delimited JSON protocol.
- Started in `onAdd()`, stopped in `onRemove()` via `std::thread`.
- Port is configurable from the right-click context menu (7000/7001/7002/7777/8000).
- Green LED at y=64mm indicates listening state.
- Commands: `list_modules`, `get_module`, `get_module_info`, `list_params`,
  `set_param`, `list_cables`, and more — `hello` returns the full list.
- Client tools live in the separate repo
  [gosub/limen-tools](https://github.com/gosub/limen-tools), checked out at
  `~/box/prj/2026/limen-tools` (C `limen-cli.c` + Python `limen-cli.py`,
  binaries built by that repo's GitHub Actions).

## Panel structure

Panels are authored in Inkscape but must stay **NanoSVG-compatible** (Rack
renders with NanoSVG, which cannot draw `<text>` — titles and labels are
pre-baked `<path>` elements). Shared visual grammar:

- **Background** rect filling the panel (`#1a1a1a`).
- **Title**: module name in **OCR-A**, baked to SVG `<path>` (fill `#f9f9f9`),
  centered horizontally near the top, baseline at `3.5 + cap height`.
  **The title cap height is 3.2mm on every panel, whatever its width**
  (`TITLE_CAP_MM` in `tools/panel-editor/panel-editor.py`). Panel width is
  not a reason to shrink it; **lack of space is the only reason**, i.e. a
  panel whose topmost element or code-drawn display sits so high that a
  3.2mm title would crowd it. Override it per panel on the `@layout:begin`
  line:

  ```
  // @layout:begin imber 182.88 128.5 title=2.8
  ```

  Current overrides, both because something sits right under the title:
  `imber` (VON buttons at y=11, 1.25mm clear at 3.2mm) and `vestigia`
  (display band starting at y=9, 2.3mm clear). `pellicula` is generated by
  its own script (`tools/panels/gen_pellicula_panel.py`) and keeps its
  smaller title: its column headers start at y≈8.8. Long names still shrink
  automatically to clear the screw zones (only MMCCCXCIX does, `regen_svg`
  handles it).
- **forsitan logo** at the bottom, centered horizontally (`width/2`,
  y=122.5mm — see bulla/vorax/textor): a rounded-rect "domino" with a
  divider line and four dots, stroked/filled in the accent yellow
  `#ffd500`. Never tuck it in a corner. On 2HP panels it is rotated 90°.
- **Input labels**: plain OCR-A text, light grey (`#e5e5e5`).
- **Output labels**: OCR-A text in dark (`#1a1a1a`) sitting on a yellow
  (`#ffd500`) badge.
- **Indicator LEDs** drawn as dark circles (`fill #333333`, `stroke #555555`).
- **Screws** at the four standard VCV corners.
- Canonical sizes: panel height 128.5mm; widths are multiples of 1HP = 5.08mm
  (narrowest module is 15.24mm / 3HP).
- `res/palette.svg` holds the reference swatches.

## Panel design principles

When laying out a panel (by hand or generated), follow these rules:

- **Symmetry**: center single elements on the panel's vertical midline
  (`width/2`); place paired elements (L/R outputs, dual knobs) at equal
  offsets from it. Prefer mirrored left/right columns over ragged placement.
- **Balance**: distribute visual weight evenly left/right and top/bottom.
  Big knobs are heavy, jacks and labels light — offset a large control on one
  side with a group of smaller ones on the other. Don't crowd everything into
  the top half; keep roughly even vertical density, and don't leave dead
  bands taller than ~15mm unless intentional breathing room around the title
  or logo.
- **No overlap**: no two element bounding boxes may intersect — knobs, jacks,
  buttons, LEDs, labels, badges, title, logo, and the 4 screw zones
  (~8×8mm at each corner). Use real widget sizes, measured from the
  ComponentLibrary SVG viewBoxes (Rogan2P ≈ 12.7mm ⌀,
  RoundBlackKnob 9.6mm ⌀, RoundBigBlackKnob 15.24mm ⌀,
  RoundHugeBlackKnob 18.24mm ⌀, PJ301M jack 8.0mm ⌀, TL1105 5.2mm ⌀,
  SmallLight 2mm ⌀, CKSS switch ≈ 4×10mm) and keep ≥1.5mm clearance between
  edges, ≥1mm between a label and the element it names.
- **Label offsets**: place a label's baseline at a fixed offset below its
  control's center — jack +7.5mm, RoundBlackKnob +8.5mm,
  RoundBigBlackKnob +11.5mm, TL1105 button +7mm. A section label naming a
  group of controls sits *below* the group. OCR-A is wide: ~2.1mm advance
  per character at 2.2mm cap height.
- **LEDs**: never free-floating. An output-level LED sits 2mm inset from
  the top-right corner of its output badge (box center +5,−5 — see
  scando). A stereo output pair always gets one level LED per badge —
  both L and R, never just one of the two. An LED for a non-jack
  control sits at that control's top-right corner. Whatever it sits on,
  it must clear that element's edge by **≥0.5mm** (`panel_audit.py`
  enforces this; the canonical offsets are +5.00/−3.00 from a jack
  centre, which clears by 0.82, and the control's own corner otherwise).
- **Stereo output labels**: when the badge labels are just the channel
  letters, write them uppercase — `L` and `R`, not `l` / `r`.
- **Readability**: every jack and control gets a label; labels sit
  consistently (below jacks/knobs unless space forces otherwise), at ≥2.0mm
  cap height, never split across an element. Output badges must fully
  contain their text with ~1mm padding. Verify label widths with
  `tools/typography/measure_text.py` before placing.
- **Verification**: after any layout work run
  `~/dl/audio/fonttools-venv/bin/python tools/panel-editor/panel_audit.py src/<mod>.cpp`
  — it checks all of the above with true circle geometry and real OCR-A
  text widths, and exits nonzero on violations (no args = all @layout
  modules).
- **Alignment & grouping**: snap centers to a coarse grid (whole or half
  mm); align related elements on shared rows/columns. Group by function
  (control + its CV jack + label as one cluster) and separate groups with
  more space than within them. Signal flow reads top→bottom: title, then
  controls/inputs, outputs near the bottom, logo last.
- **Consistency**: repeat spacing rhythms (equal row pitch for jack rows),
  reuse the shared grammar above, and match the look of existing panels in
  `res/` before inventing a new arrangement.

## Color palette

| Role               | Value     |
|--------------------|-----------|
| Background         | `#1a1a1a` |
| Title text         | `#f9f9f9` |
| Input labels       | `#e5e5e5` |
| Accent (logo, badges) | `#ffd500` |
| Output label text (on badge) | `#1a1a1a` |

## Typography & SVG title generation

The panel font is **OCR-A** (`~/dl/audio/ocr-a/OCR-A Regular/OCR-A Regular.otf`;
source archive `~/dl/ocr-a-regular.zip`). It is not installed system-wide.

Two helpers live in `tools/typography/` (they need `fonttools` +
`booleanOperations`; use the venv at `~/dl/audio/fonttools-venv`, never a
global pip install):

- **`tools/typography/gen_title_paths.py`** — bakes text into NanoSVG-safe `<path>`
  elements. It runs a union boolean op per glyph so inner contours (holes)
  render correctly under NanoSVG's even-odd fill. Pass the OCR-A `.otf` as
  `--bold` and use `:bold` segments (forsitan is single-weight):

  ```
  ~/dl/audio/fonttools-venv/bin/python tools/typography/gen_title_paths.py \
      --bold "$HOME/dl/audio/ocr-a/OCR-A Regular/OCR-A Regular.otf" \
      --titles "alea:bold" \
      --panel-width 15.24 \
      --cap-height 2.5 \
      --baseline 11 \
      --color "#f9f9f9"
  ```

  `--panel-width` is used to center the output; for an output-label badge pass
  the badge width and wrap the result in a `<g transform="translate(badge_x,0)">`,
  with `--color "#1a1a1a"`.

- **`tools/typography/measure_text.py`** — reports advance width (mm) of strings at a given
  cap height, for laying out labels and sizing badges:

  ```
  ~/dl/audio/fonttools-venv/bin/python tools/typography/measure_text.py \
      --font "$HOME/dl/audio/ocr-a/OCR-A Regular/OCR-A Regular.otf" \
      --cap-height 2.5 alea limen
  ```

## Tools

- `tools/panel-editor/` — browser-based drag-and-drop panel layout editor
  (`panel-editor.py`). On save it regenerates the panel SVG, auto-finding the
  OCR-A font and a fonttools venv from the same candidate paths above.
- `tools/typography/` — `gen_title_paths.py`, `measure_text.py`; see
  Typography above.
- `tools/panels/` — `gen_pellicula_panel.py`, generates `res/pellicula.svg`
  (background art for the matrix panel; widgets are placed in code).
- `tools/release/` — `sync_version.py`, repoints every `manualUrl` /
  `changelogUrl` in `plugin.json` at the current `"version"` tag and checks it
  against the newest `CHANGELOG.md` heading. `--check` is the tag-day gate;
  see `RELEASING.md`. Plus two image generators, so nothing in `img/` is ever
  made by hand:

  - `gen_screenshots.py` — the per-module panel images:

    ```
    python3 tools/release/gen_screenshots.py            # every module
    python3 tools/release/gen_screenshots.py alea draen # only these
    ```

    It drives Rack's own `-t <zoom>` screenshot mode against a throwaway user
    dir holding this plugin alone (the built `plugin.so` straight out of the
    source tree), then scales each panel to 600px tall with ImageMagick. The
    crop is exact by construction. What it renders is the module-browser
    preview: knobs at their defaults and no engine behind the panel, so
    displays that draw live state come out empty.

  - `gen_collection.py` — `img/forsitan-modulare.png`, the whole lineup:

    ```
    python3 tools/release/gen_collection.py --dry-run   # print the layout only
    python3 tools/release/gen_collection.py             # lay out and capture
    ```

    It starts Rack on `patches/limen.vcv` against a throwaway user dir like
    the above (with the tips dialog and CPU meter turned off), adds every
    module through **limen**, measures each one's HP from the protocol,
    splits them into rows with a dynamic program that minimises the widest
    row, and moves them into place — the row count is whichever best matches
    the screen's aspect ratio (three rows of ~113 HP for the 338 HP of
    modules on a 1920x1200 screen). Then fullscreen, zoom to fit, and
    `grim`. **It needs limen protocol 2** (`move_module`), so the installed
    plugin must be built from a tree that has it.
- `tools/patches/` — `gen_patches.py`, generates `patches/*.vcv`. A `.vcv` is a
  zstd-compressed tar of `./patch.json` + an empty `./modules/`. Currently
  builds `patches/limen.vcv` (one limen module, `serverEnabled` on) so
  `./Rack patches/limen.vcv` launches straight into a controllable state.
  Regenerate after edits: `python3 tools/patches/gen_patches.py` (needs `tar` + `zstd`).
