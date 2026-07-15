#!/usr/bin/env python
"""Regenerate a module's panel SVG from its @layout block, headless.

Usage: regen_cli.py src/<module>.cpp res/<module>.svg
Run with the fonttools venv python (see CLAUDE.md).
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'panel_editor', os.path.join(HERE, 'panel-editor.py'))
pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pe)

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    layout = pe.parse_cpp(sys.argv[1])
    if not layout:
        print(f'no @layout block found in {sys.argv[1]}')
        sys.exit(1)
    pe.regen_svg(layout, sys.argv[2])
    print(f'regenerated {sys.argv[2]}')
