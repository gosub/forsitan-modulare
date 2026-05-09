# panel-editor

A browser-based drag-and-drop panel layout editor for forsitan VCV Rack modules.

Reads the `@layout` block from a module's `.cpp` file, lets you reposition every element visually, and writes the updated coordinates back to the `.cpp` and regenerates the panel SVG — all in one Save click.

## Requirements

- Python 3 (stdlib only for the server)
- [fonttools](https://github.com/fonttools/fonttools) for SVG regeneration (OCR-A glyph paths)
- OCR-A font file at `~/dl/audio/ocr-a/OCR-A Regular/OCR-A Regular.otf` or `/tmp/ocr-a/OCR-A Regular/OCR-A Regular.otf`

### fonttools setup

```bash
python -m venv /tmp/fonttools-venv
/tmp/fonttools-venv/bin/pip install fonttools
```

The editor auto-detects the venv at `/tmp/fonttools-venv` or `~/dl/audio/fonttools-venv`; no activation needed.

## Usage

```bash
python tools/panel-editor/panel-editor.py src/MMCCCXCIX.cpp
# Open http://localhost:7890/ in a browser
```

Drag elements to position, then click **Save to .cpp + .svg**. The server rewrites the `@layout` block in the `.cpp` and regenerates the SVG using OCR-A glyph paths.

To regenerate the SVG without starting the server (e.g. after manually editing the `.cpp`):

```bash
/tmp/fonttools-venv/bin/python - <<'EOF'
import sys, os
src = open('tools/panel-editor/panel-editor.py').read().replace("if __name__ == '__main__':", "if False:")
ns = {}; exec(compile(src, 'panel-editor.py', 'exec'), ns)
data = ns['parse_cpp'](os.path.abspath('src/MMCCCXCIX.cpp'))
ns['regen_svg'](data, os.path.abspath('res/MMCCCXCIX.svg'))
EOF
```

## @layout syntax

The editor reads and writes a machine-editable comment block inside the widget constructor:

```cpp
// @layout:begin MODULE_NAME PANEL_W_MM PANEL_H_MM
// @elem ID CppType radius kind "label" label_dy [x y]
// ...

        addParam(...);  // generated C++ — do not edit
        // @layout:end
```

### Element kinds

| kind | C++ widget | position source | notes |
|------|-----------|----------------|-------|
| `param` | `addParam(createParamCentered<…>)` | Vec in C++ line | center |
| `input` | `addInput(createInputCentered<…>)` | Vec in C++ line | center |
| `output` | `addOutput(createOutputCentered<…>)` | Vec in C++ line | center |
| `light` | `addChild(createLightCentered<…>)` | Vec in C++ line | center |
| `screw` | `addChild(createWidget<ScrewSilver>)` | Vec in C++ line + 2.54mm offset | top-left → visual center |
| `label` | SVG only | trailing `x y` in `@elem` line | baseline center |
| `logo` | SVG only | trailing `x y` in `@elem` line | visual center |
| `box` | SVG only | trailing `x y` in `@elem` line | visual center of 14×14mm rect |

SVG-only elements (`label`, `logo`, `box`) produce no C++ output; their position is stored as trailing coordinates directly on the `@elem` comment line.

### Adding a new element

Add a `// @elem` line manually inside the `@layout` block, then run the editor or regen script. The C++ addParam/addInput/addOutput line will be generated on the next save.

## Keyboard shortcuts

| key | action |
|-----|--------|
| click | select element |
| shift+click | add/remove from selection |
| drag | move selected elements |
| arrow keys | nudge ±0.1mm |
| shift+arrow | nudge ±1.0mm |
| ctrl+A | select all |
| Escape | deselect all |

## Panel SVG

The SVG is generated entirely from the `@layout` data — never edit `res/*.svg` by hand. The background, title, labels, logo, and boxes are all rendered as OCR-A glyph paths by `regen_svg()`.

Panel dimensions and module name come from the `@layout:begin` header line.
