#!/usr/bin/env python3
"""Generate res/spira.svg: the standard panel over a faint nautilus.

The panel is the one the panel editor draws from the @layout block in
src/spira.cpp (title, labels, badges, logo). Behind all of it, just above
the background, sits the cross-section of a nautilus shell, in greys a few
steps off the background so it reads as texture and never as a label:

  - the wall: a logarithmic spiral, r = a e^(b theta), widening threefold
    per whorl as a nautilus does (b = ln 3 / 2 pi); one curve drawn over
    several turns is every whorl's wall at once
  - the shell filled one step lighter, closed by the aperture
  - the septa: the chamber walls, from each point of the outer whorl to the
    whorl inside it, bowed back toward the centre as real septa are
  - the siphuncle: the tube through the septa, a spiral of its own part way
    across each chamber

Everything is plain filled and stroked paths, which NanoSVG draws.

Usage (from the repo root, with the fonttools venv the editor uses):
    ~/dl/audio/fonttools-venv/bin/python tools/panels/gen_spira_panel.py
The panel editor runs it on Save (`svg=` on the @layout line).
"""

import importlib.util
import math
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CPP = os.path.join(ROOT, 'src', 'spira.cpp')
SVG = os.path.join(ROOT, 'res', 'spira.svg')

WALL = '#2e2e2e'        # the spiral wall and the aperture
SEPTUM = '#272727'      # chamber walls and the siphuncle
CHAMBER = '#1e1e1e'     # the shell's fill, one step off #1a1a1a

GROWTH = 3.0            # the whorl widens this much per turn
B = math.log(GROWTH) / (2 * math.pi)
TURNS = 3.4             # whorls drawn
SEPTA_PER_TURN = 13     # chambers per whorl, as on a grown shell
BODY = 0.3              # the last chamber, in turns, has no septa
SEPTUM_BOW = 0.35       # radians: how far a septum's middle bows back toward the apex
MIN_SEPTUM = 2.5        # mm: no septa smaller than this, near the apex
SIPHUNCLE = 0.45        # where the siphuncle runs, from the inner whorl (0) to the outer (1)

# The shell is fitted to the panel by its bounding box: this tall, centred
# here, turned so the aperture opens downward.
HEIGHT = 112.0          # mm
CENTRE = (66.04, 64.0)  # mm
ROTATE = 0.35           # turns
STEP = 0.02             # radians between points


class Shell:
    """A logarithmic spiral placed on the panel."""

    def __init__(self, a, cx, cy):
        self.a, self.cx, self.cy = a, cx, cy
        self.end = TURNS * 2 * math.pi

    def at(self, theta, r):
        t = theta + ROTATE * 2 * math.pi
        return self.cx + r * math.cos(t), self.cy - r * math.sin(t)

    def r(self, theta):
        return self.a * math.exp(B * theta)

    def wall(self, theta):
        return self.at(theta, self.r(theta))

    def curve(self, t0, t1, f=lambda th: 1.0):
        n = max(1, int((t1 - t0) / STEP))
        return [self.at(t0 + (t1 - t0) * i / n, self.r(t0 + (t1 - t0) * i / n) * f(t0 + (t1 - t0) * i / n))
                for i in range(n + 1)]

    def outline(self):
        """The shell's edge: the last whorl and the aperture closing it."""
        return self.curve(self.end - 2 * math.pi, self.end)


def placed():
    probe = Shell(1.0, 0.0, 0.0)
    xs, ys = zip(*probe.outline())
    k = HEIGHT / (max(ys) - min(ys))
    cx = CENTRE[0] - k * 0.5 * (max(xs) + min(xs))
    cy = CENTRE[1] - k * 0.5 * (max(ys) + min(ys))
    return Shell(k, cx, cy)


def pts(points):
    return ' '.join(f'{x:.2f} {y:.2f}' for x, y in points)


def polyline(points, colour, width, closed=False):
    return (f'<path d="M{pts(points[:1])} L{pts(points[1:])}{" Z" if closed else ""}" fill="none" '
            f'stroke="{colour}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>')


def art():
    sh = placed()
    out = []
    edge = sh.outline()
    out.append(f'<path d="M{pts(edge[:1])} L{pts(edge[1:])} Z" fill="{CHAMBER}"/>')
    out.append(polyline(sh.curve(0.0, sh.end), WALL, 0.7))
    out.append(polyline([sh.wall(sh.end), sh.wall(sh.end - 2 * math.pi)], WALL, 0.7))

    # the septa, quadratic curves bowed back toward the apex
    last = sh.end - BODY * 2 * math.pi
    first = None
    k = 0
    while True:
        th = last - k * 2 * math.pi / SEPTA_PER_TURN
        o, i = sh.wall(th), sh.wall(th - 2 * math.pi)
        if th < 2 * math.pi or math.dist(o, i) < MIN_SEPTUM:
            break
        first = th
        k += 1
        rm = 0.5 * (sh.r(th) + sh.r(th - 2 * math.pi))
        c = sh.at(th - SEPTUM_BOW, rm)
        width = 0.45 if math.dist(o, i) > 8 else 0.3
        out.append(f'<path d="M{o[0]:.2f} {o[1]:.2f} Q{c[0]:.2f} {c[1]:.2f} {i[0]:.2f} {i[1]:.2f}" '
                   f'fill="none" stroke="{SEPTUM}" stroke-width="{width}" stroke-linecap="round"/>')

    # the siphuncle: a spiral of its own, part way across each chamber, from
    # the first septum drawn to the last
    if first is not None:
        inner = 1.0 / GROWTH
        f = lambda th: inner + SIPHUNCLE * (1.0 - inner)
        out.append(polyline(sh.curve(first - 2 * math.pi / SEPTA_PER_TURN, last, f), SEPTUM, 0.35))
    return out


def main():
    spec = importlib.util.spec_from_file_location(
        'panel_editor', os.path.join(ROOT, 'tools', 'panel-editor', 'panel-editor.py'))
    pe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pe)
    layout = pe.parse_cpp(CPP)
    if not layout:
        print(f'no @layout block in {CPP}')
        return 1
    layout['svg_gen'] = None          # draw the standard panel, not this again
    if not pe.regen_svg(layout, SVG) and not os.path.exists(SVG):
        return 1
    svg = open(SVG).read()
    bg = '<rect width="132.08" height="128.5" fill="#1a1a1a"/>'
    if bg not in svg:
        print('background rect not found')
        return 1
    group = '\n  <g id="nautilus">\n    ' + '\n    '.join(art()) + '\n  </g>'
    svg = svg.replace(bg, bg + group, 1)
    with open(SVG, 'w') as f:
        f.write(svg)
    print(f'wrote {os.path.relpath(SVG, ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
