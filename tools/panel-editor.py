#!/usr/bin/env python3
"""
Panel layout editor for forsitan-modulare VCV Rack modules.
Usage: python3 tools/panel-editor.py src/MMCCCXCIX.cpp

Opens a browser editor. Drag elements until happy, then Save.
The server rewrites the @layout section in the .cpp and regenerates the SVG.
"""

import sys, os, re, json, webbrowser, threading, traceback
from http.server import HTTPServer, BaseHTTPRequestHandler

# ── visual properties for each VCV Rack widget type (radius in mm) ────────────
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
    'ScrewSilver':        {'r': 3.5,  'fill': '#c0c0c0', 'stroke': '#888', 'sw': 0.4},
    'ScrewBlack':         {'r': 3.5,  'fill': '#333',    'stroke': '#555', 'sw': 0.4},
}

KIND_FILL = {          # overrides widget fill per kind
    'input':  '#4d7fa8',
    'output': '#a8924d',
    'light':  '#00cc44',
}

# ── parse ─────────────────────────────────────────────────────────────────────
LAYOUT_HEAD_RE = re.compile(
    r'//\s*@layout:begin\s+(\w+)\s+([\d.]+)\s+([\d.]+)')
ELEM_RE = re.compile(
    r'//\s*@elem\s+(\S+)\s+(\S+)\s+([\d.]+)\s+(\w+)\s+"([^"]*)"\s*([-\d.]+)')
VEC_RE  = re.compile(
    r'mm2px\(Vec\(([\d.]+)f?,\s*([\d.]+)f?\)')
ID_RE   = re.compile(r'(\w+)::(\w+)\)')

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
        eid, ctype, radius, kind, label, ldy = m.groups()
        elem_defs[eid] = {
            'id': eid, 'cpp_type': ctype,
            'radius': float(radius), 'kind': kind,
            'label': label, 'label_dy': float(ldy),
            'x': 0.0, 'y': 0.0,
        }
        elem_order.append(eid)

    # extract positions from C++ lines
    for line in block.splitlines():
        mv = VEC_RE.search(line)
        mi = ID_RE.search(line)
        if mv and mi:
            eid = mi.group(2)
            if eid in elem_defs:
                elem_defs[eid]['x'] = float(mv.group(1))
                elem_defs[eid]['y'] = float(mv.group(2))

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
    vec   = f'mm2px(Vec({x:.2f}f, {y:.2f}f))'
    ref   = f'module, {module}::{eid}'
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
        lines.append(
            f'// @elem {e["id"]} {e["cpp_type"]} {e["radius"]} '
            f'{e["kind"]} "{e["label"]}" {e["label_dy"]}')
    lines.append('')
    for e in layout['elements']:
        lines.append(cpp_line(e, m))
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
# fallback search
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
    cap_h  = font['OS/2'].sCapHeight  # 606

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
    HP    = 5.08
    mod   = layout['module']
    elems = layout['elements']

    lines = [
        '<?xml version="1.0" encoding="UTF-8" standalone="no"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">',
        f'  <rect width="{W}" height="{H}" fill="#1a1a1a"/>',
    ]

    # title
    title_sz = 2.8
    title_y  = 3.5 + title_sz
    d = text_path(mod.lower(), W/2, title_y, title_sz)
    if d:
        lines.append(f'  <path d="{d}" fill="#dcdcdc"/>')

    # element labels
    for el in elems:
        lbl = el.get('label', '')
        if not lbl:
            continue
        sz = 2.2
        baseline = el['y'] + el['label_dy']
        d = text_path(lbl, el['x'], baseline, sz)
        if not d:
            continue
        fill = '#1a1a1a' if el['kind'] == 'output' else '#f9f9f9'
        # output box
        if el['kind'] == 'output':
            bw, bh = 14.0, 14.0
            bx = el['x'] - bw/2
            by = el['y'] - el['radius'] - 1.5
            lines.append(
                f'  <rect x="{bx:.2f}" y="{by:.2f}" '
                f'width="{bw}" height="{bh}" rx="1.5" fill="#e4e4e4"/>')
        lines.append(f'  <path d="{d}" fill="{fill}"/>')

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
.sb-section {
  border-bottom: 1px solid #333;
  padding: 10px 12px;
}
.sb-section h3 {
  font-size: 10px; font-weight: normal;
  text-transform: uppercase; letter-spacing: 1.5px;
  color: #666; margin-bottom: 8px;
}
#elem-list-wrap { flex: 1; overflow-y: auto; }
#elem-list { padding: 6px 0; }
.ei {
  display: flex; align-items: center; gap: 7px;
  padding: 4px 12px; cursor: pointer; border-radius: 0;
  transition: background 0.1s;
}
.ei:hover { background: #2a2a2a; }
.ei.sel   { background: #1e3a1e; }
.ei-dot   { width: 10px; height: 10px; border-radius: 50%; flex-shrink: 0; }
.ei-name  { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ei-xy    { color: #555; font-size: 10px; flex-shrink: 0; }

.btn {
  background: #333; border: 1px solid #484848;
  color: #ccc; padding: 5px 8px; cursor: pointer;
  font-size: 11px; font-family: inherit; border-radius: 3px;
  line-height: 1;
}
.btn:hover:not(:disabled) { background: #3d3d3d; border-color: #5a5a5a; }
.btn:disabled { opacity: 0.35; cursor: default; }
.btn.save {
  background: #1e4d1e; border-color: #2e6e2e; color: #8fca8f;
  padding: 7px; width: 100%; font-size: 12px;
}
.btn.save:hover { background: #255225; }

.align-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 4px; }
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

/* SVG element styles (injected) */
.grp { cursor: grab; }
.grp.sel .indicator { stroke: #ffee00 !important; stroke-width: 0.6px !important; }
.grp.sel .knob-dot { fill: #ffee00 !important; }
</style>
</head>
<body>
<div id="panel-wrap">
  <svg id="panel-svg"></svg>
</div>
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
      <button class="btn" id="al-dx" title="Distribute evenly (horizontal)">⇔ X</button>
      <button class="btn" id="al-dy" title="Distribute evenly (vertical)">⇕ Y</button>
    </div>
  </div>

  <div class="sb-section" style="flex:1; overflow:hidden; display:flex; flex-direction:column; gap:0; padding:0;">
    <div style="padding:10px 12px 6px;" class=""><h3>Elements</h3></div>
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
let selected = [];        // ordered list of selected IDs
let dragState = null;     // {startPt, startPos:{id:{x,y}}, moved}
let rubberState = null;   // {x0,y0,x,y}
const svg = document.getElementById('panel-svg');

// ── load ─────────────────────────────────────────────────────────────────────
async function load() {
  layout = await (await fetch('/api/layout')).json();
  elems  = layout.elements;
  W = layout.panel_w; H = layout.panel_h;
  render();
  renderList();
  updateAlignBtns();
  status('Loaded ' + elems.length + ' elements — ' + layout.module);
}

// ── visual helpers ────────────────────────────────────────────────────────────
function vis(el) {
  const v = layout.widget_visuals[el.cpp_type] || {r:4,fill:'#888',stroke:'#555',sw:0.5};
  const fill = layout.kind_fill[el.kind] || v.fill;
  return {...v, fill};
}

function svgNS(tag, attrs) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const [k,v] of Object.entries(attrs)) el.setAttribute(k, v);
  return el;
}
function circ(cx,cy,r,fill,stroke,sw) {
  return svgNS('circle',{cx,cy,r,fill,
    ...(stroke&&stroke!=='none'?{stroke,'stroke-width':sw}:{})});
}

// ── render panel ──────────────────────────────────────────────────────────────
function render() {
  // scale to fit
  const maxW = document.getElementById('panel-wrap').clientWidth  - 48;
  const maxH = document.getElementById('panel-wrap').clientHeight - 48;
  const scale = Math.min(maxW/W, maxH/H, 10);
  svg.setAttribute('width',  W*scale);
  svg.setAttribute('height', H*scale);
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.innerHTML = '';

  // background
  svg.appendChild(svgNS('rect', {width:W, height:H, fill:'#1a1a1a'}));

  // grid (optional, light)
  for (let x=5.08; x<W; x+=5.08) {
    svg.appendChild(svgNS('line',{x1:x,y1:0,x2:x,y2:H,
      stroke:'#2a2a2a','stroke-width':'0.15'}));
  }
  for (let y=5.08; y<H; y+=5.08) {
    svg.appendChild(svgNS('line',{x1:0,y1:y,x2:W,y2:y,
      stroke:'#2a2a2a','stroke-width':'0.15'}));
  }

  // screws (auto at 1HP corners)
  const HP = 5.08;
  [[HP,HP],[W-HP,HP],[HP,H-HP],[W-HP,H-HP]].forEach(([sx,sy]) => {
    const g = svgNS('g',{});
    g.appendChild(circ(sx,sy,3.5,'#c0c0c0','#888',0.4));
    [[-1.5,0],[1.5,0],[0,-1.5],[0,1.5]].forEach(([dx,dy]) => {
      const l = svgNS('line',{
        x1:sx+dx-0.6,y1:sy+dy,x2:sx+dx+0.6,y2:sy+dy,
        stroke:'#777','stroke-width':'0.3'});
      g.appendChild(l);
    });
    svg.appendChild(g);
  });

  // elements
  elems.forEach(renderElem);

  // rubber band (on top)
  svg._rb = svgNS('rect',{fill:'none',stroke:'#ffee00',
    'stroke-width':'0.25','stroke-dasharray':'1 0.5',
    display:'none', x:0,y:0,width:0,height:0});
  svg.appendChild(svg._rb);

  // events
  svg.addEventListener('mousedown', svgDown);
  svg.addEventListener('mousemove', svgMove);
  svg.addEventListener('mouseup',   svgUp);
  svg.addEventListener('mouseleave',svgUp);
}

function renderElem(el) {
  const v   = vis(el);
  const sel = selected.includes(el.id);
  const g   = svgNS('g', {'class': 'grp'+(sel?' sel':''), 'data-id': el.id});

  // main circle
  const c = circ(el.x, el.y, v.r, v.fill, sel?'#ffee00':(v.stroke||'#666'), sel?0.6:(v.sw||0.5));
  c.setAttribute('class','indicator');
  g.appendChild(c);

  // knob pointer
  if (el.kind === 'param' && v.r >= 3) {
    const angle = -Math.PI/4;  // fixed decorative angle
    const len   = v.r * 0.6;
    const x2    = el.x + Math.sin(angle)*len;
    const y2    = el.y - Math.cos(angle)*len;
    const l = svgNS('line',{x1:el.x,y1:el.y,x2,y2,
      stroke:sel?'#ffee00':'#aaa','stroke-width':'0.35','stroke-linecap':'round'});
    l.setAttribute('class','knob-dot');
    g.appendChild(l);
    // rim
    const rim = circ(el.x,el.y,v.r*0.18,'#555','none',0);
    g.appendChild(rim);
  }

  // jack hole
  if (el.cpp_type.includes('Port')) {
    g.appendChild(circ(el.x,el.y,v.r*0.40,'#333','none',0));
    g.appendChild(circ(el.x,el.y,v.r*0.22,'#111','none',0));
  }

  // light glow
  if (el.kind === 'light') {
    const glow = circ(el.x,el.y,v.r*0.55,'#aaffaa','none',0);
    glow.setAttribute('opacity','0.5');
    g.appendChild(glow);
  }

  // TL1105 (button)
  if (el.cpp_type === 'TL1105') {
    const top = circ(el.x,el.y,v.r*0.6,'#777','none',0);
    g.appendChild(top);
  }

  // label
  if (el.label) {
    const t = svgNS('text',{
      x:el.x, y:el.y+el.label_dy,
      fill: el.kind==='output' ? '#1a1a1a' : '#e0e0e0',
      'font-size':'1.9','text-anchor':'middle',
      'font-family':'monospace','pointer-events':'none'});
    t.textContent = el.label;
    g.appendChild(t);
  }

  g.addEventListener('mousedown', e => elemDown(e, el));
  svg.appendChild(g);
}

// ── coordinate conversion ─────────────────────────────────────────────────────
function svgPt(e) {
  const pt = svg.createSVGPoint();
  pt.x = e.clientX; pt.y = e.clientY;
  return pt.matrixTransform(svg.getScreenCTM().inverse());
}

function clamp(v,lo,hi){ return Math.max(lo,Math.min(hi,v)); }

// ── events ────────────────────────────────────────────────────────────────────
function svgDown(e) {
  const target = e.target.closest('.grp');
  if (target) return;  // handled by elemDown
  const p = svgPt(e);
  if (!e.shiftKey) { selected = []; updateSelection(); }
  rubberState = {x0:p.x, y0:p.y, x:p.x, y:p.y};
}

function elemDown(e, el) {
  e.stopPropagation();
  const p = svgPt(e);
  if (e.shiftKey) {
    const i = selected.indexOf(el.id);
    if (i >= 0) selected.splice(i,1); else selected.push(el.id);
    updateSelection();
    return;
  }
  if (!selected.includes(el.id)) { selected = [el.id]; updateSelection(); }
  const startPos = {};
  elems.filter(e2 => selected.includes(e2.id))
       .forEach(e2 => startPos[e2.id] = {x:e2.x, y:e2.y});
  dragState = {startPt:p, startPos, moved:false};
}

function svgMove(e) {
  const p = svgPt(e);
  document.getElementById('cx').textContent = p.x.toFixed(2);
  document.getElementById('cy').textContent = p.y.toFixed(2);

  if (dragState) {
    const dx = p.x - dragState.startPt.x;
    const dy = p.y - dragState.startPt.y;
    if (Math.abs(dx)>0.05||Math.abs(dy)>0.05) dragState.moved = true;
    elems.forEach(el => {
      if (!selected.includes(el.id)) return;
      const sp = dragState.startPos[el.id];
      el.x = clamp(sp.x+dx, 0, W);
      el.y = clamp(sp.y+dy, 0, H);
    });
    render(); renderList(); updateCoordInputs();
  }

  if (rubberState) {
    rubberState.x = p.x; rubberState.y = p.y;
    const rb = svg._rb;
    const rx = Math.min(rubberState.x0, p.x);
    const ry = Math.min(rubberState.y0, p.y);
    const rw = Math.abs(p.x - rubberState.x0);
    const rh = Math.abs(p.y - rubberState.y0);
    rb.setAttribute('x',rx); rb.setAttribute('y',ry);
    rb.setAttribute('width',rw); rb.setAttribute('height',rh);
    rb.removeAttribute('display');

    const x1=rx, x2=rx+rw, y1=ry, y2=ry+rh;
    if (!e.shiftKey) selected = [];
    elems.forEach(el => {
      if (el.x>=x1&&el.x<=x2&&el.y>=y1&&el.y<=y2)
        if (!selected.includes(el.id)) selected.push(el.id);
    });
    updateSelection();
  }
}

function svgUp(e) {
  dragState  = null;
  rubberState = null;
  if (svg._rb) svg._rb.setAttribute('display','none');
}

// keyboard
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') { selected=[]; updateSelection(); }
  if (e.key === 'a' && (e.ctrlKey||e.metaKey)) {
    e.preventDefault();
    selected = elems.map(el=>el.id); updateSelection();
  }
  // nudge with arrow keys
  if (['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(e.key) && selected.length) {
    e.preventDefault();
    const step = e.shiftKey ? 1.0 : 0.1;
    const dx = e.key==='ArrowLeft'?-step : e.key==='ArrowRight'?step : 0;
    const dy = e.key==='ArrowUp'  ?-step : e.key==='ArrowDown' ?step : 0;
    elems.forEach(el => { if(selected.includes(el.id)){ el.x+=dx; el.y+=dy; } });
    render(); renderList(); updateCoordInputs();
  }
});

// ── selection UI ──────────────────────────────────────────────────────────────
function updateSelection() {
  document.getElementById('sel-count').textContent = selected.length;
  document.getElementById('xy-edit').style.display = selected.length===1 ? 'block' : 'none';
  updateCoordInputs();
  updateAlignBtns();
  render(); renderList();
}

function updateCoordInputs() {
  if (selected.length !== 1) return;
  const el = elems.find(e=>e.id===selected[0]);
  if (!el) return;
  document.getElementById('inp-x').value = el.x.toFixed(2);
  document.getElementById('inp-y').value = el.y.toFixed(2);
}

document.getElementById('inp-x').addEventListener('input', () => {
  if (selected.length!==1) return;
  const el = elems.find(e=>e.id===selected[0]);
  if (el) { el.x=parseFloat(document.getElementById('inp-x').value)||el.x; render(); renderList(); }
});
document.getElementById('inp-y').addEventListener('input', () => {
  if (selected.length!==1) return;
  const el = elems.find(e=>e.id===selected[0]);
  if (el) { el.y=parseFloat(document.getElementById('inp-y').value)||el.y; render(); renderList(); }
});

function updateAlignBtns() {
  const dis = selected.length < 2;
  ['al-l','al-cx','al-r','al-t','al-cy','al-b'].forEach(id=>{
    document.getElementById(id).disabled = dis;
  });
  const dis3 = selected.length < 3;
  document.getElementById('al-dx').disabled = dis3;
  document.getElementById('al-dy').disabled = dis3;
}

// ── element list ──────────────────────────────────────────────────────────────
function renderList() {
  const list = document.getElementById('elem-list');
  list.innerHTML = '';
  elems.forEach(el => {
    const v   = vis(el);
    const sel = selected.includes(el.id);
    const div = document.createElement('div');
    div.className = 'ei' + (sel?' sel':'');
    div.innerHTML =
      `<div class="ei-dot" style="background:${v.fill};border:1px solid ${v.stroke||'#555'}"></div>` +
      `<span class="ei-name">${el.label||el.id}</span>` +
      `<span class="ei-xy">${el.x.toFixed(1)},${el.y.toFixed(1)}</span>`;
    div.addEventListener('click', e => {
      if (!e.shiftKey) selected = [];
      if (!selected.includes(el.id)) selected.push(el.id);
      else selected.splice(selected.indexOf(el.id),1);
      updateSelection();
    });
    list.appendChild(div);
  });
}

// ── alignment ─────────────────────────────────────────────────────────────────
function getSel() { return elems.filter(e=>selected.includes(e.id)); }
function r(el)    { return (layout.widget_visuals[el.cpp_type]||{r:4}).r; }

function align(mode) {
  const sel = getSel(); if(sel.length<2) return;
  switch(mode) {
    case 'l':  { const ref=Math.min(...sel.map(e=>e.x-r(e))); sel.forEach(e=>e.x=ref+r(e)); break; }
    case 'r':  { const ref=Math.max(...sel.map(e=>e.x+r(e))); sel.forEach(e=>e.x=ref-r(e)); break; }
    case 'cx': { const ref=sel.reduce((a,e)=>a+e.x,0)/sel.length; sel.forEach(e=>e.x=ref); break; }
    case 't':  { const ref=Math.min(...sel.map(e=>e.y-r(e))); sel.forEach(e=>e.y=ref+r(e)); break; }
    case 'b':  { const ref=Math.max(...sel.map(e=>e.y+r(e))); sel.forEach(e=>e.y=ref-r(e)); break; }
    case 'cy': { const ref=sel.reduce((a,e)=>a+e.y,0)/sel.length; sel.forEach(e=>e.y=ref); break; }
    case 'dx': {
      const sorted=[...sel].sort((a,b)=>a.x-b.x);
      const lo=sorted[0].x, hi=sorted[sorted.length-1].x;
      sorted.forEach((e,i)=>e.x=lo+(hi-lo)*i/(sorted.length-1));
      break;
    }
    case 'dy': {
      const sorted=[...sel].sort((a,b)=>a.y-b.y);
      const lo=sorted[0].y, hi=sorted[sorted.length-1].y;
      sorted.forEach((e,i)=>e.y=lo+(hi-lo)*i/(sorted.length-1));
      break;
    }
  }
  render(); renderList(); updateCoordInputs();
}

['l','cx','r','t','cy','b','dx','dy'].forEach(m=>{
  document.getElementById('al-'+m).addEventListener('click',()=>align(m));
});

// ── save ──────────────────────────────────────────────────────────────────────
document.getElementById('save-btn').addEventListener('click', async () => {
  status('Saving...');
  try {
    const r = await fetch('/api/layout', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({elements: elems})
    });
    const res = await r.json();
    status(res.ok ? 'Saved. Run make to rebuild.' : 'Error: '+res.error);
  } catch(ex) { status('Network error: '+ex.message); }
});

function status(msg) { document.getElementById('status').textContent = msg; }
window.addEventListener('resize', ()=>{ render(); });
load();
</script>
</body>
</html>
"""

# ── HTTP server ────────────────────────────────────────────────────────────────
CPP_FILE = None
SVG_FILE = None

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass  # quiet

    def send_json(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
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
            for el in layout['elements']:
                if el['id'] in pos:
                    el['x'] = round(pos[el['id']]['x'], 2)
                    el['y'] = round(pos[el['id']]['y'], 2)
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

    # infer SVG path: src/FOO.cpp → res/FOO.svg
    base     = os.path.splitext(os.path.basename(CPP_FILE))[0]
    repo_dir = os.path.dirname(os.path.dirname(CPP_FILE))
    SVG_FILE = os.path.join(repo_dir, 'res', base + '.svg')
    if not os.path.exists(SVG_FILE):
        print(f'  Note: SVG not found at {SVG_FILE}, will create on save.')

    data = parse_cpp(CPP_FILE)
    if data is None:
        print('ERROR: @layout section not found in the .cpp file.')
        print('Add // @layout:begin ... // @layout:end to the widget constructor.')
        sys.exit(1)

    port   = 8765
    server = HTTPServer(('localhost', port), Handler)
    url    = f'http://localhost:{port}'
    print(f'Panel editor → {url}')
    print(f'Editing:  {CPP_FILE}')
    print(f'SVG:      {SVG_FILE}')
    print('Ctrl-C to quit.')
    threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nBye.')
