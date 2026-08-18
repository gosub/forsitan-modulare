#!/usr/bin/env python3
"""Find symbols two modules both define.

    python3 tools/release/check_symbols.py          # needs a built tree

Every module is a separate translation unit linked into one plugin, so a
file-scope type in `src/<a>.cpp` shares a namespace with one in `src/<b>.cpp`.
Two of the same name is an ODR violation, and what happens next depends on the
linker rather than on the code:

  - MinGW refuses to link, and the Windows job fails *after* the tag is pushed.
  - ELF merges them silently. A strong definition in one file wins over an
    inline one in another, and the loser's calls quietly run the winner's code.

The second is the dangerous one. It cost v2.15.0 a re-tag: caligo and materiae
both declared `TimeQuantity`, caligo defined its member out of line and
materiae in-class, and on every Linux build materiae's time tooltips ran
caligo's implementation. Nothing in the test suite touches a tooltip, so
nothing caught it.

The fix is an anonymous namespace around anything file-local. This checks that
it happened, by reading the object files rather than guessing from the source:
a name is a problem when two objects both define it strongly, or when one
defines it strongly and another weakly.
"""
import collections
import os
import subprocess
import sys

STRONG = set("TDBRtdbr".upper())      # T text, D data, B bss, R rodata
WEAK = set("WVwv".upper())


def symbols(obj):
    out = subprocess.run(["nm", "-C", "--defined-only", obj],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return []
    found = []
    for line in out.stdout.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3:
            continue
        kind, name = parts[1].strip(), parts[2].strip()
        if not kind:
            continue
        if kind.islower():
            continue                   # local to its object: nothing to clash
        if name.startswith("(anonymous namespace)"):
            continue
        found.append((kind.upper(), name))
    return found


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build = os.path.join(repo, "build", "src")
    if not os.path.isdir(build):
        print("no build/src: run make first", file=sys.stderr)
        return 2

    objs = sorted(f for f in os.listdir(build) if f.endswith(".o"))
    where = collections.defaultdict(list)
    for o in objs:
        for kind, name in symbols(os.path.join(build, o)):
            where[name].append((o[:-6], kind))     # strip ".cpp.o"

    problems = []
    for name, defs in sorted(where.items()):
        if len(defs) < 2:
            continue
        kinds = {k for _, k in defs}
        strong = [m for m, k in defs if k in STRONG]
        if len(strong) > 1:
            problems.append(("multiply defined", name, defs))
        elif strong and (kinds & WEAK):
            problems.append(("strong here, inline there", name, defs))

    if not problems:
        print(f"{len(objs)} objects, {len(where)} exported symbols: "
              "no name defined by two modules")
        return 0

    print(f"{len(problems)} symbol(s) defined by more than one module:\n")
    for why, name, defs in problems:
        print(f"  {name}")
        print(f"    {why}: " + ", ".join(f"{m} ({k})" for m, k in defs))
    print("\nPut file-local types in an anonymous namespace.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
