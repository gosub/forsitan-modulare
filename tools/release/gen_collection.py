#!/usr/bin/env python3
"""Regenerate the collection screenshot, img/forsitan-modulare.png.

Every module of the plugin, laid out in a few rows and captured from a
fullscreen Rack. The layout is the fiddly part: 25 panels from 3 to 36 HP
have to be split into rows of roughly equal width, or the block comes out
ragged and the zoom-to-fit wastes half the screen. This picks the row count
that best matches the screen's aspect ratio, then splits the modules into
rows with a dynamic program that minimises the widest row.

Rack is driven through limen (protocol 2, for move_module and batch): the
patch starts as one limen module, the rest are added, measured, and moved
into their slots, then the view is zoomed to fit and grabbed with grim.

    python3 tools/release/gen_collection.py --dry-run   # just print the layout
    python3 tools/release/gen_collection.py             # capture img/

Needs a built and installed plugin, a Rack binary, grim, and ImageMagick
only if --height is given.
"""

import argparse
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Rack's grid: one HP is 15px wide, one row 380px tall, at zoom 1.
HP_PX = 15
ROW_PX = 380
# A staging row far below the layout, one module per row so nothing collides
# while modules are being shuffled into place.
STAGE_ROW = 50


def die(msg):
    sys.exit("gen_collection: " + msg)


# ── limen client ─────────────────────────────────────────────────────────────

class Limen:
    """Minimal limen client: one JSON object per line, one reply per line."""

    def __init__(self, host, port, timeout=20.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def close(self):
        self.sock.close()

    def call(self, cmd, **fields):
        req = {"cmd": cmd}
        req.update(fields)
        self.sock.sendall((json.dumps(req) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                die("limen closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        resp = json.loads(line.decode())
        if not resp.get("ok"):
            die("limen: %s (%s)" % (resp.get("error", "unknown error"), cmd))
        return resp.get("result")

    def batch(self, commands):
        """Run commands in one round trip, failing on the first error."""
        result = self.call("batch", commands=commands, stopOnError=True)
        if result["failed"]:
            failing = result["results"][result["stopped"]]
            die("limen batch: %s (command %d: %s)"
                % (failing.get("error"), result["stopped"],
                   commands[result["stopped"]]["cmd"]))
        return [r["result"] for r in result["results"]]


# ── layout ───────────────────────────────────────────────────────────────────

def split_rows(widths, rows):
    """Split widths into `rows` consecutive groups, minimising the widest.

    Ties go to the split with the smallest spread, so rows come out even
    rather than merely under the cap. Returns a list of (start, end) index
    pairs.
    """
    n = len(widths)
    if rows > n:
        die("cannot split %d modules into %d rows" % (n, rows))
    prefix = [0]
    for w in widths:
        prefix.append(prefix[-1] + w)

    def total(i, j):
        return prefix[j] - prefix[i]

    # best[r][i] = (widest row, sum of squared row widths, split points) for
    # laying out widths[i:] in r rows. The squared sum is the tie-break: for
    # a fixed maximum it is smallest when the rest are as even as possible.
    best = {}

    def solve(i, r):
        if (i, r) in best:
            return best[(i, r)]
        if r == 1:
            width = total(i, n)
            result = (width, width * width, [n])
            best[(i, r)] = result
            return result
        chosen = None
        # Leave at least one module for each remaining row.
        for j in range(i + 1, n - r + 2):
            width = total(i, j)
            rest_max, rest_sq, rest_cuts = solve(j, r - 1)
            candidate = (max(width, rest_max), width * width + rest_sq,
                         [j] + rest_cuts)
            if chosen is None or candidate[:2] < chosen[:2]:
                chosen = candidate
        best[(i, r)] = chosen
        return chosen

    sys.setrecursionlimit(10000)
    _, _, cuts = solve(0, rows)
    bounds = []
    start = 0
    for cut in cuts:
        bounds.append((start, cut))
        start = cut
    return bounds


def best_row_count(widths, aspect, limit=6):
    """Row count whose block best matches the target aspect ratio."""
    scored = []
    for rows in range(1, min(limit, len(widths)) + 1):
        bounds = split_rows(widths, rows)
        width_hp = max(sum(widths[a:b]) for a, b in bounds)
        block = (width_hp * HP_PX) / (rows * ROW_PX)
        # Compare ratios, not differences: too wide and too tall cost the same.
        scored.append((abs(block / aspect - aspect / block), rows, bounds))
    scored.sort()
    return scored[0][1], scored[0][2]


def place(modules, bounds):
    """Assign a grid position to every module, rows centred on each other."""
    row_widths = [sum(m["hp"] for m in modules[a:b]) for a, b in bounds]
    widest = max(row_widths)
    layout = []
    for row, ((a, b), width) in enumerate(zip(bounds, row_widths)):
        x = (widest - width) // 2
        for module in modules[a:b]:
            layout.append(dict(module, x=x, y=row))
            x += module["hp"]
    return layout, row_widths, widest


def print_layout(layout, row_widths, widest, aspect):
    rows = len(row_widths)
    for row in range(rows):
        names = [m["slug"] for m in layout if m["y"] == row]
        print("  row %d  %3d HP  %s" % (row, row_widths[row], " ".join(names)))
    block = (widest * HP_PX) / (rows * ROW_PX)
    print("  block  %d HP x %d rows = %dx%d px, aspect %.2f (screen %.2f)"
          % (widest, rows, widest * HP_PX, rows * ROW_PX, block, aspect))


# ── capture ──────────────────────────────────────────────────────────────────

def plugin_arch_dir():
    """Rack's per-platform plugin directory name, e.g. plugins-lin-x64."""
    systems = {"Linux": "lin", "Darwin": "mac", "Windows": "win"}
    machines = {"x86_64": "x64", "AMD64": "x64", "arm64": "arm64", "aarch64": "arm64"}
    system = systems.get(platform.system())
    machine = machines.get(platform.machine())
    if not system or not machine:
        die("unsupported platform %s/%s" % (platform.system(), platform.machine()))
    return "plugins-%s-%s" % (system, machine)


def temp_user_dir():
    """A throwaway Rack user dir holding this plugin and nothing else.

    Keeps the shot to our own modules and to the build in this tree, with no
    dependence on what happens to be installed in the real Rack home.
    """
    if not os.path.exists(os.path.join(REPO, "plugin.so")):
        die("plugin.so not found -- run `make` first")
    userdir = tempfile.mkdtemp(prefix="forsitan-collection-")
    plugins = os.path.join(userdir, plugin_arch_dir())
    os.makedirs(plugins)
    os.symlink(REPO, os.path.join(plugins, "forsitan"))
    # A first-run Rack greets you with the tips dialog, and would draw the CPU
    # meter over the top-right module. Neither belongs in the picture.
    with open(os.path.join(userdir, "settings.json"), "w") as f:
        json.dump({"showTipsOnLaunch": False,
                   "cpuMeter": False,
                   "autoCheckUpdates": False}, f)
    return userdir


def start_rack(rack, patch, userdir):
    if not os.path.exists(patch):
        die("patch not found: %s" % patch)
    # Rack loads libRack.so and its res/ relative to its own directory.
    return subprocess.Popen([rack, "-u", userdir, patch], cwd=os.path.dirname(rack),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def connect(host, port, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return Limen(host, port)
        except OSError:
            time.sleep(0.5)
    die("limen never answered on %s:%d (is the server enabled in the patch?)" % (host, port))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", type=int,
                    help="number of rows (default: whatever fits the aspect ratio)")
    ap.add_argument("--aspect", type=float, default=1920 / 1200,
                    help="screen aspect ratio to lay out for (default: %(default).2f)")
    ap.add_argument("--out", default=os.path.join(REPO, "img", "forsitan-modulare.png"),
                    help="output image (default: img/forsitan-modulare.png)")
    ap.add_argument("--height", type=int,
                    help="scale the capture to this height in px (default: leave it)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the layout and stop, without starting Rack")
    ap.add_argument("--rack", help="path to the Rack binary (or set RACK_BIN)")
    ap.add_argument("--patch", default=os.path.join(REPO, "patches", "limen.vcv"),
                    help="patch to start from, holding one limen module")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7000)
    ap.add_argument("--output", help="grim output name, for a multi-monitor setup")
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds to wait after the zoom before grabbing (default: %(default)s)")
    ap.add_argument("--user-dir",
                    help="Rack user dir to run against (default: a throwaway one "
                         "holding only this plugin)")
    ap.add_argument("--keep-open", action="store_true",
                    help="leave Rack running after the capture")
    args = ap.parse_args()

    with open(os.path.join(REPO, "plugin.json")) as f:
        plugin = json.load(f)
    slugs = [m["slug"] for m in plugin["modules"]]

    if args.dry_run:
        # Widths come from Rack, so a dry run has to guess: the panel SVGs
        # carry the same width in mm, 1 HP = 5.08mm.
        widths = [svg_hp(slug) for slug in slugs]
        modules = [{"slug": s, "hp": w} for s, w in zip(slugs, widths)]
        rows = args.rows or best_row_count(widths, args.aspect)[0]
        bounds = split_rows(widths, rows)
        layout, row_widths, widest = place(modules, bounds)
        print("layout from the panel SVGs (%d modules, %d HP):" % (len(slugs), sum(widths)))
        print_layout(layout, row_widths, widest, args.aspect)
        return

    rack = args.rack or os.environ.get("RACK_BIN") or os.path.expanduser("~/dl/audio/rack/Rack")
    if not os.access(rack, os.X_OK):
        die("no Rack binary at %s; pass --rack or set RACK_BIN" % rack)
    grim = shutil.which("grim")
    if not grim:
        die("grim not found on PATH")

    userdir = args.user_dir or temp_user_dir()
    proc = start_rack(rack, args.patch, userdir)
    limen = None
    try:
        limen = connect(args.host, args.port, timeout=60)
        hello = limen.call("hello")
        if hello["protocol"] < 2:
            die("this Rack speaks limen protocol %d; protocol 2 is needed for "
                "move_module (build and install the plugin from this tree)"
                % hello["protocol"])

        # The starting patch is one limen module: keep it, add the rest.
        existing = limen.call("list_modules", plugin=plugin["slug"])
        if len(existing) != 1 or existing[0]["model"] != "limen":
            die("expected the patch to hold exactly one limen module, found %d"
                % len(existing))
        modules = [{"slug": "limen", "id": existing[0]["id"], "hp": existing[0]["hp"]}]

        added = limen.batch([
            {"cmd": "add_module", "plugin": plugin["slug"], "model": slug,
             "x": 0, "y": STAGE_ROW + i, "mode": "strict"}
            for i, slug in enumerate(slugs) if slug != "limen"])
        for slug, result in zip([s for s in slugs if s != "limen"], added):
            modules.append({"slug": slug, "id": result["id"], "hp": result["hp"]})
        # Back into the plugin's own order, so the picture reads like the
        # module table rather than like the order things were created in.
        modules.sort(key=lambda m: slugs.index(m["slug"]))

        widths = [m["hp"] for m in modules]
        rows = args.rows or best_row_count(widths, args.aspect)[0]
        bounds = split_rows(widths, rows)
        layout, row_widths, widest = place(modules, bounds)
        print("laying out %d modules, %d HP:" % (len(modules), sum(widths)))
        print_layout(layout, row_widths, widest, args.aspect)

        # limen is staged where it was; park it too, then everything lands on
        # empty grid and strict placement is exact.
        limen.call("move_module", id=modules[0]["id"], x=0, y=STAGE_ROW - 1, mode="strict")
        limen.batch([{"cmd": "move_module", "id": m["id"], "x": m["x"], "y": m["y"],
                      "mode": "strict"} for m in layout])

        # Fullscreen is what hides the menu bar and gives the whole screen to
        # the rack, but the compositor applies it in its own time: zoom before
        # the viewport has grown and the fit is computed against the old size.
        limen.call("set_fullscreen", on=True)
        time.sleep(args.settle)
        limen.call("zoom_to_modules")
        time.sleep(args.settle)

        capture = [grim]
        if args.output:
            capture += ["-o", args.output]
        capture.append(args.out)
        subprocess.run(capture, check=True)

        if args.height:
            magick = shutil.which("magick") or shutil.which("convert")
            if not magick:
                die("ImageMagick not found, needed for --height")
            subprocess.run([magick, args.out, "-filter", "Lanczos",
                            "-resize", "x%d" % args.height, "-strip", args.out],
                           check=True)
        print("wrote %s" % os.path.relpath(args.out, REPO))
    finally:
        if limen and not args.keep_open:
            try:
                limen.call("quit")
            except SystemExit:
                pass
        if limen:
            limen.close()
        if not args.keep_open:
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
            if not args.user_dir:
                shutil.rmtree(userdir, ignore_errors=True)


def svg_hp(slug):
    """Panel width in HP, read from the module's SVG (for --dry-run)."""
    import re
    path = os.path.join(REPO, "res", slug + ".svg")
    if not os.path.exists(path):
        die("no panel SVG for %s" % slug)
    with open(path) as f:
        head = f.read(2048)
    match = re.search(r'width="([0-9.]+)(mm)?"', head)
    if not match:
        die("no width in %s" % path)
    return int(round(float(match.group(1)) / 5.08))


if __name__ == "__main__":
    main()
