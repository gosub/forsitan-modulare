#!/usr/bin/env python3
"""
audition.py -- set up one audition item and open it in Rack.

    python3 tools/audition/audition.py artifex 3.8.6
    python3 tools/audition/audition.py artifex --list
    python3 tools/audition/audition.py artifex 3.8.6 --dry-run

The bench is Python, and it lives in the audition itself
(test/audition/<slug>.md) beside the words it belongs to, so the instruction
and the setup cannot drift apart. Three levels of code run in one namespace:

    the audition's own ```python block   -- the bench every item starts from
    the section's ```python block        -- what the mode or section sets up
    the item's code                      -- what this item changes

An item's code is a one-line `code span` or an indented ```python block.
See tools/audition/vcv.py for what the bench can say, and the audition
section of CLAUDE.md for the shape of the file.

Everything specific to one machine -- where Rack is, which sound card, which
sample file -- is in test/audition/config.json, which is not tracked. Copy
config.example.json and edit.
"""
import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vcv                                                 # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AUDITION_DIR = os.path.join(ROOT, 'test', 'audition')


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


# ---------------------------------------------------------------- the audition

class Item:
    def __init__(self, ident, text, section, code):
        self.ident, self.text, self.section, self.code = ident, text, section, code


def parse_audition(slug):
    """Read test/audition/<slug>.md into a list of items, each carrying the
    code that builds it: the file's bench, then the section's, then its own."""
    path = os.path.join(AUDITION_DIR, '%s.md' % slug)
    if not os.path.exists(path):
        die('no audition at %s' % os.path.relpath(path, ROOT))
    with open(path) as f:
        lines = f.read().splitlines()

    # Which sections have items decides where a code block belongs: the one in
    # "## 0. The bench" is the bench for the whole file, and a section that
    # tests something owns its own. Without this, a block sitting above a
    # section's first item reads as the file's bench and every later item
    # inherits it -- which silently patched the drum loop into all 85.
    has_items, sec = set(), ''
    for line in lines:
        h = re.match(r'^(#+)\s+(.*)', line)
        if h:
            sec = h.group(2).strip()
        elif re.match(r'^\s*-\s*\[([ x\-X])\]\s*\d', line):
            has_items.add(sec)

    items, section, base, sect_code = [], '', [], []
    i = 0
    while i < len(lines):
        line = lines[i]
        head = re.match(r'^(#+)\s+(.*)', line)
        fence = re.match(r'^```\s*python\s*$', line)
        if head:
            section, sect_code = head.group(2).strip(), []
        elif fence:
            body, i = [], i + 1
            while i < len(lines) and not re.match(r'^```\s*$', lines[i]):
                body.append(lines[i])
                i += 1
            (sect_code if section in has_items else base).append('\n'.join(body))
        else:
            m = re.match(r'^\s*-\s*\[([ x\-X])\]\s*(\d+(?:\.\d+)*)\.\s+(.*)', line)
            if m:
                ident, text, code = m.group(2), [m.group(3)], []
                j = i + 1
                while j < len(lines):
                    nxt = lines[j]
                    if not nxt.strip():
                        if j + 1 < len(lines) and not lines[j + 1].startswith('      '):
                            break
                        j += 1
                        continue
                    if not nxt.startswith('      '):
                        break
                    body = nxt.strip()
                    if re.match(r'^```\s*python\s*$', body):
                        j += 1
                        blk = []
                        while j < len(lines) and not re.match(r'^\s*```\s*$', lines[j]):
                            blk.append(lines[j][6:] if lines[j].startswith('      ') else lines[j])
                            j += 1
                        code.append('\n'.join(blk))
                    elif re.match(r'^`[^`]+`$', body):
                        code.append(body[1:-1])
                    else:
                        text.append(body)
                    j += 1
                items.append(Item(ident, ' '.join(text).strip(), section,
                                  list(base) + list(sect_code) + code))
                i = j
                continue
        i += 1
    return items


# ---------------------------------------------------------------- the bench

def build_patch(slug, item, cfg):
    vcv.configure(cfg)
    p = vcv.patch()
    ns = {'vcv': vcv, 'cfg': cfg, 'slug': slug, 'item': item.ident}
    for n, block in enumerate(item.code):
        try:
            exec(compile(block, '<%s %s block %d>' % (slug, item.ident, n + 1),
                         'exec'), ns)
        except Exception as e:
            die('%s %s, code block %d: %s: %s'
                % (slug, item.ident, n + 1, type(e).__name__, e))
    if not p.modules:
        die('%s %s built no modules -- is there a ```python bench in the audition?'
            % (slug, item.ident))
    notes = p.module('Notes')
    notes.data = {'text': note_text(slug, item)}
    return p.build()


def plain(s):
    """The Notes module renders no markdown, so emphasis markers would show as
    asterisks in the one place the text is actually read."""
    s = re.sub(r'\*\*(.+?)\*\*', r'\1', s)
    return re.sub(r'`([^`]+)`', r'\1', s)


def note_text(slug, item):
    out = ['%s  %s' % (slug, item.ident), '']
    if item.section:
        out += [plain(item.section), '']
    out += [wrap(plain(item.text))]
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
    ap.add_argument('--dry-run', action='store_true',
                    help='build the patch and print it, do not launch Rack')
    ap.add_argument('--code', action='store_true',
                    help='print the code that builds the item, do not launch Rack')
    args = ap.parse_args()

    items = parse_audition(args.module)
    if args.list or not args.item:
        section = None
        for it in items:
            if it.section != section:
                section = it.section
                print('\n%s' % section)
            print('  %-10s %s' % (it.ident, it.text[:68]))
        return

    hit = [i for i in items if i.ident == args.item]
    if not hit:
        die('no item %s in %s (try --list)' % (args.item, args.module))
    item = hit[0]

    if args.code:
        print('\n# ---- \n'.join(item.code))
        return

    cfg = load_config()
    p = build_patch(args.module, item, cfg)

    if args.dry_run:
        print(json.dumps(p.to_json(), indent=2))
        return

    scratch = cfg.get('scratch', '/tmp/forsitan-audition')
    os.makedirs(scratch, exist_ok=True)
    path = p.write(os.path.join(scratch, '%s-%s.vcv' % (args.module, item.ident)))
    print('%s %s -- %s' % (args.module, item.ident, item.text[:68]))
    print('patch: %s' % path)

    r = cfg['rack']
    env = dict(os.environ, HOME=r['home'])
    subprocess.run(list(r['command']) + [path], cwd=r['cwd'], env=env)


if __name__ == '__main__':
    main()
