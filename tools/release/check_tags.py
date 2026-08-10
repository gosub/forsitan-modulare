#!/usr/bin/env python3
"""Check every module's `tags` in plugin.json against Rack's own tag list.

    python3 tools/release/check_tags.py            # the gate: nonzero on error
    python3 tools/release/check_tags.py --fetch    # refresh the cached list

The VCV Library rejects a manifest with an unknown tag, and it does so after
you have tagged and pushed, which makes it an expensive way to find out. That
happened once already: the library's build reported `Drone` on four modules
and `Tape` on a fifth, filed upstream as issue #19. This is the same check,
run before the tag rather than after.

The canonical list lives in Rack's `src/tag.cpp`, which is not in the SDK, so
a copy is cached next to this script as `rack_tags.txt` -- one line per tag,
the canonical name first and any aliases after it, tab separated. `--fetch`
re-reads the upstream file and rewrites the cache, reporting what changed;
run it now and then, and certainly if Rack has had a major release. The check
itself never touches the network, so it works on a plane and in CI.

Two severities:

  error    the tag is not in the list at all. The library will reject it.
  warning  the tag is a recognised alias rather than the canonical spelling
           ("Synth Voice" for "Synth voice"). The library accepts it; the
           browser groups by the canonical name, so it costs nothing but it
           reads as a typo in the manifest.
"""
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
CACHE = os.path.join(HERE, "rack_tags.txt")
MANIFEST = os.path.join(ROOT, "plugin.json")
UPSTREAM = "https://raw.githubusercontent.com/VCVRack/Rack/v2/src/tag.cpp"


def parse_tag_cpp(src):
    """The tagAliases table out of Rack's src/tag.cpp."""
    start = src.index("tagAliases = {")
    body = src[start:src.index("};", start)]
    groups = []
    for line in body.split("\n"):
        m = re.match(r'\s*\{(.+)\},?\s*$', line)
        if not m:
            continue
        names = re.findall(r'"([^"]+)"', m.group(1))
        if names:
            groups.append(names)
    return groups


def load_cache():
    if not os.path.exists(CACHE):
        sys.stderr.write(f"no cached tag list at {CACHE}; run --fetch\n")
        sys.exit(2)
    out = []
    with open(CACHE) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                out.append(line.split("\t"))
    return out


def fetch():
    with urllib.request.urlopen(UPSTREAM, timeout=30) as r:
        src = r.read().decode()
    groups = parse_tag_cpp(src)
    if not groups:
        sys.stderr.write("could not parse tagAliases from upstream\n")
        return 2
    old = load_cache() if os.path.exists(CACHE) else []
    with open(CACHE, "w") as fh:
        fh.write("# Rack's canonical module tags, cached from\n")
        fh.write(f"# {UPSTREAM}\n")
        fh.write("# canonical name first, then aliases, tab separated.\n")
        fh.write("# Refresh with: python3 tools/release/check_tags.py --fetch\n")
        for g in groups:
            fh.write("\t".join(g) + "\n")
    before = {g[0] for g in old}
    after = {g[0] for g in groups}
    added, gone = sorted(after - before), sorted(before - after)
    print(f"cached {len(groups)} tags from upstream")
    for t in added:
        print(f"  + {t}")
    for t in gone:
        print(f"  - {t}")
    if old and not added and not gone:
        print("  (unchanged)")
    return 0


def check():
    groups = load_cache()
    canonical = {g[0] for g in groups}
    lookup = {n.lower(): g[0] for g in groups for n in g}

    with open(MANIFEST) as fh:
        manifest = json.load(fh)

    errors, warnings = [], []
    for mod in manifest.get("modules", []):
        slug = mod.get("slug", "?")
        tags = mod.get("tags", [])
        if not tags:
            warnings.append((slug, "", "no tags at all"))
        for t in tags:
            hit = lookup.get(t.lower())
            if hit is None:
                errors.append((slug, t, "not a Rack tag"))
            elif hit != t:
                warnings.append((slug, t, f'non-canonical, Rack calls it "{hit}"'))
        if len(set(t.lower() for t in tags)) != len(tags):
            warnings.append((slug, "", "duplicate tags"))

    n = len(manifest.get("modules", []))
    for slug, t, why in warnings:
        print(f"warning  {slug:<12} {t!r:<20} {why}")
    for slug, t, why in errors:
        print(f"ERROR    {slug:<12} {t!r:<20} {why}")
    print(f"{n} modules checked against {len(canonical)} Rack tags: "
          f"{len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    if "--fetch" in sys.argv:
        sys.exit(fetch())
    sys.exit(check())
