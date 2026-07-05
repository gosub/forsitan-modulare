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

## limen module

`src/limen.cpp` is a control interface, not a sound module:

- TCP server on `localhost:7000` (default), newline-delimited JSON protocol.
- Started in `onAdd()`, stopped in `onRemove()` via `std::thread`.
- Port is configurable from the right-click context menu (7000/7001/7002/7777/8000).
- Green LED at y=64mm indicates listening state.
- Commands: `list_modules`, `get_module`, `list_params`, `set_param`,
  `list_cables`.
- Client tools live in `tools/cli/` (C `limen.c` + Python `limen.py`).

## Panel structure

Panels are authored in Inkscape but must stay **NanoSVG-compatible** (Rack
renders with NanoSVG, which cannot draw `<text>` — titles and labels are
pre-baked `<path>` elements). Shared visual grammar:

- **Background** rect filling the panel (`#1a1a1a`).
- **Title**: module name in **OCR-A**, baked to SVG `<path>` (fill `#f9f9f9`),
  centered horizontally near the top.
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

Two helpers live in `tools/` (they need `fonttools` + `booleanOperations`;
use the venv at `~/dl/audio/fonttools-venv`, never a global pip install):

- **`tools/gen_title_paths.py`** — bakes text into NanoSVG-safe `<path>`
  elements. It runs a union boolean op per glyph so inner contours (holes)
  render correctly under NanoSVG's even-odd fill. Pass the OCR-A `.otf` as
  `--bold` and use `:bold` segments (forsitan is single-weight):

  ```
  ~/dl/audio/fonttools-venv/bin/python tools/gen_title_paths.py \
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

- **`tools/measure_text.py`** — reports advance width (mm) of strings at a given
  cap height, for laying out labels and sizing badges:

  ```
  ~/dl/audio/fonttools-venv/bin/python tools/measure_text.py \
      --font "$HOME/dl/audio/ocr-a/OCR-A Regular/OCR-A Regular.otf" \
      --cap-height 2.5 alea limen
  ```

## Tools

- `tools/cli/` — limen client: C (`limen.c`, builds `limen`) and Python
  (`limen.py`).
- `tools/panel-editor/` — browser-based drag-and-drop panel layout editor
  (`panel-editor.py`). On save it regenerates the panel SVG, auto-finding the
  OCR-A font and a fonttools venv from the same candidate paths above.
- `tools/gen_title_paths.py`, `tools/measure_text.py` — see Typography above.
- `tools/gen_patches.py` — generates `patches/*.vcv`. A `.vcv` is a
  zstd-compressed tar of `./patch.json` + an empty `./modules/`. Currently
  builds `patches/limen.vcv` (one limen module, `serverEnabled` on) so
  `./Rack patches/limen.vcv` launches straight into a controllable state.
  Regenerate after edits: `python3 tools/gen_patches.py` (needs `tar` + `zstd`).
