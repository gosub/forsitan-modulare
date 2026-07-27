#!/usr/bin/env python3
"""Regenerate the per-module panel images in img/.

Rack renders module panels itself: `Rack -t <zoom>` draws every module of
every installed plugin into <user-dir>/screenshots/<plugin>/<model>.png and
exits.  It is the same path VCV uses for the library's images, so the crop is
exact by construction -- no window, no screenshot tool, no cropping by hand.

The catch is "every installed plugin": pointed at a real Rack home it renders
thousands of panels.  So this script builds a throwaway user directory holding
this plugin alone, runs Rack against it, and scales each panel down to the
documented height.

    python3 tools/release/gen_screenshots.py                 # every module
    python3 tools/release/gen_screenshots.py alea antrum     # just these

Needs a built plugin (`make`), a Rack binary, and ImageMagick.
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import struct
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Panels are 380 px tall at zoom 1 (RACK_GRID_HEIGHT), so rendering at 2 and
# scaling to 600 resamples down, never up.
DEFAULT_ZOOM = 2.0
DEFAULT_HEIGHT = 600
RACK_GRID_HEIGHT = 380


def die(msg):
    sys.exit("gen_screenshots: " + msg)


def plugin_arch_dir():
    """Rack's per-platform plugin directory name, e.g. plugins-lin-x64."""
    systems = {"Linux": "lin", "Darwin": "mac", "Windows": "win"}
    machines = {"x86_64": "x64", "AMD64": "x64", "arm64": "arm64", "aarch64": "arm64"}
    system = systems.get(platform.system())
    machine = machines.get(platform.machine())
    if not system or not machine:
        die("unsupported platform %s/%s" % (platform.system(), platform.machine()))
    return "plugins-%s-%s" % (system, machine)


def png_size(path):
    with open(path, "rb") as f:
        header = f.read(24)
    return struct.unpack(">II", header[16:24])


def find_rack(explicit):
    for candidate in [explicit, os.environ.get("RACK_BIN"),
                      os.path.expanduser("~/dl/audio/rack/Rack"),
                      shutil.which("Rack")]:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    die("no Rack binary found; pass --rack or set RACK_BIN")


def find_magick():
    for name in ("magick", "convert"):
        found = shutil.which(name)
        if found:
            return found
    die("ImageMagick not found (need `magick` or `convert` on PATH)")


def render_panels(rack, zoom, verbose):
    """Run Rack's screenshot mode against a throwaway user dir.

    Returns the directory holding one PNG per module; the caller removes it.
    """
    library = os.path.join(REPO, "plugin.so")
    if not os.path.exists(library):
        die("plugin.so not found -- run `make` first")

    userdir = tempfile.mkdtemp(prefix="forsitan-screenshots-")
    plugins = os.path.join(userdir, plugin_arch_dir())
    os.makedirs(plugins)
    # An unpacked plugin directory is enough: Rack loads plugin.json + the
    # built library straight out of the source tree, so what gets rendered is
    # what is built right now.
    os.symlink(REPO, os.path.join(plugins, "forsitan"))

    cmd = [rack, "-t", str(zoom), "-u", userdir]
    print("rendering panels at %gx zoom ..." % zoom)
    if verbose:
        print("  " + " ".join(cmd))
    # Rack loads libRack.so and its res/ relative to its own directory, so it
    # has to be started from there.
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            cwd=os.path.dirname(rack), timeout=600)
    if result.returncode != 0:
        sys.stderr.write(result.stdout.decode("utf-8", "replace"))
        shutil.rmtree(userdir, ignore_errors=True)
        die("Rack exited %d" % result.returncode)
    if verbose:
        sys.stdout.write(result.stdout.decode("utf-8", "replace"))

    shots = os.path.join(userdir, "screenshots", "forsitan")
    if not os.path.isdir(shots):
        shutil.rmtree(userdir, ignore_errors=True)
        die("Rack rendered no forsitan panels (did the plugin fail to load?)")
    return userdir, shots


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("modules", nargs="*",
                    help="module slugs to update (default: all of them)")
    ap.add_argument("--rack", help="path to the Rack binary (or set RACK_BIN)")
    ap.add_argument("--zoom", type=float, default=DEFAULT_ZOOM,
                    help="render zoom, panels are 380px tall at 1 (default: %(default)s)")
    ap.add_argument("--height", type=int, default=DEFAULT_HEIGHT,
                    help="height of the written image in px (default: %(default)s)")
    ap.add_argument("--out", default=os.path.join(REPO, "img"),
                    help="output directory (default: img/)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the temporary user dir and say where it is")
    ap.add_argument("-v", "--verbose", action="store_true", help="show Rack's output")
    args = ap.parse_args()

    if args.height > RACK_GRID_HEIGHT * args.zoom:
        die("--height %d needs --zoom above %.2f, or the panels are upscaled"
            % (args.height, args.height / RACK_GRID_HEIGHT))

    with open(os.path.join(REPO, "plugin.json")) as f:
        slugs = [m["slug"] for m in json.load(f)["modules"]]
    if args.modules:
        unknown = [m for m in args.modules if m not in slugs]
        if unknown:
            die("not modules of this plugin: " + ", ".join(unknown))
        slugs = args.modules

    rack = find_rack(args.rack)
    magick = find_magick()
    userdir, shots = render_panels(rack, args.zoom, args.verbose)

    try:
        os.makedirs(args.out, exist_ok=True)
        for slug in slugs:
            src = os.path.join(shots, slug + ".png")
            if not os.path.exists(src):
                die("Rack rendered no panel for %s" % slug)
            # Documentation files are lowercase (doc/mmcccxcix.md), images
            # follow them; the model slug keeps its own capitalisation.
            dst = os.path.join(args.out, slug.lower() + ".png")
            # -strip drops the creation timestamp ImageMagick would otherwise
            # write, so re-running produces identical bytes and git stays quiet.
            subprocess.run([magick, src, "-filter", "Lanczos",
                            "-resize", "x%d" % args.height, "-strip", dst],
                           check=True)
            width, height = png_size(dst)
            print("  %-12s %4d x %4d" % (os.path.basename(dst), width, height))
    finally:
        if args.keep:
            print("kept %s" % userdir)
        else:
            shutil.rmtree(userdir, ignore_errors=True)

    print("wrote %d image%s to %s" %
          (len(slugs), "" if len(slugs) == 1 else "s",
           os.path.relpath(args.out, REPO)))


if __name__ == "__main__":
    main()
