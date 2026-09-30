#!/usr/bin/env python3
"""Generate res/rubigo.svg: the standard panel plus the destination switch's
leaders.

The panel is the one the panel editor draws from the @layout block in
src/rubigo.cpp. On top of it, two leaders run from the ends of the
destination switch (DEST_PARAM) to the labels of its positions: up from
the top of the switch and across to **pitch**, down from the bottom and
across to **cutoff**, and a short level line from its side to **noise**. The leaders are read from where the switch and the labels are, so
moving either in the editor moves them too.

Usage (from the repo root, with the fonttools venv the editor uses):
    ~/dl/audio/fonttools-venv/bin/python tools/panels/gen_rubigo_panel.py
The panel editor runs it on Save (`svg=` on the @layout line).
"""

import importlib.util
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CPP = os.path.join(ROOT, 'src', 'rubigo.cpp')
SVG = os.path.join(ROOT, 'res', 'rubigo.svg')

LEADER = '#bfbfbf'
WIDTH = 0.3
SWITCH_HALF_W = 2.28    # CKSSThree: 13.46 x 28.35 px at 75 dpi
SWITCH_HALF_H = 4.8
GAP_SWITCH = 0.6        # between the switch's end and the leader
GAP_LABEL = 0.8         # between the leader and its label
SWITCH, TOP, MIDDLE, BOTTOM = 'DEST_PARAM', 'LABEL_PIT', 'LABEL_NSE', 'LABEL_CUT'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def leaders(elems, text_w):
    by = {e['id']: e for e in elems}
    sw = by[SWITCH]
    out = []
    for lid, sign in ((TOP, -1), (BOTTOM, 1)):
        lb = by[lid]
        sz = lb.get('size', 2.2)
        mid = lb['y'] - sz / 2                       # the label's cap middle
        left = lb['x'] - text_w(lb['label'], sz) / 2 - GAP_LABEL
        start = sw['y'] + sign * (SWITCH_HALF_H + GAP_SWITCH)
        out.append(path(f'M{sw["x"]:.2f} {start:.2f} L{sw["x"]:.2f} {mid:.2f} L{left:.2f} {mid:.2f}'))
    # the middle one, a short level line from the switch's side, for symmetry
    lb = by[MIDDLE]
    sz = lb.get('size', 2.2)
    mid = lb['y'] - sz / 2
    left = lb['x'] - text_w(lb['label'], sz) / 2 - GAP_LABEL
    out.append(path(f'M{sw["x"] + SWITCH_HALF_W + GAP_SWITCH:.2f} {mid:.2f} L{left:.2f} {mid:.2f}'))
    return out


def path(d):
    return (f'<path d="{d}" fill="none" stroke="{LEADER}" stroke-width="{WIDTH}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>')


def main():
    pe = load('panel_editor', os.path.join(ROOT, 'tools', 'panel-editor', 'panel-editor.py'))
    layout = pe.parse_cpp(CPP)
    if not layout:
        print(f'no @layout block in {CPP}')
        return 1
    layout['svg_gen'] = None          # draw the standard panel, not this again
    if not pe.regen_svg(layout, SVG) and not os.path.exists(SVG):
        return 1
    svg = open(SVG).read()
    pe._ensure_fonttools()
    from fontTools.ttLib import TTFont
    font = TTFont(pe._find_font())
    glyphs, cmap, cap = font.getGlyphSet(), font.getBestCmap(), font['OS/2'].sCapHeight

    def text_w(txt, sz):
        return sum(glyphs.get(cmap.get(ord(c), c), glyphs.get('.notdef')).width for c in txt) * sz / cap

    group = '  <g id="leaders">\n    ' + '\n    '.join(leaders(layout['elements'], text_w)) + '\n  </g>\n'
    svg = svg.replace('</svg>', group + '</svg>', 1)
    with open(SVG, 'w') as f:
        f.write(svg)
    print(f'wrote {os.path.relpath(SVG, ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
