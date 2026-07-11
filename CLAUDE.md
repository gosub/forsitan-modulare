# forsitan modulare — project notes for Claude

A collection of independent VCV Rack modules. Latin names, OCR-A typography,
yellow-on-dark panels.

## Versioning

- **Minor version** (`2.x.0`): bump when adding a new module
- **Patch version** (`2.x.y`): bump for fixes and enhancements to existing modules
- Keep `plugin.json` `"version"` in sync with the git tag, and update
  `CHANGELOG.md`
- The `changelogUrl` / `manualUrl` in `plugin.json` reference tagged or branch
  paths on GitHub — update them when cutting a release

## Documentation structure

- `readme.md` carries the plugin overview: the module table with one-line
  descriptions, the tools table, the "about the names" Latin glossary, and the
  license — no full module documentation.
- Each module's full documentation lives in `doc/<slug>.md` (lowercase, e.g.
  `doc/alea.md`, `doc/mmcccxcix.md`), linked from the readme table.
- Each module entry in `plugin.json` has a `manualUrl` pointing to
  `https://github.com/gosub/forsitan-modulare/blob/master-v2/doc/<slug>.md`.
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
  centered horizontally near the top. Generated panels (MMCCCXCIX onward)
  use a 3.8mm cap height, auto-shrunk on long names to clear the screw
  zones (`regen_svg` handles this).
- **forsitan logo** bottom corner: a rounded-rect "domino" with a divider line
  and four dots, stroked/filled in the accent yellow `#ffd500`. On 2HP panels
  it is rotated 90°.
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
  scando); stereo pairs get one LED per badge. An LED for a non-jack
  control sits at that control's top-right corner.
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
- `tools/patches/` — `gen_patches.py`, generates `patches/*.vcv`. A `.vcv` is a
  zstd-compressed tar of `./patch.json` + an empty `./modules/`. Currently
  builds `patches/limen.vcv` (one limen module, `serverEnabled` on) so
  `./Rack patches/limen.vcv` launches straight into a controllable state.
  Regenerate after edits: `python3 tools/patches/gen_patches.py` (needs `tar` + `zstd`).
