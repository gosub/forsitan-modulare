#!/usr/bin/env python3
"""
Generate res/pellicula.svg — the static panel art for the pellicula module.

pellicula's panel is a regular 9-column (poly + 8 voices) x 11-row matrix, so
its widgets are placed by loops in src/pellicula.cpp rather than the browser
panel-editor. This script emits only the background art (title, column headers,
row labels, logo) and reuses the panel-editor's OCR-A path baking so the text is
NanoSVG-safe and matches the house typography.

The geometry constants below MUST stay in sync with src/pellicula.cpp.

Usage:  python3 tools/panels/gen_pellicula_panel.py
"""
import os, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# import the panel-editor module (hyphenated filename → load by path)
spec = importlib.util.spec_from_file_location(
    "panel_editor", os.path.join(REPO, "tools", "panel-editor", "panel-editor.py"))
pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe)

PANEL_W, PANEL_H = 111.76, 128.5   # 22 HP

# column x centres: [0] = poly, [1..8] = voices  (matches COLX in pellicula.cpp)
COLX = [18.0, 28.5, 39.0, 49.5, 60.0, 70.5, 81.0, 91.5, 102.0]
MIX_X = 7.0

HEADER_Y = 11.0
TRIG_JACK_Y = 26.0
SAMP_JACK_Y = 45.0
PIT_JACK_Y  = 64.0
DEC_JACK_Y  = 83.0
LVL_JACK_Y  = 102.0
OUT_JACK_Y  = 112.0

# divider between the poly column and the voice columns
DIVIDER_X = (COLX[0] + COLX[1]) / 2.0


def label(text, x, y):
    return {"id": text, "cpp_type": "label", "radius": 1.0, "kind": "label",
            "label": text, "label_dy": 0.0, "x": x, "y": y}


def logo(x, y):
    return {"id": "LOGO", "cpp_type": "forsitan_logo", "radius": 2.75,
            "kind": "logo", "label": "", "label_dy": 0.0, "x": x, "y": y}


elements = []

# column headers
for name, x in zip(["poly", "1", "2", "3", "4", "5", "6", "7", "8"], COLX):
    elements.append(label(name, x, HEADER_Y))

# row labels (left gutter, aligned with each input row)
elements.append(label("trig",   MIX_X, TRIG_JACK_Y))
elements.append(label("sample", MIX_X, SAMP_JACK_Y))
elements.append(label("pitch",  MIX_X, PIT_JACK_Y))
elements.append(label("decay",  MIX_X, DEC_JACK_Y))
elements.append(label("level",  MIX_X, LVL_JACK_Y))
elements.append(label("mix",    MIX_X, OUT_JACK_Y - 6.0))   # above the mix jack

# logo, bottom centre
elements.append(logo(PANEL_W / 2, 121.0))

layout = {
    "module": "pellicula",
    "panel_w": PANEL_W,
    "panel_h": PANEL_H,
    "elements": elements,
}

svg_path = os.path.join(REPO, "res", "pellicula.svg")
ok = pe.regen_svg(layout, svg_path)

# Post-process: the output jacks (poly + 8 voices + mix) all render as plain grey
# Rack ports, so add a subtle accent-yellow bar behind the output row to mark it
# as outputs — the house grammar uses yellow for output affordances.
if ok:
    bar = (f'  <rect x="2.4" y="{OUT_JACK_Y - 5.0:.2f}" width="{PANEL_W - 5.6:.2f}" '
           f'height="10.0" rx="2" fill="#ffd500" opacity="0.22"/>')
    # thin divider separating the poly column from the eight voice columns
    divider = (f'  <line x1="{DIVIDER_X:.2f}" y1="{HEADER_Y + 3.0:.2f}" '
               f'x2="{DIVIDER_X:.2f}" y2="{OUT_JACK_Y + 6.0:.2f}" '
               f'stroke="#4d4d4d" stroke-width="0.3"/>')
    with open(svg_path) as f:
        svg = f.read()
    marker = '<rect width="111.76" height="128.5" fill="#1a1a1a"/>'
    svg = svg.replace(marker, marker + "\n" + bar + "\n" + divider, 1)
    with open(svg_path, "w") as f:
        f.write(svg)

print(("wrote " if ok else "FAILED to write ") + svg_path)
