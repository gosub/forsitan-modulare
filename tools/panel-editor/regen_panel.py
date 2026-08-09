#!/usr/bin/env python3
"""Regenerate res/<module>.svg from the @layout block of one or more .cpp files.

The browser editor (panel-editor.py) does this on Save; this is the same code
path without the browser, for when the layout was written by hand.

    python3 tools/panel-editor/regen_panel.py src/raucus.cpp [...]
"""

import os
import sys
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'panel_editor', os.path.join(HERE, 'panel-editor.py'))
pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe)


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    rc = 0
    for cpp in argv:
        cpp = os.path.abspath(cpp)
        layout = pe.parse_cpp(cpp)
        if layout is None:
            print(f'{cpp}: no @layout block')
            rc = 1
            continue
        base = os.path.splitext(os.path.basename(cpp))[0]
        repo = os.path.dirname(os.path.dirname(cpp))
        svg = os.path.join(repo, 'res', base + '.svg')
        if pe.regen_svg(layout, svg):
            print(f'wrote {os.path.relpath(svg, repo)}')
        else:
            print(f'{svg}: SVG generation failed (fonttools missing?)')
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
