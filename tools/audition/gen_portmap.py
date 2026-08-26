#!/usr/bin/env python3
"""
gen_portmap.py -- record the port and param names of the modules an audition
bench is built from.

forsitan's own modules are read straight out of src/<slug>.cpp by modspec.py.
The bench also uses other people's modules -- Fundamental's VCO and VCA, Core's
Audio 2 and Notes -- whose sources are not in this tree, and whose port indices
must never be guessed: a wrong index silently patches the wrong jack, and the
audition then tests something other than what it says.

So they are asked. This drives a real Rack through limen, adds each module,
records what list_ports and list_params answer, and writes portmap.json. Run
it again when a plugin is updated, or to add a module to the bench:

    python3 tools/audition/gen_portmap.py
    python3 tools/audition/gen_portmap.py --add 4msCompany/BWAVP

Needs a built and installed plugin (limen has to be in the Rack it launches).
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.dirname(os.path.abspath(__file__))
PORTMAP = os.path.join(HERE, "portmap.json")

# The bench's stock cast. Fundamental is the default answer for a source or a
# utility: it ships with Rack, so an audition written against it runs anywhere.
DEFAULT = [
    "Core/AudioInterface2", "Core/Notes",
    "Fundamental/VCO", "Fundamental/VCO2", "Fundamental/LFO", "Fundamental/VCA",
    "Fundamental/VCA-1", "Fundamental/Noise", "Fundamental/Random",
    "Fundamental/Mixer", "Fundamental/VCMixer", "Fundamental/8vert",
    "Fundamental/ADSR", "Fundamental/Scope", "Fundamental/Split",
    "Fundamental/Merge", "Fundamental/Quantizer", "Fundamental/SEQ3",
]


def die(msg):
    sys.exit("gen_portmap: " + msg)


class Limen:
    def __init__(self, host="127.0.0.1", port=7000, timeout=20.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

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
            return None
        return resp.get("result")

    def close(self):
        self.sock.close()


def connect(timeout=40.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return Limen()
        except OSError:
            time.sleep(0.5)
    die("limen never answered on 127.0.0.1:7000")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--add", action="append", default=[],
                    help="extra plugin/model to record, repeatable")
    ap.add_argument("--rack", default=os.path.expanduser("~/dl/audio/rack/Rack"))
    ap.add_argument("--user-dir",
                    default=os.path.expanduser("~/dl/audio/rackhome/.local/share/Rack2"),
                    help="Rack user dir; the real one, so third-party plugins are there")
    args = ap.parse_args()

    wanted = DEFAULT + args.add
    patch = os.path.join(REPO, "patches", "limen.vcv")
    if not os.path.exists(patch):
        die("patches/limen.vcv not found -- run tools/patches/gen_patches.py")

    proc = subprocess.Popen([args.rack, "-u", args.user_dir, patch],
                            cwd=os.path.dirname(args.rack),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = {}
    try:
        lim = connect()
        for spec in wanted:
            plug, model = spec.split("/", 1)
            r = lim.call("add_module", plugin=plug, model=model, x=0, y=20)
            if not r:
                sys.stderr.write("  skip %s (not installed?)\n" % spec)
                continue
            mid = r["id"]
            ports = lim.call("list_ports", id=mid) or {}
            params = lim.call("list_params", id=mid) or []
            out[spec] = {
                "inputs": [p.get("name") or "" for p in ports.get("inputs", [])],
                "outputs": [p.get("name") or "" for p in ports.get("outputs", [])],
                # min/max as well as the name: a bench that sets someone
                # else's knob needs its range to check the value against.
                "params": [{"name": p.get("name") or "",
                            "min": p.get("min", 0.0), "max": p.get("max", 1.0),
                            "default": p.get("value", 0.0)}
                           for p in params],
            }
            print("  %-28s %2d in  %2d out  %2d params"
                  % (spec, len(out[spec]["inputs"]), len(out[spec]["outputs"]),
                     len(out[spec]["params"])))
            lim.call("remove_module", id=mid)
        lim.call("quit")
        lim.close()
    finally:
        # Rack is asked to quit through limen and then waited for. Killing it
        # loses whatever it was going to write on the way out -- the autosave
        # and settings.json among it -- and this runs against the real user
        # dir, so that is the user's own Rack state.
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            die("Rack did not quit after limen asked it to; leaving it running "
                "(pid %d) rather than killing it" % proc.pid)

    if not out:
        die("nothing recorded")
    old = {}
    if os.path.exists(PORTMAP):
        with open(PORTMAP) as f:
            old = json.load(f)
    old.update(out)
    with open(PORTMAP, "w") as f:
        json.dump(old, f, indent=2, sort_keys=True)
    print("wrote %s (%d modules)" % (os.path.relpath(PORTMAP, REPO), len(old)))


if __name__ == "__main__":
    main()
