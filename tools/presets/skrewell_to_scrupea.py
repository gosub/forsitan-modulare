#!/usr/bin/env python3
"""Turn the snapshots in a Reaktor ensemble into scrupea presets.

    python3 tools/presets/skrewell_to_scrupea.py <ensemble.ens> <outdir>

This reads an ensemble **you supply** and writes `.vcvm` files next to it. It
ships no snapshot data of its own, and none is checked into this repository:
Skrewell's factory snapshots are Native Instruments' content, names and all,
and converting your own copy for your own use is a different thing from
redistributing them. Point it at your Reaktor installation and you get your
presets; nobody else gets them from us.

The format it reads, in short: one block per snapshot in the tail after the
last KSModul record, each block ending with its name, and inside it one
record per snapshot-enabled control, marked by a run of five 1s, holding one
normalised value for a knob or eight for a polycontrol's per-voice bars.
"""
import json
import os
import struct
import sys

# ---------------------------------------------------------------- reading ---

def records(d):
    """(offset, class, end) for every bracketed record."""
    out, i, n = [], 0, len(d)
    while i < n - 8:
        if d[i] == 0x5b:                       # '['
            ln = struct.unpack_from('<I', d, i + 1)[0]
            if 4 <= ln <= 16 and i + 5 + ln <= n:
                cls = d[i+5:i+5+ln]
                if all(65 <= c < 123 for c in cls):
                    out.append((i, cls.decode(), 0))
                    i += 5 + ln
                    continue
        i += 1
    return out


def knobs(d):
    """Every knob, in module order, as (name, min, max)."""
    recs = records(d)
    ends = [(o, c, (recs[k+1][0] if k+1 < len(recs) else len(d)))
            for k, (o, c, _z) in enumerate(recs)]
    out = []
    for o, cls, nxt in ends:
        if cls != 'KSModul' or nxt - o < 4 * 24:
            continue
        body = o + 5 + len(cls)
        v = struct.unpack_from('<24i', d, body)
        f = struct.unpack_from('<24f', d, body)
        if v[2] == 20 and v[18] == 127 and f[20] == f[20] and \
           f[20] != f[21] and abs(f[20]) < 1e5:
            out.append((v[3], f[20], f[21]))
    out.sort()
    return [(u, lo, hi) for u, lo, hi in out]


def isval(d, o):
    f = struct.unpack_from('<f', d, o)[0]
    return f == f and (f == 0.0 or 1e-7 < abs(f) <= 1.0)


def value_runs(d, a, b):
    """(control id, [values]) for every record in [a, b)."""
    out, p = [], a + 24
    while p + 4 <= b:
        if not isval(d, p) or any(
                struct.unpack_from('<I', d, p - 4 * k)[0] != 1
                for k in range(1, 6)):
            p += 4
            continue
        vals = [struct.unpack_from('<f', d, p)[0]]
        q = p + 4
        while q + 8 <= b and struct.unpack_from('<I', d, q)[0] == 1 \
                and isval(d, q + 4):
            vals.append(struct.unpack_from('<f', d, q + 4)[0])
            q += 8
        # the trailer sits just past the last value and its flag
        cid = struct.unpack_from('<I', d, p + 8 * len(vals))[0]
        out.append((cid, vals))
        p = q + 4
    return out


def snapshot_names(d, start):
    out, i = [], start
    while i < len(d) - 4:
        n = struct.unpack_from('<I', d, i)[0]
        if 4 <= n <= 48 and i + 4 + n <= len(d):
            s = d[i+4:i+4+n]
            if all(32 <= c < 127 for c in s) and \
               sum(1 for c in s if 65 <= c <= 122) > n * 0.6:
                out.append((i, s.decode()))
                i += 4 + n
                continue
        i += 1
    keep = []
    for o, s in out:
        if not keep or keep[-1][1] != s:
            keep.append((o, s))
    return keep


# ---------------------------------------------------------------- writing ---

NCH, NFUNC = 8, 8
# param ids, and they must match src/scrupea.cpp
CH_PARAM = 0
FUNC_PARAM = CH_PARAM + NFUNC * NCH
MODE_PARAM, EDIT_PARAM = FUNC_PARAM + 1, FUNC_PARAM + 2
PITCH_PARAM, CUTOFF_PARAM = FUNC_PARAM + 3, FUNC_PARAM + 4
DELAY_PARAM, FLOW_PARAM = FUNC_PARAM + 5, FUNC_PARAM + 6
ATT = FUNC_PARAM + 7                       # four attenuverters
LEVEL_PARAM, RAND_PARAM = FUNC_PARAM + 11, FUNC_PARAM + 12
SCOPE_X_PARAM, SCOPE_Y_PARAM = FUNC_PARAM + 13, FUNC_PARAM + 14
NPARAM = SCOPE_Y_PARAM + 1


def preset(bars, macros, level_db=0.0):
    p = [0.0] * NPARAM
    for f in range(NFUNC):
        for c in range(NCH):
            p[CH_PARAM + f * NCH + c] = float(bars[f][c])
    p[PITCH_PARAM], p[CUTOFF_PARAM] = macros[0], macros[1]
    p[DELAY_PARAM], p[FLOW_PARAM] = macros[2], macros[3]
    p[LEVEL_PARAM] = level_db
    p[SCOPE_X_PARAM] = p[SCOPE_Y_PARAM] = 1.0
    return {"plugin": "forsitan", "model": "scrupea",
            "params": [{"id": i, "value": v} for i, v in enumerate(p)]}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    d = open(sys.argv[1], 'rb').read()
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    recs = records(d)
    tail = recs[-1][0] if recs else 0
    kb = knobs(d)
    names = [(o, s) for o, s in snapshot_names(d, tail)
             if len(s) > 5 and 'KSModul' not in s and 'KInPort' not in s
             and 'connected' not in s and not s.startswith('/')]

    written = 0
    for k in range(1, len(names)):
        a, b = names[k-1][0], names[k][0]
        if not (3000 < b - a < 9000):
            continue
        runs = value_runs(d, a, b)
        banks = [v for cid, v in sorted(runs) if len(v) == 8]
        singles = [(cid, v[0]) for cid, v in sorted(runs)
                   if len(v) == 1 and cid > 2100]
        if len(banks) != NFUNC or len(singles) != len(kb):
            continue

        # The four master knobs are the first four in module order, and their
        # own range is 0..1, so the normalised value is the value.
        macros = [singles[i][1] for i in range(4)]
        name = names[k][1]
        safe = "".join(ch if ch.isalnum() or ch in " -_" else "_"
                       for ch in name).strip()
        with open(os.path.join(outdir, safe + ".vcvm"), "w") as fh:
            json.dump(preset(banks, macros), fh, indent=2)
        written += 1

    print(f"{written} presets written to {outdir}")
    print("NOTE: the bar banks are written in control-id order, which is "
          "assumed to be the polycontrol's function order (F fm A am cut lbh "
          "DEL FB). Check one by ear before trusting the set.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
