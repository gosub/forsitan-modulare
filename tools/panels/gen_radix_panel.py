#!/usr/bin/env python3
"""Generate res/radix.svg: the label-free radix panel.

The panel has no text at all. The art is full-bleed branching growth in the
spirit of the Radical22's front (tangled branches, white over gold over
black), made by a random process from a fixed seed, so the panel regenerates
identically. The controls sit in the art on plain discs:

  - knobs and inputs on dark discs, cut out of the art
  - outputs on yellow discs: the output-badge grammar with the text removed
  - the stepped knobs (SRC, LAW, TABLE, BITS) with one tick per position

Positions come from the @layout block in src/radix.cpp, which is where the
panel editor keeps them. The growth keeps out of the screws and the logo, but
runs under the controls and the discs are drawn over it, opaque. Keeping it
out of the discs as well was tried first: a branch cannot find its way into
the narrow gaps inside a block of knobs and jacks, so every block sat in a
black blob several times the size of its discs, where on the hardware the
art runs right up to each control.

NanoSVG constraints: filled paths only, no strokes on the art, no gradients,
filters, masks or clip paths. Each branch is one closed outline (its left
edge forward, its right edge back), so every outline winds the same way and a
whole layer can be one compound path under the nonzero rule: overlapping
branches union instead of punching holes in each other.

Usage (from the repo root):
    python3 tools/panels/gen_radix_panel.py            # write res/radix.svg
    python3 tools/panels/gen_radix_panel.py --seed 7   # try another seed
"""

import argparse
import importlib.util
import math
import os
import random
import sys

SEED = 22

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CPP = os.path.join(ROOT, 'src', 'radix.cpp')
SVG = os.path.join(ROOT, 'res', 'radix.svg')

BG = '#1a1a1a'
WHITE = '#e5e5e5'
YELLOW = '#ffd500'

# knob positions per stepped control, and Rack's default knob sweep
STEPS = {'SRC_PARAM': 5, 'LAW_PARAM': 5, 'TABLE_PARAM': 6, 'BITS_PARAM': 16}
SWEEP = 0.83 * math.pi

DISC_PAD = 0.8        # disc radius past the widget's own
TICK_IN, TICK_OUT = 0.4, 1.3   # ticks, past the knob's edge
STEP = 0.8            # growth step, mm


def load_layout():
    spec = importlib.util.spec_from_file_location(
        'pe', os.path.join(ROOT, 'tools', 'panel-editor', 'panel-editor.py'))
    pe = importlib.util.module_from_spec(spec)
    argv, sys.argv = sys.argv, ['x']
    spec.loader.exec_module(pe)
    sys.argv = argv
    return pe.parse_cpp(CPP)


class Field:
    """The keep-out geometry: circles, and the logo's rectangle."""

    def __init__(self, layout):
        self.W, self.H = layout['panel_w'], layout['panel_h']
        self.discs = []      # (x, y, r, kind, id, widget r)
        self.circles = []    # keep-out circles
        self.rects = []      # keep-out rectangles (x0, y0, x1, y1)
        self.logo = None
        for e in layout['elements']:
            k = e['kind']
            if k in ('param', 'input', 'output'):
                r = e['radius'] + DISC_PAD
                if e['id'] in STEPS:
                    r = max(r, e['radius'] + TICK_OUT + 0.3)
                self.discs.append((e['x'], e['y'], r, k, e['id'], e['radius']))
            elif k == 'screw':
                # parse_cpp gives the screw's visual centre
                self.circles.append((e['x'], e['y'], 4.4))
            elif k == 'logo':
                self.logo = (e['x'], e['y'])
                self.rects.append((e['x'] - 7.5, e['y'] - 4.0,
                                   e['x'] + 7.5, e['y'] + 4.0))

    def clearance(self, x, y):
        """Distance to the nearest keep-out, negative inside one."""
        d = 1e9
        for cx, cy, r in self.circles:
            d = min(d, math.hypot(x - cx, y - cy) - r)
        for x0, y0, x1, y1 in self.rects:
            dx = max(x0 - x, 0.0, x - x1)
            dy = max(y0 - y, 0.0, y - y1)
            if dx == 0.0 and dy == 0.0:
                d = min(d, -min(x - x0, x1 - x, y - y0, y1 - y))
            else:
                d = min(d, math.hypot(dx, dy))
        return d

    def inside(self, x, y, m=1.5):
        return -m <= x <= self.W + m and -m <= y <= self.H + m


class Coverage:
    """A 1 mm grid of how much art each cell already has."""

    def __init__(self, W, H):
        self.nx, self.ny = int(W) + 1, int(H) + 1
        self.c = [[0] * self.nx for _ in range(self.ny)]

    def mark(self, x, y, w):
        r = int(w / 2) + 1
        ix, iy = int(x), int(y)
        for j in range(max(0, iy - r), min(self.ny, iy + r + 1)):
            for i in range(max(0, ix - r), min(self.nx, ix + r + 1)):
                self.c[j][i] += 1

    def around(self, x, y, r=4):
        ix, iy = int(x), int(y)
        t = 0
        for j in range(max(0, iy - r), min(self.ny, iy + r + 1)):
            for i in range(max(0, ix - r), min(self.nx, ix + r + 1)):
                t += self.c[j][i] > 0
        return t


def grow_tree(field, cov, rng, x, y, a, w, wmin, split, taper, wander):
    """One tree from (x, y): outlines of the trunk and every branch off it.

    Each branch is a random walk whose heading drifts by `wander` a step and
    whose width shrinks by `taper`, splitting with probability `split` a
    step. A branch stops at the panel edge, at a keep-out, or once it is
    thinner than `wmin`.
    """
    outlines = []
    stack = [(x, y, a, w)]
    while stack and len(outlines) < 24:
        x, y, a, w = stack.pop()
        pts = [(x, y, w)]
        while w > wmin:
            a += rng.gauss(0.0, wander)
            nx, ny = x + STEP * math.cos(a), y + STEP * math.sin(a)
            if not field.inside(nx, ny) or field.clearance(nx, ny) < w / 2:
                break
            x, y = nx, ny
            w *= taper
            pts.append((x, y, w))
            cov.mark(x, y, w)
            if rng.random() < split:
                side = rng.choice((-1.0, 1.0))
                stack.append((x, y, a + side * rng.uniform(0.35, 0.9),
                              w * rng.uniform(0.55, 0.85)))
                a -= side * rng.uniform(0.05, 0.3)
        if len(pts) >= 3:
            outlines.append(outline(pts))
    return outlines


def grow_layer(field, rng, trees, from_edge, **kw):
    """`trees` trees, each rooted wherever the layer is emptiest.

    A root is the least covered of a handful of random candidates, so the
    layer fills the panel evenly instead of piling up where it started.
    `from_edge` of them grow in from the panel edge, as the long limbs do.
    """
    cov = Coverage(field.W, field.H)
    outlines = []
    for t in range(trees):
        if t < from_edge:
            cands = edge_starts(field, rng, 8)
        else:
            cands = inner_starts(field, rng, 8)
        x, y, a = min(cands, key=lambda c: cov.around(c[0], c[1]))
        w = kw['w0'] * rng.uniform(0.6, 1.0)
        outlines += grow_tree(field, cov, rng, x, y, a, w, kw['wmin'],
                              kw['split'], kw['taper'], kw['wander'])
    return outlines


def outline(pts):
    """A branch's closed outline: left edge forward, right edge back."""
    left, right = [], []
    n = len(pts)
    for i, (x, y, w) in enumerate(pts):
        x0, y0, _ = pts[max(i - 1, 0)]
        x1, y1, _ = pts[min(i + 1, n - 1)]
        dx, dy = x1 - x0, y1 - y0
        d = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / d, dx / d
        h = w / 2
        left.append((x + nx * h, y + ny * h))
        right.append((x - nx * h, y - ny * h))
    return simplify(left) + simplify(right[::-1])


def simplify(pts, tol=0.08):
    """Douglas-Peucker: drop points within `tol` mm of the line through
    their neighbours. Most of a random walk's points are."""
    if len(pts) < 3:
        return pts
    (x0, y0), (x1, y1) = pts[0], pts[-1]
    dx, dy = x1 - x0, y1 - y0
    d = math.hypot(dx, dy) or 1e-9
    worst, wi = -1.0, 0
    for i in range(1, len(pts) - 1):
        px, py = pts[i]
        e = abs(dy * (px - x0) - dx * (py - y0)) / d
        if e > worst:
            worst, wi = e, i
    if worst <= tol:
        return [pts[0], pts[-1]]
    return simplify(pts[:wi + 1], tol)[:-1] + simplify(pts[wi:], tol)


def num(v):
    # 0.1 mm is finer than the art needs; "-.3" rather than "-0.3"
    t = '%.1f' % v
    if t in ('0.0', '-0.0'):
        return '0'
    t = t.rstrip('0').rstrip('.') if '.' in t else t
    return t.replace('0.', '.', 1) if t.startswith(('0.', '-0.')) else t


def path_d(outlines):
    """Relative coordinates, rounded, on a running position so the rounding
    never accumulates."""
    parts = []
    for poly in outlines:
        qx, qy = round(poly[0][0], 1), round(poly[0][1], 1)
        p = ['M' + num(qx) + ' ' + num(qy) + 'l']
        steps = []
        for x, y in poly[1:]:
            rx, ry = round(x - qx, 1), round(y - qy, 1)
            if rx == 0 and ry == 0:
                continue
            qx, qy = qx + rx, qy + ry
            a, b = num(rx), num(ry)
            steps.append(a + ('' if b.startswith('-') else ' ') + b)
        if len(steps) < 2:
            continue
        parts.append(p[0] + ' '.join(steps) + 'z')
    return ''.join(parts)


def edge_starts(field, rng, n):
    """Points on the panel edge, heading inwards."""
    W, H = field.W, field.H
    out = []
    while len(out) < n:
        side = rng.randrange(4)
        if side == 0:
            x, y, a = rng.uniform(0, W), -0.5, math.pi / 2
        elif side == 1:
            x, y, a = rng.uniform(0, W), H + 0.5, -math.pi / 2
        elif side == 2:
            x, y, a = -0.5, rng.uniform(0, H), 0.0
        else:
            x, y, a = W + 0.5, rng.uniform(0, H), math.pi
        if field.clearance(x, y) > 1.0:
            out.append((x, y, a + rng.gauss(0.0, 0.4)))
    return out


def inner_starts(field, rng, n):
    """Points clear of every keep-out, heading anywhere."""
    out = []
    while len(out) < n:
        x, y = rng.uniform(0, field.W), rng.uniform(0, field.H)
        if field.clearance(x, y) > 1.0:
            out.append((x, y, rng.uniform(0, 2 * math.pi)))
    return out


def discs_svg(field):
    lines = []
    for x, y, r, kind, eid, wr in field.discs:
        lines.append(f'  <circle cx="{x:.2f}" cy="{y:.2f}" r="{r:.2f}" fill="{BG}"/>')
        if kind == 'output':
            # yellow inside a dark ring, or it melts into the gold art
            lines.append(f'  <circle cx="{x:.2f}" cy="{y:.2f}" r="{r - 0.7:.2f}" '
                         f'fill="{YELLOW}"/>')
        n = STEPS.get(eid)
        if not n:
            continue
        # one tick per knob position, in Rack's sweep, 0 = straight up
        ticks = []
        for i in range(n):
            a = -SWEEP + 2 * SWEEP * i / (n - 1)
            sx, sy = math.sin(a), -math.cos(a)
            tx, ty = math.cos(a), math.sin(a)      # across the tick
            half = 0.25 if n <= 6 else 0.14
            r0, r1 = wr + TICK_IN, wr + (TICK_OUT if n <= 6 else TICK_OUT - 0.4)
            quad = [(x + sx * r0 + tx * half, y + sy * r0 + ty * half),
                    (x + sx * r1 + tx * half, y + sy * r1 + ty * half),
                    (x + sx * r1 - tx * half, y + sy * r1 - ty * half),
                    (x + sx * r0 - tx * half, y + sy * r0 - ty * half)]
            ticks.append(quad)
        lines.append(f'  <path d="{path_d(ticks)}" fill="{WHITE}"/>')
    return lines


def logo_svg(cx, cy):
    # the horizontal forsitan logo, as tools/panel-editor draws it
    bx, by = cx - 6.25, cy - 2.75
    return [
        f'  <rect x="{bx - 1.2:.2f}" y="{by - 1.2:.2f}" width="14.9" height="7.9" '
        f'rx="2" fill="{BG}"/>',
        f'  <rect x="{bx:.2f}" y="{by:.2f}" width="12.5" height="5.5" rx="1.2" '
        f'fill="none" stroke="{YELLOW}" stroke-width="0.5"/>',
        f'  <rect x="{cx - 0.125:.2f}" y="{cy - 2.25:.2f}" width="0.25" height="4.5" '
        f'fill="{YELLOW}"/>',
        f'  <circle cx="{cx - 2.063:.2f}" cy="{cy + 1.361:.2f}" r="0.625" fill="{YELLOW}"/>',
        f'  <circle cx="{cx - 4.180:.2f}" cy="{cy - 1.285:.2f}" r="0.625" fill="{YELLOW}"/>',
        f'  <circle cx="{cx + 4.066:.2f}" cy="{cy + 1.361:.2f}" r="0.625" fill="{YELLOW}"/>',
        f'  <circle cx="{cx + 1.949:.2f}" cy="{cy - 1.285:.2f}" r="0.625" fill="{YELLOW}"/>',
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--seed', type=int, default=SEED)
    ap.add_argument('--out', default=SVG)
    args = ap.parse_args()

    layout = load_layout()
    field = Field(layout)
    rng = random.Random(args.seed)

    # gold underneath: few, broad, slow to branch
    gold = grow_layer(field, rng, 26, 10, w0=3.4, wmin=0.5, split=0.03,
                      taper=0.992, wander=0.10)
    # white over it: the tangle
    white = grow_layer(field, rng, 70, 24, w0=1.4, wmin=0.15, split=0.09,
                       taper=0.985, wander=0.15)
    # black cracks through both, the texture the photo has
    black = grow_layer(field, rng, 36, 10, w0=0.5, wmin=0.1, split=0.05,
                       taper=0.988, wander=0.2)

    W, H = field.W, field.H
    lines = [
        '<?xml version="1.0" encoding="UTF-8" standalone="no"?>',
        f'<!-- generated by tools/panels/gen_radix_panel.py, seed {args.seed}; '
        'do not edit -->',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" '
        f'viewBox="0 0 {W} {H}">',
        f'  <rect width="{W}" height="{H}" fill="{BG}"/>',
        f'  <path d="{path_d(gold)}" fill="{YELLOW}" fill-rule="nonzero"/>',
        f'  <path d="{path_d(white)}" fill="{WHITE}" fill-rule="nonzero"/>',
        f'  <path d="{path_d(black)}" fill="{BG}" fill-rule="nonzero"/>',
    ]
    lines += discs_svg(field)
    if field.logo:
        lines += logo_svg(*field.logo)
    lines.append('</svg>')

    with open(args.out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    size = os.path.getsize(args.out)
    print(f'{os.path.relpath(args.out, ROOT)}: {len(gold)} gold, {len(white)} white, '
          f'{len(black)} black branches, {size / 1024:.0f} KB')


if __name__ == '__main__':
    main()
