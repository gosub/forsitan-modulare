#!/usr/bin/env python3
"""Audit forsitan panel @layout blocks for overlaps and label clearances.

Rules (from CLAUDE.md):
  - >= 1.5 mm between any two element bounding boxes
  - >= 1 mm between a label and the element it names
Intentional containments are whitelisted: jack/label inside its panel box,
screws vs. panel edge, label sitting right under/over its own control
(still needs >= 1 mm).

Label baseline conventions (established v2.7.0): jack center +7.5 mm,
small knob (r4.5) +8.5 mm, big knob (r6) +10 mm, TL1105 button +7 mm.

Usage (from the repo root; needs the fonttools venv on hand):
    ~/dl/audio/fonttools-venv/bin/python tools/panel-editor/panel_audit.py [src/foo.cpp ...]
With no arguments it audits every src/*.cpp that has an @layout block.
Exits nonzero if any issue is found.
"""
import os, sys, re

sys.path.insert(0, 'tools/panel-editor')
src = open('tools/panel-editor/panel-editor.py').read().replace(
    "if __name__ == '__main__':", "if False:")
ns = {}
exec(compile(src, 'panel-editor.py', 'exec'), ns)

# font metrics via fonttools (venv already importable through regen helper)
ns['_ensure_fonttools']()
from fontTools.ttLib import TTFont
font = TTFont(ns['_find_font']())
glyphs = font.getGlyphSet()
cap_h = font['OS/2'].sCapHeight
cmap = font.getBestCmap()

def text_w(txt, sz):
    total = 0
    for ch in txt:
        g = glyphs.get(cmap.get(ord(ch), ch), glyphs.get('.notdef'))
        total += g.width * (sz / cap_h)
    return total

DESCENDERS = set('gjpqy')

# real widget sizes measured from the Rack ComponentLibrary SVG viewBoxes
RADIUS_OVERRIDE = {'RoundHugeBlackKnob': 9.12, 'RoundBigBlackKnob': 7.62,
                   'RoundBlackKnob': 4.8, 'PJ301MPort': 4.01,
                   'TL1105': 2.6, 'SmallLight': 1.0, 'MediumLight': 1.5}

def bbox(el):
    x, y = el['x'], el['y']
    k = el['kind']
    if k == 'label':
        w = text_w(el['label'], 2.2)
        desc = 0.7 if any(c in DESCENDERS for c in el['label']) else 0.0
        return (x - w/2, y - 2.2, x + w/2, y + desc)
    if k == 'box':
        return (x - 7, y - 7, x + 7, y + 7)
    if k == 'logo':
        return (x - 6.25, y - 2.75, x + 6.25, y + 2.75)
    r = RADIUS_OVERRIDE.get(el['cpp_type'], el['radius'])
    return (x - r, y - r, x + r, y + r)

CIRCLE_KINDS = ('param', 'input', 'output', 'light', 'screw')

def circle(el):
    if el.get('kind') not in CIRCLE_KINDS:
        return None
    r = RADIUS_OVERRIDE.get(el.get('cpp_type'), el.get('radius', 1.0))
    return (el['x'], el['y'], r)

def rect_gap(a, b):
    dx = max(a[0] - b[2], b[0] - a[2])
    dy = max(a[1] - b[3], b[1] - a[3])
    if dx < 0 and dy < 0:
        return -min(-dx, -dy)   # negative: overlap depth
    return max(dx, dy)

def circle_rect_gap(c, r):
    import math
    cx, cy, cr = c
    px = min(max(cx, r[0]), r[2])
    py = min(max(cy, r[1]), r[3])
    return math.hypot(cx - px, cy - py) - cr

def pair_gap(ea, ba, eb, bb):
    import math
    ca, cb = circle(ea), circle(eb)
    if ca and cb:
        return math.hypot(ca[0] - cb[0], ca[1] - cb[1]) - ca[2] - cb[2]
    if ca:
        return circle_rect_gap(ca, bb)
    if cb:
        return circle_rect_gap(cb, ba)
    return rect_gap(ba, bb)

def contains(outer, inner):
    return (outer[0] <= inner[0] and outer[1] <= inner[1]
            and outer[2] >= inner[2] and outer[3] >= inner[3])

def related(a, b):
    """label b names control a (same stem)?"""
    if b['kind'] != 'label':
        return False
    stem = b['id'].replace('LABEL_', '')
    return stem in a['id'] or a['id'].replace('_PARAM', '').replace(
        '_INPUT', '').replace('_OUTPUT', '').endswith(stem)

def audit(path):
    data = ns['parse_cpp'](os.path.abspath(path))
    els = data['elements']
    W, H = data['panel_w'], data['panel_h']
    issues = []
    # title bbox
    title_sz = 3.8 if W <= 60 else 2.8
    avail = W - 2 * 12.1
    if text_w(data['module'], title_sz) > avail:
        title_sz *= avail / text_w(data['module'], title_sz)
    title_w = text_w(data['module'], title_sz)
    title = {'id': 'TITLE', 'kind': 'label', 'label': data['module'],
             'x': W/2, 'y': 3.5 + title_sz}
    title_bb = (W/2 - title_w/2, 3.5, W/2 + title_w/2, 3.5 + title_sz)
    all_els = [(e, bbox(e)) for e in els]
    all_els.append((title, title_bb))
    for i in range(len(all_els)):
        for j in range(i+1, len(all_els)):
            (a, ba), (b, bb) = all_els[i], all_els[j]
            # whitelists
            if a['kind'] == 'box' or b['kind'] == 'box':
                box, obb = (a, bb) if a['kind'] == 'box' else (b, ba)
                other = b if a['kind'] == 'box' else a
                if other['kind'] in ('output', 'input', 'label', 'light'):
                    if contains(bbox(box), obb):
                        continue   # element fully inside its badge box: fine
            if a['kind'] == 'light' or b['kind'] == 'light':
                need = 0.2   # LEDs sit at the corner of their control
            elif a['kind'] == 'screw' or b['kind'] == 'screw':
                need = 1.0
            elif a['kind'] == 'label' and b['kind'] == 'label':
                need = 1.0
            elif a['kind'] == 'label' or b['kind'] == 'label':
                need = 1.0
            else:
                need = 1.5
            g = pair_gap(a, ba, b, bb)
            if g < need - 1e-6:
                issues.append((a['id'], b['id'], round(g, 2), need))
    # out-of-panel check
    for e, bb2 in all_els:
        if bb2[0] < 0.3 or bb2[1] < 0.3 or bb2[2] > W - 0.3 or bb2[3] > H - 0.3:
            if e['kind'] != 'screw':
                issues.append((e['id'], 'PANEL_EDGE', 0, 0))
    return data['module'], issues

import glob
files = sys.argv[1:]
if not files:
    files = [f for f in sorted(glob.glob('src/*.cpp'))
             if '@layout:begin' in open(f).read()]
total = 0
for f in files:
    mod, issues = audit(f)
    total += len(issues)
    print(f"== {mod}: {len(issues)} issue(s)")
    for a, b, g, need in issues:
        print(f"   {a} <-> {b}: gap {g}mm (need {need})")
sys.exit(1 if total else 0)
