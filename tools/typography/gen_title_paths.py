#!/usr/bin/env python3
"""
gen_title_paths.py — Generate NanoVG-compatible SVG <path> elements from a TTF font.

NanoVG limitation: compound paths (outer + inner contours) rendered with even-odd
fail when inner contours extend outside the outer path.

Fix pipeline per glyph:
  1. Draw with Qu2CuPen  → convert TrueType quadratic curves to cubic
  2. BooleanOperationManager.union  → clips inner contours strictly inside outer
  3. TransformPen(scale, 0, 0, -scale, x, baseline)  → SVG mm coordinates
  4. Emit <path fill-rule="evenodd"> — clean holes, no artifacts

Usage
-----
  python3 gen_title_paths.py \\
      --bold   /path/to/Font-Bold.ttf  \\
      --light  /path/to/Font-Light.ttf \\
      --titles "BASE:bold 64:light" "BTTN:bold 64:light" \\
      --panel-width 71.12 \\
      --cap-height 4.125 \\
      --baseline 8.125 \\
      --color "#d0d0d0"

Each --titles entry is a space-separated list of  TEXT:WEIGHT  segments.
Weights: bold | light | regular (only bold/light need separate TTF paths).

Output: SVG <g id="title"> blocks printed to stdout, one per --titles entry.

Dependencies (install into your project venv):
  pip install fonttools booleanOperations
"""
import argparse, sys
from fontTools.ttLib import TTFont
from fontTools.pens.recordingPen import RecordingPen, RecordingPointPen
from fontTools.pens.qu2cuPen import Qu2CuPen
from fontTools.pens.pointPen import SegmentToPointPen, PointToSegmentPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from booleanOperations import BooleanOperationManager


class _PointContour:
    def __init__(self, ops):
        self.ops = ops
    def __len__(self):
        return len(self.ops)
    def drawPoints(self, pp):
        for op, args, kwargs in self.ops:
            getattr(pp, op)(*args, **kwargs)


def glyph_to_svg_d(gs, gname, scale, tx, baseline_y):
    """Return SVG path d= string for one glyph, NanoVG-compatible."""
    rec_cubic = RecordingPen()
    gs[gname].draw(Qu2CuPen(rec_cubic, max_err=1.0, all_cubic=True))

    rpp = RecordingPointPen()
    spp = SegmentToPointPen(rpp)
    for op, args in rec_cubic.value:
        getattr(spp, op)(*args)

    contours, cur = [], []
    for op, args, kwargs in rpp.value:
        cur.append((op, args, kwargs))
        if op == 'endPath':
            contours.append(_PointContour(cur))
            cur = []

    mgr    = BooleanOperationManager()
    result = RecordingPointPen()
    mgr.union(contours, result)

    svg  = SVGPathPen(gs)
    tpen = TransformPen(svg, (scale, 0, 0, -scale, tx, baseline_y))
    p2s  = PointToSegmentPen(tpen)
    for op, args, kwargs in result.value:
        getattr(p2s, op)(*args, **kwargs)

    return svg.getCommands()


def glyph_advance(gs, gname, scale):
    return gs[gname].width * scale


def text_width(font, text, scale):
    gs   = font.getGlyphSet()
    cmap = font.getBestCmap()
    return sum(gs[cmap[ord(c)]].width * scale
               for c in text if ord(c) in cmap)


def emit_title(segments, fonts, panel_w, baseline_y, color):
    """
    segments : list of (text, weight_key)
    fonts    : dict  weight_key → (TTFont, scale, GlyphSet, cmap)
    """
    # measure total width for centering
    total_w = sum(text_width(fonts[wk][0], txt, fonts[wk][1])
                  for txt, wk in segments)
    x = (panel_w - total_w) / 2.0

    print('<g id="title">')
    for txt, wk in segments:
        font, scale, gs, cmap = fonts[wk]
        for ch in txt:
            cp = ord(ch)
            if cp not in cmap:
                x += 0
                continue
            gname = cmap[cp]
            d = glyph_to_svg_d(gs, gname, scale, x, baseline_y)
            if d:
                print(f'  <path fill="{color}" fill-rule="evenodd" d="{d}"/>')
            x += glyph_advance(gs, gname, scale)
    print('</g>')


def parse_title(spec):
    """Parse  "BASE:bold 64:light"  into  [("BASE","bold"), ("64","light")].
    Text segments may contain spaces: "COL 1:bold" → [("COL 1","bold")].
    """
    import re
    WEIGHTS = ('bold', 'light', 'regular')
    # Split on :weight boundaries (with capturing group so weights are kept).
    parts = re.split(r':(' + '|'.join(WEIGHTS) + r')(?=\s|$)', spec)
    # parts alternates:  text0, weight0, text1, weight1, ...  (trailing '' possible)
    result = []
    accumulated = ''
    for tok in parts:
        if tok in WEIGHTS:
            result.append((accumulated.strip(), tok))
            accumulated = ''
        else:
            accumulated += tok
    if accumulated.strip():
        result.append((accumulated.strip(), 'bold'))
    return result


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--bold',        required=True,  help='Path to Bold TTF')
    p.add_argument('--light',       default=None,   help='Path to Light TTF (optional)')
    p.add_argument('--regular',     default=None,   help='Path to Regular TTF (optional)')
    p.add_argument('--titles',      nargs='+', required=True,
                   help='One or more title specs, e.g. "BASE:bold 64:light"')
    p.add_argument('--panel-width', type=float, default=71.12, help='Panel width in mm')
    p.add_argument('--cap-height',  type=float, default=4.125, help='Desired cap height in mm')
    p.add_argument('--baseline',    type=float, default=None,
                   help='Baseline y in mm (default: cap-top at y=4mm)')
    p.add_argument('--color',       default='#d0d0d0', help='Fill colour')
    args = p.parse_args()

    baseline = args.baseline if args.baseline else 4.0 + args.cap_height

    def load(path, wk):
        if path is None:
            return None
        font  = TTFont(path)
        cap_h = font['OS/2'].sCapHeight or font['head'].unitsPerEm
        scale = args.cap_height / cap_h
        gs    = font.getGlyphSet()
        cmap  = font.getBestCmap()
        return (font, scale, gs, cmap)

    fonts = {}
    bold_data = load(args.bold, 'bold')
    if bold_data:
        fonts['bold'] = bold_data

    light_data = load(args.light, 'light')
    if light_data:
        fonts['light'] = light_data
    elif bold_data:
        fonts['light'] = bold_data   # fall back to bold if no light provided

    reg_data = load(args.regular, 'regular')
    if reg_data:
        fonts['regular'] = reg_data
    elif bold_data:
        fonts['regular'] = bold_data

    for i, spec in enumerate(args.titles):
        if i > 0:
            print()
        segments = parse_title(spec)
        emit_title(segments, fonts, args.panel_width, baseline, args.color)


if __name__ == '__main__':
    main()
