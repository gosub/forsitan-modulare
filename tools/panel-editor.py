#!/usr/bin/env python3
"""
Panel layout editor for forsitan-modulare VCV Rack modules.
Usage: python3 tools/panel-editor.py src/MMCCCXCIX.cpp

Opens a browser editor. Drag elements until happy, then Save.
The server rewrites the @layout section in the .cpp and regenerates the SVG.

Element kinds:
  param / input / output / light  — VCV Rack widget → C++ addParam/addInput/etc.
  screw                           — VCV Rack widget positioned by top-left; editor stores visual center
  label                           — SVG-only text; position in @elem line as trailing x y
  box                             — SVG-only grey panel box; position in @elem line as trailing x y
  logo                            — SVG-only forsitan logo; position in @elem line as trailing x y
"""

import sys, os, re, json, webbrowser, threading, traceback
from http.server import HTTPServer, BaseHTTPRequestHandler

# ── visual properties for each VCV Rack widget type (radius in mm) ────────────
# vec_off: offset from stored visual-center to the createWidget Vec arg (screws only)
WIDGET_VISUALS = {
    'RoundHugeBlackKnob': {'r': 9.0,  'fill': '#2e2e2e', 'stroke': '#777', 'sw': 0.8},
    'RoundBigBlackKnob':  {'r': 6.0,  'fill': '#2e2e2e', 'stroke': '#777', 'sw': 0.6},
    'RoundBlackKnob':     {'r': 4.5,  'fill': '#2e2e2e', 'stroke': '#777', 'sw': 0.5},
    'Rogan1PWhite':       {'r': 4.5,  'fill': '#eeeeee', 'stroke': '#aaa', 'sw': 0.5},
    'Rogan2PWhite':       {'r': 5.5,  'fill': '#eeeeee', 'stroke': '#aaa', 'sw': 0.5},
    'Trimpot':            {'r': 2.5,  'fill': '#363636', 'stroke': '#888', 'sw': 0.4},
    'PJ301MPort':         {'r': 4.18, 'fill': '#999',    'stroke': '#555', 'sw': 0.5},
    'PJ3410Port':         {'r': 4.18, 'fill': '#999',    'stroke': '#555', 'sw': 0.5},
    'TL1105':             {'r': 2.0,  'fill': '#555',    'stroke': '#999', 'sw': 0.4},
    'SmallLight':         {'r': 1.5,  'fill': '#00cc44', 'stroke': 'none', 'sw': 0},
    'MediumLight':        {'r': 2.0,  'fill': '#00cc44', 'stroke': 'none', 'sw': 0},
    # ScrewSilver/Black: vec_off = half the 15px widget size at 2.953 px/mm ≈ 2.54mm
    'ScrewSilver':        {'r': 3.5,  'vec_off': 2.54, 'fill': '#c0c0c0', 'stroke': '#888', 'sw': 0.4},
    'ScrewBlack':         {'r': 3.5,  'vec_off': 2.54, 'fill': '#333',    'stroke': '#555', 'sw': 0.4},
    # SVG-only pseudo-types
    'label':              {'r': 1.0,  'fill': '#555',    'stroke': '#888', 'sw': 0.3},
    'panel_box':          {'r': 7.0,  'fill': '#e4e4e4', 'stroke': '#aaa', 'sw': 0.4},
    'forsitan_logo':      {'r': 2.75, 'fill': '#ffd500', 'stroke': '#aa8800', 'sw': 0.3},
}

KIND_FILL = {
    'input':  '#4d7fa8',
    'output': '#a8924d',
    'light':  '#00cc44',
}

# SVG-only kinds: position stored in @elem line, no C++ output
SVG_ONLY = ('label', 'logo', 'box')

# ── parse ─────────────────────────────────────────────────────────────────────
LAYOUT_HEAD_RE = re.compile(
    r'//\s*@layout:begin\s+(\w+)\s+([\d.]+)\s+([\d.]+)')
# @elem ID TYPE RADIUS KIND "LABEL" LDY [X Y]  — X Y optional for SVG-only kinds
ELEM_RE = re.compile(
    r'//\s*@elem\s+(\S+)\s+(\S+)\s+([\d.]+)\s+(\w+)\s+"([^"]*)"\s*([-\d.]+)'
    r'(?:\s+([\d.]+)\s+([\d.]+))?')
VEC_RE      = re.compile(r'mm2px\(Vec\(([\d.]+)f?,\s*([\d.]+)f?\)')
ID_RE       = re.compile(r'(\w+)::(\w+)\)')
SCREW_ID_RE = re.compile(r'createWidget.*mm2px.*Vec.*//\s*(\w+)')


def parse_cpp(path):
    with open(path) as f:
        text = f.read()

    mh = LAYOUT_HEAD_RE.search(text)
    if not mh:
        return None

    module_name = mh.group(1)
    panel_w     = float(mh.group(2))
    panel_h     = float(mh.group(3))

    block_m = re.search(
        r'(//\s*@layout:begin.*?//\s*@layout:end)', text, re.DOTALL)
    if not block_m:
        return None
    block = block_m.group(1)

    elem_defs = {}
    elem_order = []

    for m in ELEM_RE.finditer(block):
        g = m.groups()
        eid, ctype, radius, kind, label, ldy = g[:6]
        ox, oy = g[6], g[7]
        elem_defs[eid] = {
            'id': eid, 'cpp_type': ctype,
            'radius': float(radius), 'kind': kind,
            'label': label, 'label_dy': float(ldy),
            # SVG-only elements carry position in the @elem line itself
            'x': float(ox) if ox else 0.0,
            'y': float(oy) if oy else 0.0,
        }
        elem_order.append(eid)

    # Extract positions from C++ lines (skip SVG-only kinds)
    for line in block.splitlines():
        mv = VEC_RE.search(line)
        if not mv:
            continue
        mi = ID_RE.search(line)
        ms = SCREW_ID_RE.search(line)
        eid = None
        if mi:
            eid = mi.group(2)
        elif ms:
            eid = ms.group(1)
        if eid and eid in elem_defs:
            el = elem_defs[eid]
            if el['kind'] in SVG_ONLY:
                continue
            v   = WIDGET_VISUALS.get(el['cpp_type'], {})
            off = v.get('vec_off', 0.0)
            el['x'] = float(mv.group(1)) + off
            el['y'] = float(mv.group(2)) + off

    elements = [elem_defs[k] for k in elem_order if k in elem_defs]
    return {
        'module':   module_name,
        'panel_w':  panel_w,
        'panel_h':  panel_h,
        'elements': elements,
        'widget_visuals': WIDGET_VISUALS,
        'kind_fill': KIND_FILL,
    }


# ── generate & write ──────────────────────────────────────────────────────────
def cpp_line(el, module):
    x, y  = el['x'], el['y']
    eid   = el['id']
    ctype = el['cpp_type']
    kind  = el['kind']
    if kind in SVG_ONLY:
        return ''
    v   = WIDGET_VISUALS.get(ctype, {})
    off = v.get('vec_off', 0.0)
    vx, vy = x - off, y - off
    vec = f'mm2px(Vec({vx:.2f}f, {vy:.2f}f))'
    if kind == 'screw':
        return f'        addChild(createWidget<{ctype}>({vec})); // {eid}'
    ref = f'module, {module}::{eid}'
    if kind == 'param':
        return f'        addParam(createParamCentered<{ctype}>({vec}, {ref}));'
    if kind == 'input':
        return f'        addInput(createInputCentered<{ctype}>({vec}, {ref}));'
    if kind == 'output':
        return f'        addOutput(createOutputCentered<{ctype}>({vec}, {ref}));'
    if kind == 'light':
        return f'        addChild(createLightCentered<{ctype}<GreenLight>>({vec}, {ref}));'
    return f'        // @unknown kind={kind} id={eid}'


def generate_block(layout):
    m = layout['module']
    w = layout['panel_w']
    h = layout['panel_h']
    lines = [f'// @layout:begin {m} {w} {h}']
    for e in layout['elements']:
        if e['kind'] in SVG_ONLY:
            lines.append(
                f'// @elem {e["id"]} {e["cpp_type"]} {e["radius"]} '
                f'{e["kind"]} "{e["label"]}" {e["label_dy"]} {e["x"]:.2f} {e["y"]:.2f}')
        else:
            lines.append(
                f'// @elem {e["id"]} {e["cpp_type"]} {e["radius"]} '
                f'{e["kind"]} "{e["label"]}" {e["label_dy"]}')
    lines.append('')
    for e in layout['elements']:
        cl = cpp_line(e, m)
        if cl:
            lines.append(cl)
    lines.append('        // @layout:end')
    return '\n'.join(lines)


def write_cpp(path, layout):
    with open(path) as f:
        text = f.read()
    new_block = generate_block(layout)
    new_text = re.sub(
        r'//\s*@layout:begin.*?//\s*@layout:end',
        new_block, text, flags=re.DOTALL)
    with open(path, 'w') as f:
        f.write(new_text)


# ── SVG regeneration (uses fonttools if available) ────────────────────────────
FONT_PATH = os.path.expanduser(
    '~/dl/audio/ocr-a/OCR-A Regular/OCR-A Regular.otf')
_FONT_CANDIDATES = [
    FONT_PATH,
    '/tmp/ocr-a/OCR-A Regular/OCR-A Regular.otf',
]


def _find_font():
    for p in _FONT_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def regen_svg(layout, svg_path):
    font_path = _find_font()
    if font_path is None:
        print('  SVG: OCR-A font not found, skipping SVG regeneration')
        return False
    try:
        from fontTools.ttLib import TTFont
        from fontTools.pens.svgPathPen import SVGPathPen
        from fontTools.pens.transformPen import TransformPen
    except ImportError:
        print('  SVG: fonttools not available, skipping SVG regeneration')
        return False

    font   = TTFont(font_path)
    glyphs = font.getGlyphSet()
    cap_h  = font['OS/2'].sCapHeight

    def char_w(ch, sz):
        g = glyphs.get(ch, glyphs.get('.notdef'))
        return g.width * (sz / cap_h)

    def text_w(txt, sz):
        return sum(char_w(c, sz) for c in txt)

    def glyph_path(ch, x0, baseline_y, sz):
        scale = sz / cap_h
        pen   = SVGPathPen(glyphs)
        tpen  = TransformPen(pen, (scale, 0, 0, -scale, x0, baseline_y))
        g     = glyphs.get(ch, glyphs.get('.notdef'))
        g.draw(tpen)
        return pen.getCommands()

    def text_path(txt, cx, baseline_y, sz):
        x = cx - text_w(txt, sz) / 2
        parts, cur = [], x
        for ch in txt:
            p = glyph_path(ch, cur, baseline_y, sz)
            if p:
                parts.append(p)
            cur += char_w(ch, sz)
        return ' '.join(parts)

    W, H  = layout['panel_w'], layout['panel_h']
    mod   = layout['module']
    elems = layout['elements']

    lines = [
        '<?xml version="1.0" encoding="UTF-8" standalone="no"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">',
        f'  <rect width="{W}" height="{H}" fill="#1a1a1a"/>',
    ]

    # module name title at top
    title_sz = 2.8
    d = text_path(mod.lower(), W/2, 3.5 + title_sz, title_sz)
    if d:
        lines.append(f'  <path d="{d}" fill="#dcdcdc"/>')

    # grey panel boxes (SVG-only, drawn before labels and jacks)
    for el in elems:
        if el['kind'] == 'box':
            bw, bh = 14.0, 14.0
            bx = el['x'] - bw / 2
            by = el['y'] - bh / 2
            lines.append(
                f'  <rect x="{bx:.2f}" y="{by:.2f}" '
                f'width="{bw}" height="{bh}" rx="1.5" fill="#e4e4e4"/>')

    # detect which labels are inside a box (→ dark text)
    box_centers = [(e['x'], e['y']) for e in elems if e['kind'] == 'box']

    def in_any_box(lx, ly):
        return any(abs(lx - bx) < 7.5 and abs(ly - by) < 7.5
                   for bx, by in box_centers)

    # independent labels (SVG-only, freely positioned)
    sz = 2.2
    for el in elems:
        if el['kind'] == 'label':
            lbl = el.get('label', '')
            if not lbl:
                continue
            d = text_path(lbl, el['x'], el['y'], sz)
            if d:
                fill = '#1a1a1a' if in_any_box(el['x'], el['y']) else '#f9f9f9'
                lines.append(f'  <path d="{d}" fill="{fill}"/>')

    # forsitan logo — horizontal orientation (12.5mm wide × 5.5mm tall)
    # Shape derived from alea.svg (pre-rotation geometry, 90° CCW from alea's vertical form)
    for el in elems:
        if el['kind'] == 'logo':
            cx, cy = el['x'], el['y']
            bx, by = cx - 6.25, cy - 2.75
            lines += [
                f'  <rect x="{bx:.2f}" y="{by:.2f}" width="12.5" height="5.5" rx="1.2" '
                f'fill="none" stroke="#ffd500" stroke-width="0.5"/>',
                # vertical dividing bar in the centre
                f'  <rect x="{cx-0.125:.2f}" y="{cy-2.25:.2f}" '
                f'width="0.25" height="4.5" fill="#ffd500"/>',
                # 4 circles at horizontal-logo relative positions
                f'  <circle cx="{cx-2.063:.2f}" cy="{cy+1.361:.2f}" r="0.625" fill="#ffd500"/>',
                f'  <circle cx="{cx-4.180:.2f}" cy="{cy-1.285:.2f}" r="0.625" fill="#ffd500"/>',
                f'  <circle cx="{cx+4.066:.2f}" cy="{cy+1.361:.2f}" r="0.625" fill="#ffd500"/>',
                f'  <circle cx="{cx+1.949:.2f}" cy="{cy-1.285:.2f}" r="0.625" fill="#ffd500"/>',
            ]

    lines.append('</svg>')
    with open(svg_path, 'w') as f:
        f.write('\n'.join(lines))
    return True


# ── HTML editor (embedded) ────────────────────────────────────────────────────
HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Panel Editor</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  display: flex; height: 100vh;
  background: #181818; color: #e0e0e0;
  font-family: 'Consolas', 'Courier New', monospace;
  font-size: 12px;
  overflow: hidden;
}
#panel-wrap {
  flex: 1; display: flex; align-items: center; justify-content: center;
  overflow: auto; padding: 24px; background: #111;
}
#panel-svg { display: block; user-select: none; cursor: crosshair; }

#sidebar {
  width: 240px; min-width: 240px;
  background: #222; border-left: 1px solid #383838;
  display: flex; flex-direction: column; gap: 0;
  overflow: hidden;
}
.sb-section { border-bottom: 1px solid #333; padding: 10px 12px; }
.sb-section h3 {
  font-size: 10px; font-weight: normal;
  text-transform: uppercase; letter-spacing: 1.5px;
  color: #666; margin-bottom: 8px;
}
#elem-list-wrap { flex: 1; overflow-y: auto; }
#elem-list { padding: 6px 0; }
.ei {
  display: flex; align-items: center; gap: 7px;
  padding: 4px 12px; cursor: pointer;
  transition: background 0.1s;
}
.ei:hover { background: #2a2a2a; }
.ei.sel   { background: #1e3a1e; }
.ei-dot { width: 10px; height: 10px; border-radius: 50%; flex-shrink: 0; }
.ei-dot.sq { border-radius: 2px; }
.ei-name { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ei-xy   { color: #555; font-size: 10px; flex-shrink: 0; }

.btn {
  background: #333; border: 1px solid #484848;
  color: #ccc; padding: 5px 8px; cursor: pointer;
  font-size: 11px; font-family: inherit; border-radius: 3px; line-height: 1;
}
.btn:hover:not(:disabled) { background: #3d3d3d; border-color: #5a5a5a; }
.btn:disabled { opacity: 0.35; cursor: default; }
.btn.save {
  background: #1e4d1e; border-color: #2e6e2e; color: #8fca8f;
  padding: 7px; width: 100%; font-size: 12px;
}
.btn.save:hover { background: #255225; }

.align-grid   { display: grid; grid-template-columns: repeat(3, 1fr); gap: 4px; }
.align-grid-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 4px; margin-top: 4px; }

.xy-row { display: flex; align-items: center; gap: 6px; margin-top: 5px; }
.xy-row label { color: #666; width: 14px; }
.xy-row input {
  flex: 1; background: #1a1a1a; border: 1px solid #404040;
  color: #e0e0e0; padding: 4px 6px; font-size: 11px;
  font-family: inherit; border-radius: 2px;
}
.xy-row input:focus { outline: none; border-color: #5a8a5a; }
.xy-row span { color: #555; font-size: 10px; }

#status { font-size: 10px; color: #666; min-height: 16px; padding: 0 12px 8px; }
#cursor-pos { font-size: 10px; color: #555; }
#cursor-pos span { color: #888; }

.grp { cursor: grab; }
.grp.sel .indicator { stroke: #ffee00 !important; stroke-width: 0.6px !important; }
.grp.sel .knob-dot  { fill: #ffee00 !important; }
</style>
</head>
<body>
<div id="panel-wrap"><svg id="panel-svg"></svg></div>
<div id="sidebar">
  <div class="sb-section">
    <h3>Panel Editor</h3>
    <div id="cursor-pos">x: <span id="cx">—</span>  y: <span id="cy">—</span> mm</div>
  </div>
  <div class="sb-section">
    <h3>Selection (<span id="sel-count">0</span>)</h3>
    <div id="xy-edit" style="display:none">
      <div class="xy-row"><label>x</label>
        <input id="inp-x" type="number" step="0.1" min="0"><span>mm</span></div>
      <div class="xy-row"><label>y</label>
        <input id="inp-y" type="number" step="0.1" min="0"><span>mm</span></div>
    </div>
  </div>
  <div class="sb-section">
    <h3>Align</h3>
    <div class="align-grid">
      <button class="btn" id="al-l"  title="Align left edges">⊢ L</button>
      <button class="btn" id="al-cx" title="Align centers X"> | X</button>
      <button class="btn" id="al-r"  title="Align right edges">R ⊣</button>
      <button class="btn" id="al-t"  title="Align top edges">⊤ T</button>
      <button class="btn" id="al-cy" title="Align centers Y">— Y</button>
      <button class="btn" id="al-b"  title="Align bottom edges">B ⊥</button>
    </div>
    <div class="align-grid-2">
      <button class="btn" id="al-dx" title="Distribute evenly H">⇔ X</button>
      <button class="btn" id="al-dy" title="Distribute evenly V">⇕ Y</button>
    </div>
  </div>
  <div class="sb-section" style="flex:1;overflow:hidden;display:flex;flex-direction:column;padding:0;">
    <div style="padding:10px 12px 6px;"><h3>Elements</h3></div>
    <div id="elem-list-wrap"><div id="elem-list"></div></div>
  </div>
  <div style="padding:8px 12px;">
    <div id="status"></div>
    <button class="btn save" id="save-btn">Save to .cpp + .svg</button>
  </div>
</div>

<script>
'use strict';
let layout = null, elems = [], W = 0, H = 0;
let selected = [];
let dragState = null;
let rubberState = null;
const svg = document.getElementById('panel-svg');

async function load() {
  layout = await (await fetch('/api/layout')).json();
  elems  = layout.elements;
  W = layout.panel_w; H = layout.panel_h;
  render(); renderList(); updateAlignBtns();
  status('Loaded ' + elems.length + ' elements — ' + layout.module);
}

function vis(el) {
  const v = layout.widget_visuals[el.cpp_type] || {r:4,fill:'#888',stroke:'#555',sw:0.5};
  return {...v, fill: layout.kind_fill[el.kind] || v.fill};
}

function svgNS(tag, attrs) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [k,v] of Object.entries(attrs)) el.setAttribute(k,v);
  return el;
}
function circ(cx,cy,r,fill,stroke,sw) {
  return svgNS('circle',{cx,cy,r,fill,
    ...(stroke&&stroke!=='none'?{stroke,'stroke-width':sw}:{})});
}

// Returns true if (lx,ly) is inside any box element
function inAnyBox(lx, ly) {
  return elems.some(e => e.kind==='box' && Math.abs(lx-e.x)<7.5 && Math.abs(ly-e.y)<7.5);
}

function render() {
  const maxW = document.getElementById('panel-wrap').clientWidth  - 48;
  const maxH = document.getElementById('panel-wrap').clientHeight - 48;
  const scale = Math.min(maxW/W, maxH/H, 10);
  svg.setAttribute('width',  W*scale);
  svg.setAttribute('height', H*scale);
  svg.setAttribute('viewBox',`0 0 ${W} ${H}`);
  svg.innerHTML = '';

  svg.appendChild(svgNS('rect',{width:W,height:H,fill:'#1a1a1a'}));

  // HP grid
  for (let x=5.08;x<W;x+=5.08)
    svg.appendChild(svgNS('line',{x1:x,y1:0,x2:x,y2:H,stroke:'#2a2a2a','stroke-width':'0.15'}));
  for (let y=5.08;y<H;y+=5.08)
    svg.appendChild(svgNS('line',{x1:0,y1:y,x2:W,y2:y,stroke:'#2a2a2a','stroke-width':'0.15'}));

  // Decorative screws only when no screw elements in layout
  if (!elems.some(e=>e.kind==='screw')) {
    const HP=5.08;
    [[HP,HP],[W-HP,HP],[HP,H-HP],[W-HP,H-HP]].forEach(([sx,sy])=>{
      const g=svgNS('g',{});
      g.appendChild(circ(sx,sy,3.5,'#c0c0c0','#888',0.4));
      [[-1.5,0],[1.5,0],[0,-1.5],[0,1.5]].forEach(([dx,dy])=>{
        g.appendChild(svgNS('line',{x1:sx+dx-0.6,y1:sy+dy,x2:sx+dx+0.6,y2:sy+dy,
          stroke:'#777','stroke-width':'0.3'}));
      });
      svg.appendChild(g);
    });
  }

  // Boxes rendered first (background layer), then everything else
  elems.filter(e=>e.kind==='box').forEach(el=>renderElem(el));
  elems.filter(e=>e.kind!=='box').forEach(el=>renderElem(el));

  svg._rb = svgNS('rect',{fill:'none',stroke:'#ffee00',
    'stroke-width':'0.25','stroke-dasharray':'1 0.5',
    display:'none',x:0,y:0,width:0,height:0});
  svg.appendChild(svg._rb);

  svg.addEventListener('mousedown',svgDown);
  svg.addEventListener('mousemove',svgMove);
  svg.addEventListener('mouseup',  svgUp);
  svg.addEventListener('mouseleave',svgUp);
}

function renderElem(el) {
  const v   = vis(el);
  const sel = selected.includes(el.id);
  const g   = svgNS('g',{'class':'grp'+(sel?' sel':''),'data-id':el.id});

  // ── box ──────────────────────────────────────────────────────────────────
  if (el.kind === 'box') {
    const bw=14, bh=14;
    const rect = svgNS('rect',{
      x:el.x-bw/2, y:el.y-bh/2, width:bw, height:bh, rx:1.5,
      fill:sel?'rgba(255,238,0,0.12)':'rgba(228,228,228,0.3)',
      stroke:sel?'#ffee00':'#999',
      'stroke-width':sel?'0.4':'0.2',
      'stroke-dasharray':sel?'none':'1.5 0.8'});
    rect.setAttribute('class','indicator');
    g.appendChild(rect);
    g.addEventListener('mousedown',e=>elemDown(e,el));
    svg.appendChild(g);
    return;
  }

  // ── screw ─────────────────────────────────────────────────────────────────
  if (el.kind === 'screw') {
    const c=circ(el.x,el.y,v.r,v.fill,sel?'#ffee00':(v.stroke||'#888'),sel?0.6:(v.sw||0.4));
    c.setAttribute('class','indicator');
    g.appendChild(c);
    [[-1.5,0],[1.5,0],[0,-1.5],[0,1.5]].forEach(([dx,dy])=>{
      g.appendChild(svgNS('line',{
        x1:el.x+dx-0.6,y1:el.y+dy,x2:el.x+dx+0.6,y2:el.y+dy,
        stroke:sel?'#ffee00':'#777','stroke-width':'0.3'}));
    });
    g.addEventListener('mousedown',e=>elemDown(e,el));
    svg.appendChild(g);
    return;
  }

  // ── label ─────────────────────────────────────────────────────────────────
  if (el.kind === 'label') {
    const dark = inAnyBox(el.x, el.y);
    if (sel) {
      const tw=Math.max((el.label||el.id).length*1.3,4);
      g.appendChild(svgNS('rect',{
        x:el.x-tw,y:el.y-2.4,width:tw*2,height:2.9,
        fill:'none',stroke:'#ffee00','stroke-width':'0.25','stroke-dasharray':'0.5 0.3'}));
    }
    const t=svgNS('text',{
      x:el.x,y:el.y,fill:dark?'#222':'#e8e8e8',
      'font-size':'2.1','text-anchor':'middle','font-family':'monospace'});
    t.textContent=el.label||el.id;
    g.appendChild(t);
    // invisible hit target for easier clicking
    g.appendChild(svgNS('rect',{
      x:el.x-7,y:el.y-2.8,width:14,height:3.5,
      fill:'transparent','pointer-events':'all'}));
    g.addEventListener('mousedown',e=>elemDown(e,el));
    svg.appendChild(g);
    return;
  }

  // ── logo (horizontal orientation) ─────────────────────────────────────────
  if (el.kind === 'logo') {
    const [cx,cy]=[el.x,el.y];
    const border=svgNS('rect',{x:cx-6.25,y:cy-2.75,width:12.5,height:5.5,rx:1.2,
      fill:'none',stroke:sel?'#ffee00':'#ffd500','stroke-width':'0.5'});
    border.setAttribute('class','indicator');
    g.appendChild(border);
    // vertical centre bar
    g.appendChild(svgNS('rect',{x:cx-0.125,y:cy-2.25,width:0.25,height:4.5,fill:'#ffd500'}));
    // 4 circles at horizontal-logo positions
    [[-2.063,1.361],[-4.180,-1.285],[4.066,1.361],[1.949,-1.285]].forEach(([dx,dy])=>{
      g.appendChild(circ(cx+dx,cy+dy,0.625,'#ffd500','none',0));
    });
    g.addEventListener('mousedown',e=>elemDown(e,el));
    svg.appendChild(g);
    return;
  }

  // ── standard VCV widget ───────────────────────────────────────────────────
  const c=circ(el.x,el.y,v.r,v.fill,sel?'#ffee00':(v.stroke||'#666'),sel?0.6:(v.sw||0.5));
  c.setAttribute('class','indicator');
  g.appendChild(c);

  if (el.kind==='param' && v.r>=3) {
    const angle=-Math.PI/4;
    const len=v.r*0.6;
    const l=svgNS('line',{x1:el.x,y1:el.y,
      x2:el.x+Math.sin(angle)*len,y2:el.y-Math.cos(angle)*len,
      stroke:sel?'#ffee00':'#aaa','stroke-width':'0.35','stroke-linecap':'round'});
    l.setAttribute('class','knob-dot');
    g.appendChild(l);
    g.appendChild(circ(el.x,el.y,v.r*0.18,'#555','none',0));
  }
  if (el.cpp_type.includes('Port')) {
    g.appendChild(circ(el.x,el.y,v.r*0.40,'#333','none',0));
    g.appendChild(circ(el.x,el.y,v.r*0.22,'#111','none',0));
  }
  if (el.kind==='light') {
    const glow=circ(el.x,el.y,v.r*0.55,'#aaffaa','none',0);
    glow.setAttribute('opacity','0.5');
    g.appendChild(glow);
  }
  if (el.cpp_type==='TL1105')
    g.appendChild(circ(el.x,el.y,v.r*0.6,'#777','none',0));

  g.addEventListener('mousedown',e=>elemDown(e,el));
  svg.appendChild(g);
}

function svgPt(e) {
  const pt=svg.createSVGPoint();
  pt.x=e.clientX; pt.y=e.clientY;
  return pt.matrixTransform(svg.getScreenCTM().inverse());
}
function clamp(v,lo,hi){return Math.max(lo,Math.min(hi,v));}

function svgDown(e) {
  if (e.target.closest('.grp')) return;
  const p=svgPt(e);
  if (!e.shiftKey){selected=[];updateSelection();}
  rubberState={x0:p.x,y0:p.y,x:p.x,y:p.y};
}

function elemDown(e,el) {
  e.stopPropagation();
  const p=svgPt(e);
  if (e.shiftKey){
    const i=selected.indexOf(el.id);
    if(i>=0)selected.splice(i,1);else selected.push(el.id);
    updateSelection(); return;
  }
  if(!selected.includes(el.id)){selected=[el.id];updateSelection();}
  const startPos={};
  elems.filter(e2=>selected.includes(e2.id)).forEach(e2=>startPos[e2.id]={x:e2.x,y:e2.y});
  dragState={startPt:p,startPos,moved:false};
}

function svgMove(e) {
  const p=svgPt(e);
  document.getElementById('cx').textContent=p.x.toFixed(2);
  document.getElementById('cy').textContent=p.y.toFixed(2);

  if (dragState) {
    const dx=p.x-dragState.startPt.x, dy=p.y-dragState.startPt.y;
    if(Math.abs(dx)>0.05||Math.abs(dy)>0.05)dragState.moved=true;
    elems.forEach(el=>{
      if(!selected.includes(el.id))return;
      const sp=dragState.startPos[el.id];
      el.x=clamp(sp.x+dx,0,W); el.y=clamp(sp.y+dy,0,H);
    });
    render(); renderList(); updateCoordInputs();
  }

  if (rubberState) {
    rubberState.x=p.x; rubberState.y=p.y;
    const rb=svg._rb;
    const rx=Math.min(rubberState.x0,p.x), ry=Math.min(rubberState.y0,p.y);
    const rw=Math.abs(p.x-rubberState.x0), rh=Math.abs(p.y-rubberState.y0);
    rb.setAttribute('x',rx);rb.setAttribute('y',ry);
    rb.setAttribute('width',rw);rb.setAttribute('height',rh);
    rb.removeAttribute('display');
    if(!e.shiftKey)selected=[];
    elems.forEach(el=>{
      if(el.x>=rx&&el.x<=rx+rw&&el.y>=ry&&el.y<=ry+rh)
        if(!selected.includes(el.id))selected.push(el.id);
    });
    updateSelection();
  }
}

function svgUp(e) {
  dragState=null; rubberState=null;
  if(svg._rb)svg._rb.setAttribute('display','none');
}

document.addEventListener('keydown',e=>{
  if(e.key==='Escape'){selected=[];updateSelection();}
  if(e.key==='a'&&(e.ctrlKey||e.metaKey)){e.preventDefault();selected=elems.map(el=>el.id);updateSelection();}
  if(['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(e.key)&&selected.length){
    e.preventDefault();
    const step=e.shiftKey?1.0:0.1;
    const dx=e.key==='ArrowLeft'?-step:e.key==='ArrowRight'?step:0;
    const dy=e.key==='ArrowUp'?-step:e.key==='ArrowDown'?step:0;
    elems.forEach(el=>{if(selected.includes(el.id)){el.x+=dx;el.y+=dy;}});
    render();renderList();updateCoordInputs();
  }
});

function updateSelection(){
  document.getElementById('sel-count').textContent=selected.length;
  document.getElementById('xy-edit').style.display=selected.length===1?'block':'none';
  updateCoordInputs();updateAlignBtns();render();renderList();
}
function updateCoordInputs(){
  if(selected.length!==1)return;
  const el=elems.find(e=>e.id===selected[0]);
  if(!el)return;
  document.getElementById('inp-x').value=el.x.toFixed(2);
  document.getElementById('inp-y').value=el.y.toFixed(2);
}
document.getElementById('inp-x').addEventListener('input',()=>{
  if(selected.length!==1)return;
  const el=elems.find(e=>e.id===selected[0]);
  if(el){el.x=parseFloat(document.getElementById('inp-x').value)||el.x;render();renderList();}
});
document.getElementById('inp-y').addEventListener('input',()=>{
  if(selected.length!==1)return;
  const el=elems.find(e=>e.id===selected[0]);
  if(el){el.y=parseFloat(document.getElementById('inp-y').value)||el.y;render();renderList();}
});

function updateAlignBtns(){
  const d2=selected.length<2, d3=selected.length<3;
  ['al-l','al-cx','al-r','al-t','al-cy','al-b'].forEach(id=>document.getElementById(id).disabled=d2);
  document.getElementById('al-dx').disabled=d3;
  document.getElementById('al-dy').disabled=d3;
}

function renderList(){
  const list=document.getElementById('elem-list');
  list.innerHTML='';
  elems.forEach(el=>{
    const v=vis(el);
    const sel=selected.includes(el.id);
    const div=document.createElement('div');
    div.className='ei'+(sel?' sel':'');
    let dot,name;
    if(el.kind==='label'){
      dot=`<div class="ei-dot sq" style="background:#555;border:1px dashed #888;font-size:7px;display:flex;align-items:center;justify-content:center;color:#ccc">T</div>`;
      name=`"${el.label||el.id}"`;
    } else if(el.kind==='logo'){
      dot=`<div class="ei-dot sq" style="background:#ffd500;border:1px solid #aa8800"></div>`;
      name='logo';
    } else if(el.kind==='box'){
      dot=`<div class="ei-dot sq" style="background:#e4e4e4;border:1px solid #aaa"></div>`;
      name=el.id.toLowerCase().replace(/^box_/,'')+ ' box';
    } else if(el.kind==='screw'){
      dot=`<div class="ei-dot" style="background:${v.fill};border:1px solid ${v.stroke||'#888'}"></div>`;
      name=el.id.toLowerCase().replace(/^screw_/,'')+ ' screw';
    } else {
      dot=`<div class="ei-dot" style="background:${v.fill};border:1px solid ${v.stroke||'#555'}"></div>`;
      name=el.id;
    }
    div.innerHTML=dot+`<span class="ei-name">${name}</span><span class="ei-xy">${el.x.toFixed(1)},${el.y.toFixed(1)}</span>`;
    div.addEventListener('click',e=>{
      if(!e.shiftKey)selected=[];
      if(!selected.includes(el.id))selected.push(el.id);
      else selected.splice(selected.indexOf(el.id),1);
      updateSelection();
    });
    list.appendChild(div);
  });
}

function getSel(){return elems.filter(e=>selected.includes(e.id));}
function r(el){return(layout.widget_visuals[el.cpp_type]||{r:4}).r;}

function align(mode){
  const sel=getSel();if(sel.length<2)return;
  switch(mode){
    case 'l':{const ref=Math.min(...sel.map(e=>e.x-r(e)));sel.forEach(e=>e.x=ref+r(e));break;}
    case 'r':{const ref=Math.max(...sel.map(e=>e.x+r(e)));sel.forEach(e=>e.x=ref-r(e));break;}
    case 'cx':{const ref=sel.reduce((a,e)=>a+e.x,0)/sel.length;sel.forEach(e=>e.x=ref);break;}
    case 't':{const ref=Math.min(...sel.map(e=>e.y-r(e)));sel.forEach(e=>e.y=ref+r(e));break;}
    case 'b':{const ref=Math.max(...sel.map(e=>e.y+r(e)));sel.forEach(e=>e.y=ref-r(e));break;}
    case 'cy':{const ref=sel.reduce((a,e)=>a+e.y,0)/sel.length;sel.forEach(e=>e.y=ref);break;}
    case 'dx':{const s=[...sel].sort((a,b)=>a.x-b.x);const lo=s[0].x,hi=s[s.length-1].x;s.forEach((e,i)=>e.x=lo+(hi-lo)*i/(s.length-1));break;}
    case 'dy':{const s=[...sel].sort((a,b)=>a.y-b.y);const lo=s[0].y,hi=s[s.length-1].y;s.forEach((e,i)=>e.y=lo+(hi-lo)*i/(s.length-1));break;}
  }
  render();renderList();updateCoordInputs();
}
['l','cx','r','t','cy','b','dx','dy'].forEach(m=>{
  document.getElementById('al-'+m).addEventListener('click',()=>align(m));
});

document.getElementById('save-btn').addEventListener('click',async()=>{
  status('Saving...');
  try {
    const res=await(await fetch('/api/layout',{
      method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({elements:elems})
    })).json();
    status(res.ok?'Saved. Run make to rebuild.':'Error: '+res.error);
  } catch(ex){status('Network error: '+ex.message);}
});

function status(msg){document.getElementById('status').textContent=msg;}
window.addEventListener('resize',()=>{render();});
load();
</script>
</body>
</html>
"""

# ── HTTP server ────────────────────────────────────────────────────────────────
CPP_FILE = None
SVG_FILE = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass

    def send_json(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Cache-Control', 'no-cache, no-store')
        self.send_header('Content-Length', len(body))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/api/layout':
            data = parse_cpp(CPP_FILE)
            if data is None:
                self.send_json({'error': '@layout section not found'}, 404)
            else:
                self.send_json(data)
        else:
            body = HTML.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', len(body))
            self.end_headers()
            self.wfile.write(body)

    def do_POST(self):
        if self.path != '/api/layout':
            self.send_json({'error': 'not found'}, 404); return
        try:
            length = int(self.headers.get('Content-Length', 0))
            body   = json.loads(self.rfile.read(length))
            layout = parse_cpp(CPP_FILE)
            pos    = {e['id']: e for e in body['elements']}
            moved  = []
            for el in layout['elements']:
                if el['id'] in pos:
                    old_x, old_y = el['x'], el['y']
                    el['x'] = round(pos[el['id']]['x'], 2)
                    el['y'] = round(pos[el['id']]['y'], 2)
                    if el['kind'] in SVG_ONLY and (old_x != el['x'] or old_y != el['y']):
                        moved.append(f"{el['id']}→({el['x']:.2f},{el['y']:.2f})")
            if moved:
                print(f'  SVG-only moved: {", ".join(moved)}')
            write_cpp(CPP_FILE, layout)
            svg_ok = regen_svg(layout, SVG_FILE) if SVG_FILE else False
            msg = 'Saved .cpp' + (' + .svg' if svg_ok else ' (SVG skipped: fonttools missing)')
            print(' ', msg)
            self.send_json({'ok': True, 'msg': msg})
        except Exception:
            tb = traceback.format_exc()
            print(tb)
            self.send_json({'ok': False, 'error': tb.splitlines()[-1]})


# ── main ──────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <module.cpp>')
        sys.exit(1)

    CPP_FILE = os.path.abspath(sys.argv[1])
    if not os.path.exists(CPP_FILE):
        print(f'File not found: {CPP_FILE}')
        sys.exit(1)

    base     = os.path.splitext(os.path.basename(CPP_FILE))[0]
    repo_dir = os.path.dirname(os.path.dirname(CPP_FILE))
    SVG_FILE = os.path.join(repo_dir, 'res', base + '.svg')
    if not os.path.exists(SVG_FILE):
        print(f'  Note: SVG not found at {SVG_FILE}, will create on save.')

    data = parse_cpp(CPP_FILE)
    if data is None:
        print('ERROR: @layout section not found.')
        print('       Add // @layout:begin MODULE W H  ...  // @layout:end to the widget constructor.')
        sys.exit(1)

    port = 7890
    print(f'Panel editor: http://localhost:{port}/')
    print(f'  module : {data["module"]}  ({data["panel_w"]}×{data["panel_h"]} mm)')
    print(f'  cpp    : {CPP_FILE}')
    print(f'  svg    : {SVG_FILE}')
    print('Ctrl-C to stop.')

    threading.Timer(0.6, lambda: webbrowser.open(f'http://localhost:{port}/')).start()
    HTTPServer(('localhost', port), Handler).serve_forever()
