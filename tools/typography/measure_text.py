#!/usr/bin/env python3
"""
measure_text.py — Report advance width of text strings in a TTF font at a given cap height.

Usage
-----
  python3 measure_text.py --font /path/to/Font.ttf --cap-height 2.5 WORD [WORD ...]

Output: one line per word:  WORD: W.WW mm
"""
import argparse
from fontTools.ttLib import TTFont


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--font',       required=True, help='Path to TTF file')
    p.add_argument('--cap-height', type=float, default=2.5,
                   help='Desired cap height in mm (default: 2.5)')
    p.add_argument('words', nargs='+', help='Text strings to measure')
    args = p.parse_args()

    font  = TTFont(args.font)
    cap_h = font['OS/2'].sCapHeight or font['head'].unitsPerEm
    scale = args.cap_height / cap_h
    gs    = font.getGlyphSet()
    cmap  = font.getBestCmap()

    for word in args.words:
        w = sum(gs[cmap[ord(c)]].width * scale for c in word if ord(c) in cmap)
        print(f"{word}: {w:.3f} mm")


if __name__ == '__main__':
    main()
