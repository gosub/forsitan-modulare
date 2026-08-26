#!/usr/bin/env python3
"""
audition.py -- set up one audition item and open it in Rack.

    python3 tools/audition/audition.py artifex 3.8.6
    python3 tools/audition/audition.py artifex --list
    python3 tools/audition/audition.py artifex 3.8.6 --dry-run

The scene lives in the audition itself (test/audition/<module>.md), beside
the words it belongs to, so the instruction and the setup cannot drift apart.
A section's ```scene block is its bench; each item's `scene:` line is a delta
on it. See the audition section of CLAUDE.md for the format.

Everything specific to one machine -- where Rack is, which sound card, which
sample file -- is in test/audition/config.json, which is not tracked.
Copy config.example.json and edit.
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modspec                                             # noqa: E402
import vcvpatch                                            # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AUDITION_DIR = os.path.join(ROOT, 'test', 'audition')
HP = 5.08


def die(msg):
    sys.stderr.write('audition: %s\n' % msg)
    sys.exit(1)


def load_config():
    path = os.path.join(AUDITION_DIR, 'config.json')
    if not os.path.exists(path):
        die('no test/audition/config.json -- copy config.example.json and edit it')
    with open(path) as f:
        cfg = json.load(f)

    def expand(o):
        if isinstance(o, str):
            return os.path.expanduser(o)
        if isinstance(o, dict):
            return {k: expand(v) for k, v in o.items()}
        if isinstance(o, list):
            return [expand(v) for v in o]
        return o
    return expand(cfg)


def module_hp(slug):
    """Panel width in HP, so the bench lays out without Rack shoving modules
    aside on load."""
    svg = os.path.join(ROOT, 'res', '%s.svg' % slug)
    try:
        with open(svg) as f:
            head = f.read(4000)
        m = re.search(r'\bwidth="([\d.]+)(mm)?"', head)
        if m:
            return max(2, int(round(float(m.group(1)) / HP)))
    except OSError:
        pass
    return 20


# ---------------------------------------------------------------- the audition

class Item:
    def __init__(self, ident, text, section, scene):
        self.ident, self.text, self.section, self.scene = ident, text, section, scene


def parse_scene(text):
    """`mode=replayer, time=1.0, source=sine 220` -> a dict. Values may hold
    spaces; keys may not."""
    out = {}
    for part in re.split(r',(?![^()]*\))', text):
        part = part.strip()
        if not part:
            continue
        if '=' not in part:
            die('scene fragment %r is not key=value' % part)
        k, v = part.split('=', 1)
        out[k.strip().lower()] = v.strip()
    return out


def parse_audition(slug):
    """Read test/audition/<slug>.md into a list of items. A section's bench is
    its ```scene block; an item's `scene:` line overrides keys in it."""
    path = os.path.join(AUDITION_DIR, '%s.md' % slug)
    if not os.path.exists(path):
        die('no audition at %s' % os.path.relpath(path, ROOT))
    with open(path) as f:
        lines = f.read().splitlines()

    items, section, base = [], '', {}
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r'^##+\s+(.*)', line)
        if m:
            section, base = m.group(1).strip(), {}
        elif re.match(r'^\s*```\s*scene\s*$', line):
            body, i = [], i + 1
            while i < len(lines) and not re.match(r'^\s*```\s*$', lines[i]):
                body.append(lines[i].strip())
                i += 1
            base = parse_scene(', '.join(b for b in body if b))
        else:
            m = re.match(r'^\s*-\s*\[([ x\-X])\]\s*(\d+(?:\.\d+)*)\.\s+(.*)', line)
            if m:
                ident, text, scene = m.group(2), [m.group(3)], dict(base)
                j = i + 1
                while j < len(lines) and (lines[j].startswith('      ') or not lines[j].strip()):
                    if not lines[j].strip():
                        if j + 1 < len(lines) and not lines[j + 1].startswith('      '):
                            break
                        text.append('')
                    else:
                        body = lines[j].strip()
                        sm = re.match(r'^`?scene:\s*(.*?)`?$', body)
                        if sm:
                            scene.update(parse_scene(sm.group(1)))
                        else:
                            text.append(body)
                    j += 1
                items.append(Item(ident, ' '.join(text).strip(), section, scene))
                i = j
                continue
        i += 1
    return items


# ---------------------------------------------------------------- the bench

def build_patch(slug, item, cfg):
    spec = modspec.load(slug, ROOT)
    scene = dict(item.scene)
    source = scene.pop('source', 'silence')
    menu = scene.pop('menu', '')
    note = scene.pop('note', '')

    params = spec.defaults()
    for k, v in scene.items():
        c = spec.param(k)
        params[c.index] = c.value(v)

    data = dict(cfg.get('menu_defaults', {}).get(slug, {}))
    for k, v in parse_scene(menu).items() if menu else []:
        data[k] = {'true': True, 'false': False}.get(v.lower(), v)
        if isinstance(data[k], str):
            try:
                data[k] = float(v) if '.' in v else int(v)
            except ValueError:
                pass

    p = vcvpatch.Patch()
    x = 0
    src_id, src_out, src_out_r = None, 0, None
    if source != 'silence':
        s = resolve_source(source, cfg)
        src_id = p.add(s['plugin'], s['model'], (x, 0),
                       params=s.get('params'), data=s.get('data'))
        src_out, src_out_r = s.get('out', 0), s.get('out_r')
        x += s.get('hp', 10)

    mut = p.add('forsitan', slug, (x, 0), params=params,
                data=(data if data else None))
    x += module_hp(slug)

    a = cfg['audio']
    audio = p.add('Core', 'AudioInterface2', (x, 0), data={
        'audio': {'driver': a['driver'], 'deviceName': a['deviceName'],
                  'sampleRate': float(a.get('sampleRate', 48000)),
                  'blockSize': a.get('blockSize', 256),
                  'inputOffset': 0, 'outputOffset': 0},
        'dcFilter': True})
    x += 8

    p.add('Core', 'Notes', (x, 0), data={'text': note_text(slug, item, note)})

    if src_id is not None:
        p.cable(src_id, src_out, mut, spec.port('in', cfg['bench'][slug]['in'][0]))
        if src_out_r is not None and len(cfg['bench'][slug]['in']) > 1:
            p.cable(src_id, src_out_r, mut, spec.port('in', cfg['bench'][slug]['in'][1]))
    for n, port in enumerate(cfg['bench'][slug]['out'][:2]):
        p.cable(mut, spec.port('out', port), audio, n)
    return p


def resolve_source(source, cfg):
    m = re.match(r'^sine\s+([\d.]+)\s*(?:hz)?$', source.strip(), re.I)
    if m:
        import math
        s = dict(cfg['sine'])
        ref = s.pop('ref', 261.6256)
        s['params'] = {s.pop('freq_param', 0): math.log2(float(m.group(1)) / ref)}
        return s
    if source in cfg.get('sources', {}):
        return cfg['sources'][source]
    die('unknown source %r (config has: %s, plus "sine <hz>" and "silence")'
        % (source, ', '.join(sorted(cfg.get('sources', {})))))


def plain(s):
    """The Notes module renders no markdown, so emphasis markers would show up
    as asterisks in the one place the text is actually read."""
    s = re.sub(r'\*\*(.+?)\*\*', r'\1', s)
    s = re.sub(r'`([^`]+)`', r'\1', s)
    return s


def note_text(slug, item, extra):
    out = ['%s  %s' % (slug, item.ident), '']
    if item.section:
        out += [plain(item.section), '']
    out += [wrap(plain(item.text))]
    if extra:
        out += ['', wrap(plain(extra))]
    return '\n'.join(out)


def wrap(s, width=34):
    words, line, out = s.split(), '', []
    for w in words:
        if line and len(line) + 1 + len(w) > width:
            out.append(line)
            line = w
        else:
            line = (line + ' ' + w).strip()
    if line:
        out.append(line)
    return '\n'.join(out)


# ---------------------------------------------------------------- entry point

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('module')
    ap.add_argument('item', nargs='?')
    ap.add_argument('--list', action='store_true', help='list the items and stop')
    ap.add_argument('--dry-run', action='store_true', help='build the patch, print it, do not launch')
    args = ap.parse_args()

    items = parse_audition(args.module)
    if args.list or not args.item:
        section = None
        for it in items:
            if it.section != section:
                section = it.section
                print('\n%s' % section)
            print('  %-10s %s' % (it.ident, it.text[:70]))
        return

    hit = [i for i in items if i.ident == args.item]
    if not hit:
        die('no item %s in %s (try --list)' % (args.item, args.module))
    item = hit[0]

    cfg = load_config()
    if args.module not in cfg.get('bench', {}):
        die('config.json has no bench entry for %s' % args.module)
    p = build_patch(args.module, item, cfg)

    if args.dry_run:
        print(json.dumps(p.to_json(), indent=2))
        return

    scratch = cfg.get('scratch', '/tmp/forsitan-audition')
    os.makedirs(scratch, exist_ok=True)
    path = p.write(os.path.join(scratch, '%s-%s.vcv' % (args.module, item.ident)))
    print('%s %s -- %s' % (args.module, item.ident, item.text[:70]))
    print('patch: %s' % path)

    r = cfg['rack']
    env = dict(os.environ, HOME=r['home'])
    subprocess.run(list(r['command']) + [path], cwd=r['cwd'], env=env)


if __name__ == '__main__':
    main()
