#!/usr/bin/env python3
"""
vcv.py -- build a Rack patch in Python, for audition benches.

    sine = vcv.module("VCO", freq=vcv.hz(220))
    fx   = vcv.module("artifex", fxmode="replayer", time=0.8333, amt="100%")
    out  = vcv.module("Audio 2")

    sine["sine"] >> fx["left"]
    fx["left"] >> out[0]
    fx["right"] >> out[1]

Nothing here is addressed by index if it can be helped. Ports and knobs are
named, and the names are not invented: forsitan's come from src/<slug>.cpp via
modspec, everyone else's from portmap.json, which gen_portmap.py records by
asking a running Rack through limen. Guessing an index is how you patch the
wrong jack and then audition something other than what the item says -- the
Fundamental VCO's frequency knob is param 2, and the VCA's audio input is
port 2, neither of which is the number you would assume.

`a >> b` reads the left side as an output and the right as an input, and
returns the left, so chaining fans one output out to several inputs:

    sine["sine"] >> fx["left"] >> fx["right"]     # one source, two inputs

It never means a chain through the middle module. Write that as two lines.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modspec                                             # noqa: E402
import vcvpatch                                            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# Rack shows Core and Fundamental under one brand, "VCV", so an audition that
# says "VCO" means Fundamental/VCO. That resolution is done by looking up the
# model slugs already in portmap.json, not from a list kept here: a table of
# other people's modules in this file would be a second thing to maintain and
# a first thing to go stale. Only names that are *not* the slug need saying,
# and there is one -- the module Rack's browser calls "Audio 2".
DISPLAY_NAMES = {"audio_2": "Core/AudioInterface2"}

def resolve(spec):
    """A module name as an audition writes it -> "plugin/Model".

    "forsitan/artifex" and "Fundamental/VCO" are taken as given. A bare name
    is matched against this repo's own src/ first, then against the model
    slugs in portmap.json, so "VCO" finds Fundamental/VCO without anyone
    keeping a list of Fundamental's modules here. Ambiguity raises.
    """
    spec = str(spec).strip()
    if '/' in spec:
        return spec
    key = norm(spec)
    if key in DISPLAY_NAMES:
        return DISPLAY_NAMES[key]
    if os.path.exists(os.path.join(REPO, 'src', '%s.cpp' % spec)):
        return 'forsitan/' + spec
    hits = [k for k in portmap() if norm(k.split('/', 1)[1]) == key]
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        raise KeyError("%r is ambiguous: %s -- name the plugin too"
                       % (spec, ', '.join(sorted(hits))))
    raise KeyError("no module %r: not src/%s.cpp, and not in portmap.json "
                   "(record it with gen_portmap.py --add <plugin>/<model>)"
                   % (spec, spec))


_portmap = None


def portmap():
    global _portmap
    if _portmap is None:
        path = os.path.join(HERE, "portmap.json")
        if not os.path.exists(path):
            raise RuntimeError("no portmap.json -- run tools/audition/gen_portmap.py")
        with open(path) as f:
            _portmap = json.load(f)
    return _portmap


def norm(s):
    return re.sub(r'[^a-z0-9]+', '_', str(s).strip().lower()).strip('_')


def pick(table, key, what, where):
    """Resolve a name against a list of names: exact, then unique prefix, then
    unique substring. Ambiguity and absence both raise, with the candidates,
    because the alternative is silently addressing the wrong control."""
    k = norm(key)
    exact = [i for i, n in table if n == k]
    if len(exact) == 1:
        return exact[0]
    by_index = dict(table)
    for match in (lambda n: n.startswith(k), lambda n: k in n):
        hits = [i for i, n in table if n and match(n)]
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            # The shortest match wins, so `freq` finds "frequency" rather than
            # tripping over "frequency modulation". Only when two names are
            # the same length is it genuinely ambiguous.
            hits.sort(key=lambda i: len(by_index[i]))
            if len(by_index[hits[0]]) < len(by_index[hits[1]]):
                return hits[0]
            names = ', '.join(by_index[i] for i in hits)
            raise KeyError("%s: %r matches several %s (%s)" % (where, key, what, names))
    names = ', '.join(n for _, n in table if n) or '(none named)'
    raise KeyError("%s has no %s %r (has: %s)" % (where, what, key, names))


# ---------------------------------------------------------------- the surface

class Knob:
    def __init__(self, index, name, lo, hi, default, choices=None):
        self.index, self.name = index, name
        self.lo, self.hi, self.default, self.choices = lo, hi, default, choices or []

    def value(self, token):
        if isinstance(token, bool):
            return float(token)
        if isinstance(token, (int, float)):
            v = float(token)
        else:
            t = str(token).strip()
            for i, c in enumerate(self.choices):
                if norm(c) == norm(t):
                    return float(self.lo + i)
            if t.endswith('%'):
                return self.lo + float(t[:-1]) / 100.0 * (self.hi - self.lo)
            try:
                v = float(t)
            except ValueError:
                raise KeyError("%s has no setting %r (choices: %s)"
                               % (self.name, token,
                                  ', '.join(self.choices) or 'a number'))
        lo, hi = min(self.lo, self.hi), max(self.lo, self.hi)
        if not (lo - 1e-6 <= v <= hi + 1e-6):
            raise ValueError("%s = %g is outside %g..%g" % (self.name, v, lo, hi))
        return v


class Surface:
    """What a module offers: named inputs, outputs and knobs."""

    def __init__(self, spec):
        self.spec = spec
        self.inputs, self.outputs, self.knobs = [], [], []

    @staticmethod
    def of(spec):
        s = Surface(spec)
        plug, model = spec.split('/', 1)
        if plug == 'forsitan':
            m = modspec.load(model, REPO)
            s.inputs = sorted((i, n) for n, i in m.inputs.items())
            s.outputs = sorted((i, n) for n, i in m.outputs.items())
            s.knobs = [Knob(c.index, n, c.lo, c.hi, c.default, c.choices)
                       for n, c in sorted(m.params.items(), key=lambda kv: kv[1].index)]
        else:
            d = portmap().get(spec)
            if d is None:
                raise KeyError("%s is not in portmap.json -- record it with "
                               "gen_portmap.py --add %s" % (spec, spec))
            s.inputs = [(i, norm(n)) for i, n in enumerate(d['inputs'])]
            s.outputs = [(i, norm(n)) for i, n in enumerate(d['outputs'])]
            s.knobs = [Knob(i, norm(p['name']) or 'param_%d' % i,
                            p['min'], p['max'], p['default'])
                       for i, p in enumerate(d['params'])]
        return s

    def knob(self, key, where):
        if isinstance(key, int):
            return self.knobs[key]
        return self.knobs[pick([(k.index, k.name) for k in self.knobs],
                               key, 'knob', where)]


def _ports(o):
    """Whatever was written on either side of >>, as a flat list of jacks."""
    if isinstance(o, PortRef):
        return [o]
    if isinstance(o, PortGroup):
        return list(o.refs)
    if isinstance(o, (list, tuple)):
        out = []
        for x in o:
            out.extend(_ports(x))
        return out
    raise TypeError(">> needs a port on both sides, got %r" % (o,))


def _wire(src, dst):
    """One jack to many, or N to N. Anything else is a mistake worth naming:
    three outputs into two inputs has no reading that is not a guess."""
    a, b = _ports(src), _ports(dst)
    if len(a) == 1:
        a = a * len(b)
    if len(a) != len(b):
        raise ValueError("cannot wire %d output%s to %d input%s"
                         % (len(a), '' if len(a) == 1 else 's',
                            len(b), '' if len(b) == 1 else 's'))
    for s, d in zip(a, b):
        s.module.patch.connect(s, d)
    return src


def _no_and(self, other):
    raise TypeError("`&` binds looser than `>>`, so `a >> b & c` means "
                    "`(a >> b) & c`. Write `a >> (b, c)`, or chain: "
                    "`a >> b >> c`.")


class PortRef:
    """A jack, not yet known to be an input or an output -- which side of the
    >> it lands on decides that."""

    def __init__(self, module, key):
        self.module, self.key = module, key

    def resolve(self, side):
        table = self.module.surface.inputs if side == 'in' else self.module.surface.outputs
        if isinstance(self.key, int):
            if not 0 <= self.key < len(table):
                raise IndexError("%s has no %s %d" % (self.module.spec, side, self.key))
            return self.key
        return pick(table, self.key, side, self.module.spec)

    def __rshift__(self, other):
        return _wire(self, other)

    __and__ = _no_and


class PortGroup:
    """Several jacks of one module, from m["left", "right"]."""

    def __init__(self, refs):
        self.refs = list(refs)

    def __len__(self):
        return len(self.refs)

    def __iter__(self):
        return iter(self.refs)

    def __rshift__(self, other):
        return _wire(self, other)

    __and__ = _no_and


class Module:
    def __init__(self, patch, spec, mid):
        self.patch, self.spec, self.id = patch, spec, mid
        self.surface = Surface.of(spec)
        self.params = {k.index: k.default for k in self.surface.knobs}
        self.data = None

    def __getitem__(self, key):
        # m["left", "right"] hands the tuple straight through, so a stereo
        # pair is one line and still names both jacks.
        if isinstance(key, tuple):
            return PortGroup(PortRef(self, k) for k in key)
        return PortRef(self, key)

    def set(self, **kw):
        for name, value in kw.items():
            k = self.surface.knob(name, self.spec)
            self.params[k.index] = k.value(value)
        return self

    def menu(self, **kw):
        """Context-menu state, i.e. the module's own dataToJson keys."""
        if self.data is None:
            self.data = {}
        self.data.update(kw)
        return self


class Patch:
    def __init__(self):
        self.p = vcvpatch.Patch()
        self.modules = []
        self.x = 0

    def module(self, spec, hp=None, **params):
        spec = resolve(spec)
        m = Module(self, spec, None)
        if spec == 'Core/AudioInterface2' and _config.get('audio'):
            a = _config['audio']
            m.data = {'audio': {'driver': a['driver'], 'deviceName': a['deviceName'],
                                'sampleRate': float(a.get('sampleRate', 48000)),
                                'blockSize': a.get('blockSize', 256),
                                'inputOffset': 0, 'outputOffset': 0},
                      'dcFilter': True}
        if params:
            m.set(**params)
        m.hp = hp if hp is not None else self._hp(spec)
        self.modules.append(m)
        return m

    def _hp(self, spec):
        """Panel width, so the bench lays out without Rack shoving modules
        aside on load. forsitan's comes from its own panel SVG, everyone
        else's from the portmap, where limen reported it."""
        plug, model = spec.split('/', 1)
        if plug == 'forsitan':
            svg = os.path.join(REPO, 'res', '%s.svg' % model)
            try:
                with open(svg) as f:
                    mm = re.search(r'\bwidth="([\d.]+)', f.read(4000))
                if mm:
                    return max(2, int(round(float(mm.group(1)) / 5.08)))
            except OSError:
                pass
        return (portmap().get(spec) or {}).get('hp') or 10

    def connect(self, src, dst):
        # An input takes one cable. Patching a second into it replaces the
        # first, exactly as dragging a cable into an occupied jack does in
        # Rack, so an item that wants drums where the section put a sine just
        # says so instead of having to unpatch first.
        key = (id(dst.module), dst.resolve('in'))
        self._cables = [(s, d) for s, d in self._cables
                        if (id(d.module), d.resolve('in')) != key]
        self._cables.append((src, dst))

    _cables = None

    def build(self):
        # A module left with nothing patched to it is one the audition stopped
        # using -- the sine a later item replaced with drums. Leaving it in the
        # rack is just something else for the eye to land on. Notes is the
        # exception: it is there to be read, not patched.
        live = set()
        for s, d in (self._cables or []):
            live.add(id(s.module))
            live.add(id(d.module))
        self.modules = [m for m in self.modules
                        if id(m) in live or m.spec == 'Core/Notes']

        self.p = vcvpatch.Patch()
        x = 0
        for m in self.modules:
            m.id = self.p.add(m.spec.split('/')[0], m.spec.split('/', 1)[1],
                              (x, 0), params=m.params, data=m.data)
            x += m.hp
        for src, dst in (self._cables or []):
            self.p.cable(src.module.id, src.resolve('out'),
                         dst.module.id, dst.resolve('in'))
        return self.p


# ---------------------------------------------------------------- the session

_current = None
_config = {}


def configure(cfg):
    """Hand the session the machine-local settings, so an audition can say
    `vcv.module("Audio 2")` and `vcv.source("drums")` without ever naming a
    sound card or a path."""
    global _config
    _config = cfg or {}


def source(name, **params):
    """A source declared in config.json, by the name the audition calls it.

    The audition says "drums"; the config says which module plays it and which
    file it plays. That indirection is the only reason an audition can ask for
    real material at all: a plugin someone happens to have installed and a path
    into their sample library are exactly what must not be committed.
    """
    src = (_config.get('sources') or {}).get(name)
    if src is None:
        raise KeyError("no source %r in config.json (has: %s)"
                       % (name, ', '.join(sorted(_config.get('sources') or {})) or 'none'))
    m = current().module('%s/%s' % (src['plugin'], src['model']))
    if 'data' in src:
        m.data = src['data']
    if src.get('params'):
        m.params.update({int(k): float(v) for k, v in src['params'].items()})
    if params:
        m.set(**params)
    return m


def patch():
    global _current
    _current = Patch()
    _current._cables = []
    return _current


def current():
    if _current is None:
        patch()
    return _current


def module(spec, hp=None, **params):
    return current().module(spec, hp=hp, **params)


def hz(f, model='Fundamental/VCO'):
    """A frequency knob's value for a given pitch. The VCO's is in semitones
    from C4, the LFO's in octaves from 2 Hz -- neither is a number to write by
    hand into an audition."""
    import math
    if 'LFO' in model:
        return math.log2(float(f))
    return 12.0 * math.log2(float(f) / 261.6255653005986)


def modulate(target, depth=0.0, rate=0.5, shape='sine'):
    """An LFO into a VCA into `target`, with the VCA shut.

    The audition then says "open the VCA": a slow sweep by hand is not
    repeatable and not describable, and this makes the depth a knob someone
    can turn while listening, at a rate that is written down.
    """
    p = current()
    lfo = p.module('LFO')
    lfo.set(frequency=hz(rate, 'LFO'))
    vca = p.module('VCA')
    vca.set(channel_1_level=depth)
    lfo[shape] >> vca['channel_1']
    vca['channel_1'] >> target
    return vca
