#!/usr/bin/env python3
"""
modspec.py -- read a forsitan module's control surface out of its source.

An audition scene names knobs and jacks the way the panel does ("time",
"amt", "out l"). A patch file stores them as indices. Writing indices into
an audition would rot on the first change, because new params and ports are
*appended* to a shipped module's enum rather than inserted (see the
released-module port order rule in CLAUDE.md), so every index after the
insertion point would shift while the audition kept the old number and
silently set the wrong knob.

So the indices are read from src/<module>.cpp at build time: the enums give
the order, configParam/configSwitch give the range and the switch labels.
A name that does not exist raises, which is the point -- a typo in an
audition fails loudly instead of auditioning the wrong control.
"""
import os
import re

NUM = r'(-?[\d.]+)f?'


def _f(s):
    return float(s.rstrip('f'))


def _enum(src, name, end):
    m = re.search(r'enum\s+' + name + r'\s*\{(.*?)' + end, src, re.S)
    if not m:
        return []
    out = []
    for line in m.group(1).splitlines():
        line = re.sub(r'//.*', '', line).strip().rstrip(',').strip()
        if re.fullmatch(r'[A-Z0-9_]+', line):
            out.append(line)
    return out


def _alias(name, suffix):
    """TIME_PARAM -> time, LFO_ATT_PARAM -> lfo_att, OUT_L_OUTPUT -> out_l."""
    return name[:-(len(suffix) + 1)].lower() if name.endswith('_' + suffix) else name.lower()


class Control:
    def __init__(self, index, name, lo=0.0, hi=1.0, default=0.0, label=None, choices=None):
        self.index, self.name = index, name
        self.lo, self.hi, self.default = lo, hi, default
        self.label, self.choices = label, choices or []

    def value(self, token):
        """Resolve one scene token to a raw param value. A switch or a
        snapped selector accepts its own label ('replayer', 'randomize'),
        anything accepts a number, and a percentage is written as such."""
        token = str(token).strip()
        low = token.lower()
        for i, c in enumerate(self.choices):
            if c.lower() == low:
                return float(self.lo + i)
        if low.endswith('%'):
            v = float(low[:-1]) / 100.0
            return self.lo + v * (self.hi - self.lo)
        try:
            v = float(low)
        except ValueError:
            raise KeyError("%s has no setting %r (choices: %s)"
                           % (self.name, token, ', '.join(self.choices) or 'a number'))
        if not (min(self.lo, self.hi) - 1e-6 <= v <= max(self.lo, self.hi) + 1e-6):
            raise ValueError("%s = %g is outside %g..%g" % (self.name, v, self.lo, self.hi))
        return v


class ModSpec:
    def __init__(self, slug, src):
        self.slug = slug
        self.params, self.inputs, self.outputs = {}, {}, {}

        for i, n in enumerate(_enum(src, 'ParamId', 'PARAMS_LEN')):
            self.params[_alias(n, 'PARAM')] = Control(i, n)
        for i, n in enumerate(_enum(src, 'InputId', 'INPUTS_LEN')):
            self.inputs[_alias(n, 'INPUT')] = i
        for i, n in enumerate(_enum(src, 'OutputId', 'OUTPUTS_LEN')):
            self.outputs[_alias(n, 'OUTPUT')] = i

        by_enum = {c.name: c for c in self.params.values()}

        for m in re.finditer(r'configParam\w*\s*\(\s*([A-Z0-9_]+)\s*,\s*' + NUM +
                             r'\s*,\s*' + NUM + r'\s*,\s*' + NUM +
                             r'\s*,\s*"([^"]*)"', src):
            c = by_enum.get(m.group(1))
            if c:
                c.lo, c.hi, c.default, c.label = _f(m.group(2)), _f(m.group(3)), _f(m.group(4)), m.group(5)

        for m in re.finditer(r'configSwitch\s*\(\s*([A-Z0-9_]+)\s*,\s*' + NUM +
                             r'\s*,\s*' + NUM + r'\s*,\s*' + NUM +
                             r'\s*,\s*"([^"]*)"\s*,\s*\{(.*?)\}', src, re.S):
            c = by_enum.get(m.group(1))
            if c:
                c.lo, c.hi, c.default, c.label = _f(m.group(2)), _f(m.group(3)), _f(m.group(4)), m.group(5)
                c.choices = re.findall(r'"([^"]*)"', m.group(6))

        for m in re.finditer(r'configButton\s*\(\s*([A-Z0-9_]+)\s*,\s*"([^"]*)"', src):
            c = by_enum.get(m.group(1))
            if c:
                c.lo, c.hi, c.default, c.label = 0.0, 1.0, 0.0, m.group(2)

    def defaults(self):
        return {c.index: c.default for c in self.params.values()}

    def param(self, name):
        key = name.strip().lower().replace(' ', '_').replace('-', '_')
        if key not in self.params:
            raise KeyError("%s has no knob %r (has: %s)"
                           % (self.slug, name, ', '.join(sorted(self.params))))
        return self.params[key]

    def port(self, side, name):
        table = self.inputs if side == 'in' else self.outputs
        key = name.strip().lower().replace(' ', '_').replace('-', '_')
        if key not in table:
            raise KeyError("%s has no %s %r (has: %s)"
                           % (self.slug, side, name, ', '.join(sorted(table))))
        return table[key]


def load(slug, root=None):
    root = root or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    path = os.path.join(root, 'src', '%s.cpp' % slug)
    with open(path) as f:
        return ModSpec(slug, f.read())
