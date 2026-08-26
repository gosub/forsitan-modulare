#!/usr/bin/env python3
"""
vcvpatch.py -- build a Rack 2 .vcv file.

A .vcv is a zstd-compressed tar of ./patch.json and an empty ./modules/.
This is the same format tools/patches/gen_patches.py writes; that script
predates this module and builds one fixed patch, while this one is a library
for assembling arbitrary ones.

Positions are in Rack's own grid units: x counts 1HP columns, y counts rack
rows. Module and cable ids are arbitrary distinct integers.

Requires the `tar` and `zstd` command-line tools.
"""
import json
import os
import subprocess
import tempfile

RACK_VERSION = "2.6.6"

# Rack's cable colours, so a generated patch looks like a hand-made one
# rather than a wall of identical wires.
CABLE_COLORS = ["#f3374b", "#ffb437", "#00b56e", "#3695ef", "#8b4ade"]


class Patch:
    def __init__(self, version=RACK_VERSION):
        self.version = version
        self.modules = []
        self.cables = []
        self._id = 0

    def add(self, plugin, model, pos=(0, 0), params=None, data=None, version=None):
        """Add a module, returning its id. `params` maps param index to raw
        value -- raw meaning the param's own configParam range, not 0..1."""
        self._id += 1
        m = {"id": self._id, "plugin": plugin, "model": model, "pos": list(pos)}
        if version:
            m["version"] = version
        m["params"] = [{"id": int(i), "value": float(v)}
                       for i, v in sorted((params or {}).items())]
        if data is not None:
            m["data"] = data
        self.modules.append(m)
        return self._id

    def cable(self, out_module, out_port, in_module, in_port, color=None):
        """Patch one output into one input. Rack keys cables by port *index*,
        which is why the callers here resolve names to indices first."""
        self._id += 1
        if color is None:
            color = CABLE_COLORS[len(self.cables) % len(CABLE_COLORS)]
        self.cables.append({
            "id": self._id,
            "outputModuleId": out_module, "outputId": int(out_port),
            "inputModuleId": in_module, "inputId": int(in_port),
            "color": color,
        })

    def to_json(self):
        return {"version": self.version, "modules": self.modules,
                "cables": self.cables}

    def write(self, path):
        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, "modules"))
            with open(os.path.join(d, "patch.json"), "w") as f:
                json.dump(self.to_json(), f, indent=2)
            tar = os.path.join(d, "patch.tar")
            subprocess.run(["tar", "-cf", tar, "./modules", "./patch.json"],
                           cwd=d, check=True)
            subprocess.run(["zstd", "-q", "-f", tar, "-o", path], check=True)
        return path
