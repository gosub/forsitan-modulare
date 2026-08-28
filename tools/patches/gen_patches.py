#!/usr/bin/env python3
"""
gen_patches.py - Generate the example patches in patches/*.vcv.

A Rack 2 .vcv file is a zstd-compressed tar archive containing patch.json
(and an empty modules/ directory). Run from the repo root after changing
anything here:

    python3 tools/patches/gen_patches.py

Requires the `tar` and `zstd` command-line tools.
"""
import json, os, subprocess, tempfile

RACK_VERSION    = "2.6.6"
FORSITAN_VERSION = "2.3.0"


class Patch:
    def __init__(self):
        self.modules = []
        self.cables = []
        self._id = 0

    def add(self, plugin, model, pos, params=None, data=None, version=None):
        self._id += 1
        m = {"id": self._id, "plugin": plugin, "model": model, "pos": list(pos)}
        if version:
            m["version"] = version
        m["params"] = [{"id": i, "value": v} for i, v in (params or {}).items()]
        if data is not None:
            m["data"] = data
        self.modules.append(m)
        return self._id

    def write(self, path):
        patch = {"version": RACK_VERSION, "modules": self.modules, "cables": self.cables}
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, "modules"))
            with open(os.path.join(d, "patch.json"), "w") as f:
                json.dump(patch, f, indent=2)
            tar = path + ".tar"
            subprocess.run(["tar", "-cf", os.path.abspath(tar),
                            "./modules", "./patch.json"], cwd=d, check=True)
        subprocess.run(["zstd", "-q", "-f", tar, "-o", path], check=True)
        os.remove(tar)
        print("wrote", path)


def limen_patch():
    """Minimal patch: a single limen module with its server already enabled,
    so launching Rack with this file lands in a controllable state."""
    p = Patch()
    p.add("forsitan", "limen", (0, 0), version=FORSITAN_VERSION,
          data={"port": 7000, "serverEnabled": True})
    return p


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = os.path.join(root, "patches")
    limen_patch().write(os.path.join(out, "limen.vcv"))


if __name__ == "__main__":
    main()
