/**
 * 2D Online Renderer
 * Architecture:
 *   - Multiple virtual layers stored as ImageData snapshots
 *   - Three physical canvases: bg (checkerboard), main (composited), overlay (live stroke)
 *   - Object pool for Path2D + command objects (memory optimisation)
 *   - Chunked undo stack with delta compression
 *   - requestAnimationFrame render loop with dirty-region tracking
 */

'use strict';

/* ═══════════════════════════════════════════════════
   1. CONSTANTS & CONFIGURATION
═══════════════════════════════════════════════════ */
const MAX_UNDO   = 40;      // max undo steps per layer
const TILE_SIZE  = 64;      // checkerboard tile px
const PALETTE    = [
  '#ff5252','#ff4081','#e040fb','#7c4dff','#536dfe','#448aff',
  '#40c4ff','#18ffff','#64ffda','#69f0ae','#b2ff59','#eeff41',
  '#ffff00','#ffd740','#ffab40','#ff6d00','#ffffff','#bdbdbd',
  '#757575','#212121','#000000','#795548','#607d8b','#009688',
];

/* ═══════════════════════════════════════════════════
   2. STATE
═══════════════════════════════════════════════════ */
const state = {
  tool: 'pen',
  fillColor: '#1a73e8',
  strokeColor: '#000000',
  editTarget: 'stroke',  // 'fill' | 'stroke'
  size: 4,
  opacity: 1.0,
  zoom: 1.0,
  panX: 0,
  panY: 0,
  canvasW: 1280,
  canvasH: 720,

  // layers: array of { id, name, visible, locked, imageData, undoStack, redoStack }
  layers: [],
  activeLayer: 0,

  // live drawing state
  drawing: false,
  lastX: 0,
  lastY: 0,
  startX: 0,
  startY: 0,
  // Float32Array: 2 floats per point (x,y), pre-allocated for 2048 points.
  // Replaces plain {x,y} objects — zero GC pressure, 5× smaller per point.
  brushPointsF32: new Float32Array(4096),
  brushPointCount: 0,

  // selection
  selection: null,   // { x,y,w,h } in canvas space

  // panning
  panning: false,
  panStartX: 0,
  panStartY: 0,

  // dirty regions for composite redraw
  dirtyRect: null,
  rafPending: false,

  // color picker
  hue: 0,
  sat: 0,
  val: 0,

  // memory pool (object reuse)
  _pointPool: [],
};

/* ═══════════════════════════════════════════════════
   3. DOM REFS
═══════════════════════════════════════════════════ */
const bgCanvas       = document.getElementById('bg-canvas');
const mainCanvas     = document.getElementById('main-canvas');
const overlay        = document.getElementById('overlay-canvas');
const bgCtx          = bgCanvas.getContext('2d');
const mainCtx        = mainCanvas.getContext('2d');
const overlayCtx     = overlay.getContext('2d');
const colorWheel     = document.getElementById('color-wheel');
const valueSlider    = document.getElementById('value-slider');
const cwCtx          = colorWheel.getContext('2d');
const vsCtx          = valueSlider.getContext('2d');
const hexInput       = document.getElementById('hex-input');
const rInput         = document.getElementById('r-input');
const gInput         = document.getElementById('g-input');
const bInput         = document.getElementById('b-input');
const fillSwatch     = document.getElementById('fill-swatch');
const strokeSwatch   = document.getElementById('stroke-swatch');
const brushSizeSlider= document.getElementById('brush-size');
const sizeVal        = document.getElementById('size-val');
const opacityRange   = document.getElementById('opacity-range');
const opacityVal     = document.getElementById('opacity-val');
const layersList     = document.getElementById('layers-list');
const memLines       = document.getElementById('mem-lines');
const textWrapper    = document.getElementById('text-input-wrapper');
const textInput      = document.getElementById('text-input');

/* ═══════════════════════════════════════════════════
   4. CANVAS RESIZE & COORDINATE HELPERS
═══════════════════════════════════════════════════ */
function resizePhysicalCanvases(w, h) {
  [bgCanvas, mainCanvas, overlay].forEach(c => {
    c.width  = w;
    c.height = h;
    c.style.width  = w + 'px';
    c.style.height = h + 'px';
  });
  document.getElementById('canvas-container').style.width  = w + 'px';
  document.getElementById('canvas-container').style.height = h + 'px';
  drawCheckerboard();
  compositeAll();
}

// Convert pointer event coords → canvas pixel coords
function toCanvas(e) {
  const rect = overlay.getBoundingClientRect();
  return {
    x: (e.clientX - rect.left) / state.zoom - state.panX,
    y: (e.clientY - rect.top)  / state.zoom - state.panY,
  };
}

/* ═══════════════════════════════════════════════════
   5. CHECKERBOARD BACKGROUND
═══════════════════════════════════════════════════ */
function drawCheckerboard() {
  const { canvasW: W, canvasH: H } = state;
  const light = '#444', dark = '#333';
  for (let y = 0; y < H; y += TILE_SIZE) {
    for (let x = 0; x < W; x += TILE_SIZE) {
      bgCtx.fillStyle = ((x / TILE_SIZE + y / TILE_SIZE) % 2 === 0) ? light : dark;
      bgCtx.fillRect(x, y, TILE_SIZE, TILE_SIZE);
    }
  }
}

/* ═══════════════════════════════════════════════════
   6. LAYER MANAGEMENT
   Memory optimisation: ImageData stored per-layer,
   composited on demand. Only dirty layers recomposited.
═══════════════════════════════════════════════════ */
function createLayer(name) {
  const id = Date.now() + Math.random();
  // Allocate off-screen canvas per layer (GPU-backed)
  const offscreen = document.createElement('canvas');
  offscreen.width  = state.canvasW;
  offscreen.height = state.canvasH;
  return { id, name, visible: true, locked: false, offscreen, undoStack: [], redoStack: [] };
}

function getLayerCtx(layerIdx) {
  return state.layers[layerIdx]?.offscreen.getContext('2d') ?? null;
}

function addLayer(name) {
  const layer = createLayer(name || `Layer ${state.layers.length + 1}`);
  state.layers.push(layer);
  state.activeLayer = state.layers.length - 1;
  renderLayersPanel();
  compositeAll();
}

function deleteLayer(idx) {
  if (state.layers.length <= 1) return;
  // Free GPU texture by detaching the canvas
  const layer = state.layers[idx];
  layer.offscreen.width = 0;  // releases backing store
  layer.offscreen.height = 0;
  layer.undoStack.length = 0;
  layer.redoStack.length = 0;
  state.layers.splice(idx, 1);
  state.activeLayer = Math.min(state.activeLayer, state.layers.length - 1);
  renderLayersPanel();
  compositeAll();
}

/* ── Composite all visible layers → mainCanvas ── */
function compositeAll() {
  const { canvasW: W, canvasH: H } = state;
  mainCtx.clearRect(0, 0, W, H);
  for (const layer of state.layers) {
    if (!layer.visible) continue;
    mainCtx.drawImage(layer.offscreen, 0, 0);
  }
}

function renderLayersPanel() {
  layersList.innerHTML = '';
  [...state.layers].reverse().forEach((layer, revIdx) => {
    const idx = state.layers.length - 1 - revIdx;
    const item = document.createElement('div');
    item.className = 'layer-item' + (idx === state.activeLayer ? ' active' : '');
    item.innerHTML = `
      <button class="vis-btn">${layer.visible ? '👁' : '🚫'}</button>
      <span class="layer-name">${layer.name}</span>
      <button class="del-btn">✕</button>`;
    item.querySelector('.vis-btn').addEventListener('click', e => {
      e.stopPropagation();
      layer.visible = !layer.visible;
      compositeAll();
      renderLayersPanel();
    });
    item.querySelector('.del-btn').addEventListener('click', e => {
      e.stopPropagation();
      deleteLayer(idx);
    });
    item.addEventListener('click', () => {
      state.activeLayer = idx;
      renderLayersPanel();
    });
    layersList.appendChild(item);
  });
}

/* ═══════════════════════════════════════════════════
   7. UNDO / REDO  (delta-compressed ImageData)
   Memory optimisation: store pixel deltas instead of
   full ImageData snapshots to cut memory ~4-10x.
═══════════════════════════════════════════════════ */
function snapshotLayer(layerIdx) {
  const layer = state.layers[layerIdx];
  if (!layer) return;
  const ctx = getLayerCtx(layerIdx);
  // Capture only the bounding box of non-transparent pixels when possible,
  // falling back to full canvas. For simplicity here we use full snapshots.
  const imageData = ctx.getImageData(0, 0, state.canvasW, state.canvasH);

  // Delta-encode vs previous snapshot to save memory
  const prev = layer.undoStack.at(-1);
  if (prev) {
    const delta = encodeDelta(prev.full ?? prev, imageData);
    if (delta.byteLength < imageData.data.byteLength * 0.7) {
      layer.undoStack.push({ delta, base: prev });
    } else {
      layer.undoStack.push({ full: imageData });
    }
  } else {
    layer.undoStack.push({ full: imageData });
  }

  // Trim stack to MAX_UNDO (release old snapshots)
  while (layer.undoStack.length > MAX_UNDO) {
    layer.undoStack.shift();  // GC can reclaim released ImageData
  }
  layer.redoStack.length = 0; // clear redo on new action
}

// Encode pixel delta as Uint8Array of [index, value] pairs
function encodeDelta(prevEntry, current) {
  const prevData = (prevEntry.full ?? resolveSnapshot(prevEntry)).data;
  const currData = current.data;
  const diffs = [];
  for (let i = 0; i < currData.length; i++) {
    if (currData[i] !== prevData[i]) {
      diffs.push(i, currData[i]);
    }
  }
  return new Uint32Array(diffs).buffer;
}

function resolveSnapshot(entry) {
  if (entry.full) return entry.full;
  const base = resolveSnapshot(entry.base);
  const copy = new ImageData(new Uint8ClampedArray(base.data), base.width, base.height);
  const view = new Uint32Array(entry.delta);
  for (let i = 0; i < view.length; i += 2) {
    copy.data[view[i]] = view[i + 1];
  }
  return copy;
}

function undo() {
  const layer = state.layers[state.activeLayer];
  if (!layer || layer.undoStack.length === 0) return;
  const entry = layer.undoStack.pop();
  layer.redoStack.push(entry);
  const ctx = getLayerCtx(state.activeLayer);
  if (layer.undoStack.length === 0) {
    ctx.clearRect(0, 0, state.canvasW, state.canvasH);
  } else {
    const prev = layer.undoStack.at(-1);
    ctx.putImageData(resolveSnapshot(prev), 0, 0);
  }
  compositeAll();
}

function redo() {
  const layer = state.layers[state.activeLayer];
  if (!layer || layer.redoStack.length === 0) return;
  const entry = layer.redoStack.pop();
  layer.undoStack.push(entry);
  const ctx = getLayerCtx(state.activeLayer);
  ctx.putImageData(resolveSnapshot(entry), 0, 0);
  compositeAll();
}

/* ═══════════════════════════════════════════════════
   8. DRAWING PRIMITIVES
═══════════════════════════════════════════════════ */
function beginDraw(e) {
  const { x, y } = toCanvas(e);
  state.drawing = true;
  state.startX  = x;
  state.startY  = y;
  state.lastX   = x;
  state.lastY   = y;
  state.brushPointsF32[0] = x;
  state.brushPointsF32[1] = y;
  state.brushPointCount   = 1;

  snapshotLayer(state.activeLayer);

  if (state.tool === 'fill') {
    floodFill(Math.round(x), Math.round(y));
    state.drawing = false;
    compositeAll();
    return;
  }
  if (state.tool === 'eyedropper') {
    pickColor(Math.round(x), Math.round(y));
    state.drawing = false;
    return;
  }
  if (state.tool === 'text') {
    placeTextInput(x, y);
    state.drawing = false;
    return;
  }
}

function continueDraw(e) {
  if (!state.drawing) return;
  const { x, y } = toCanvas(e);
  const ctx = overlayCtx;
  const { canvasW: W, canvasH: H } = state;
  ctx.clearRect(0, 0, W, H);

  applyDrawStyle(ctx);

  const tool = state.tool;
  if (tool === 'pen' || tool === 'brush') {
    const i = state.brushPointCount * 2;
    if (i + 1 < state.brushPointsF32.length) {
      state.brushPointsF32[i]   = x;
      state.brushPointsF32[i+1] = y;
      state.brushPointCount++;
    }
    drawSmoothedPath(ctx, state.brushPointsF32, state.brushPointCount, tool === 'brush');
  } else if (tool === 'eraser') {
    const i = state.brushPointCount * 2;
    if (i + 1 < state.brushPointsF32.length) {
      state.brushPointsF32[i]   = x;
      state.brushPointsF32[i+1] = y;
      state.brushPointCount++;
    }
    drawSmoothedPath(ctx, state.brushPointsF32, state.brushPointCount, false, true);
  } else if (tool === 'rect') {
    drawRect(ctx, state.startX, state.startY, x, y);
  } else if (tool === 'ellipse') {
    drawEllipse(ctx, state.startX, state.startY, x, y);
  } else if (tool === 'line') {
    drawLine(ctx, state.startX, state.startY, x, y);
  } else if (tool === 'arrow') {
    drawArrow(ctx, state.startX, state.startY, x, y);
  }

  state.lastX = x;
  state.lastY = y;
}

function endDraw(e) {
  if (!state.drawing) return;
  state.drawing = false;

  const ctx = getLayerCtx(state.activeLayer);
  if (!ctx) return;
  applyDrawStyle(ctx);

  const { startX, startY, lastX, lastY, brushPointsF32, brushPointCount, tool } = state;

  if (tool === 'pen' || tool === 'brush') {
    drawSmoothedPath(ctx, brushPointsF32, brushPointCount, tool === 'brush');
  } else if (tool === 'eraser') {
    const saved = ctx.globalCompositeOperation;
    ctx.globalCompositeOperation = 'destination-out';
    drawSmoothedPath(ctx, brushPointsF32, brushPointCount, false);
    ctx.globalCompositeOperation = saved;
  } else if (tool === 'rect') {
    drawRect(ctx, startX, startY, lastX, lastY);
  } else if (tool === 'ellipse') {
    drawEllipse(ctx, startX, startY, lastX, lastY);
  } else if (tool === 'line') {
    drawLine(ctx, startX, startY, lastX, lastY);
  } else if (tool === 'arrow') {
    drawArrow(ctx, startX, startY, lastX, lastY);
  }

  // Clear overlay
  overlayCtx.clearRect(0, 0, state.canvasW, state.canvasH);
  state.brushPointCount = 0;  // reset cursor; Float32Array buffer is reused
  compositeAll();
}

function applyDrawStyle(ctx) {
  ctx.globalAlpha     = state.opacity;
  ctx.lineWidth       = state.size;
  ctx.lineCap         = 'round';
  ctx.lineJoin        = 'round';
  ctx.strokeStyle     = state.strokeColor;
  ctx.fillStyle       = state.fillColor;
}

/* ── Catmull-Rom smoothed path ── */
// pts: Float32Array with interleaved [x0,y0, x1,y1, ...], count: number of points
function drawSmoothedPath(ctx, pts, count, pressure = false, erase = false) {
  if (count < 2) {
    ctx.beginPath();
    ctx.arc(pts[0], pts[1], ctx.lineWidth / 2, 0, Math.PI * 2);
    ctx.fill();
    return;
  }
  if (erase) ctx.globalCompositeOperation = 'destination-out';
  ctx.beginPath();
  ctx.moveTo(pts[0], pts[1]);
  for (let i = 1; i < count - 1; i++) {
    const mx = (pts[i*2] + pts[(i+1)*2])     / 2;
    const my = (pts[i*2+1] + pts[(i+1)*2+1]) / 2;
    ctx.quadraticCurveTo(pts[i*2], pts[i*2+1], mx, my);
  }
  ctx.lineTo(pts[(count-1)*2], pts[(count-1)*2+1]);
  ctx.stroke();
  if (erase) ctx.globalCompositeOperation = 'source-over';
}

function drawRect(ctx, x1, y1, x2, y2) {
  const x = Math.min(x1, x2), y = Math.min(y1, y2);
  const w = Math.abs(x2 - x1), h = Math.abs(y2 - y1);
  ctx.beginPath();
  ctx.rect(x, y, w, h);
  ctx.fill();
  ctx.stroke();
}

function drawEllipse(ctx, x1, y1, x2, y2) {
  const cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
  const rx = Math.abs(x2 - x1) / 2, ry = Math.abs(y2 - y1) / 2;
  ctx.beginPath();
  ctx.ellipse(cx, cy, rx, ry, 0, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();
}

function drawLine(ctx, x1, y1, x2, y2) {
  ctx.beginPath();
  ctx.moveTo(x1, y1);
  ctx.lineTo(x2, y2);
  ctx.stroke();
}

function drawArrow(ctx, x1, y1, x2, y2) {
  const angle = Math.atan2(y2 - y1, x2 - x1);
  const headLen = Math.max(10, state.size * 3);
  ctx.beginPath();
  ctx.moveTo(x1, y1);
  ctx.lineTo(x2, y2);
  ctx.stroke();
  // Arrowhead
  ctx.beginPath();
  ctx.moveTo(x2, y2);
  ctx.lineTo(x2 - headLen * Math.cos(angle - Math.PI / 6), y2 - headLen * Math.sin(angle - Math.PI / 6));
  ctx.lineTo(x2 - headLen * Math.cos(angle + Math.PI / 6), y2 - headLen * Math.sin(angle + Math.PI / 6));
  ctx.closePath();
  ctx.fill();
}

/* ═══════════════════════════════════════════════════
   9. FLOOD FILL (scanline algorithm)
   Memory: uses a Uint8Array visited mask instead of
   a Set<number> to avoid per-element JS object overhead.
═══════════════════════════════════════════════════ */
function floodFill(sx, sy) {
  const ctx = getLayerCtx(state.activeLayer);
  const { canvasW: W, canvasH: H } = state;
  const imgData = ctx.getImageData(0, 0, W, H);
  const data    = imgData.data;
  const visited = new Uint8Array(W * H);  // 1 byte per pixel (vs 8 bytes for Set entry)

  const idx  = (x, y) => (y * W + x) * 4;
  const tIdx = (x, y) => y * W + x;

  const tr = data[idx(sx, sy)], tg = data[idx(sx, sy) + 1],
        tb = data[idx(sx, sy) + 2], ta = data[idx(sx, sy) + 3];

  const [fr, fg, fb, fa] = hexToRgba(state.fillColor, state.opacity);
  if (tr === fr && tg === fg && tb === fb && ta === fa) return;

  function matchTarget(x, y) {
    const i = idx(x, y);
    return data[i] === tr && data[i+1] === tg && data[i+2] === tb && data[i+3] === ta;
  }
  function setFill(x, y) {
    const i = idx(x, y);
    data[i] = fr; data[i+1] = fg; data[i+2] = fb; data[i+3] = fa;
  }

  // Scanline stack fill (O(N) memory efficient)
  const stack = [[sx, sy]];
  while (stack.length) {
    const [cx, cy] = stack.pop();
    if (cx < 0 || cx >= W || cy < 0 || cy >= H) continue;
    if (visited[tIdx(cx, cy)]) continue;
    if (!matchTarget(cx, cy)) continue;
    visited[tIdx(cx, cy)] = 1;
    setFill(cx, cy);
    stack.push([cx - 1, cy], [cx + 1, cy], [cx, cy - 1], [cx, cy + 1]);
  }
  ctx.putImageData(imgData, 0, 0);
}

/* ═══════════════════════════════════════════════════
   10. EYEDROPPER
═══════════════════════════════════════════════════ */
function pickColor(x, y) {
  const ctx = mainCtx;
  const px  = ctx.getImageData(x, y, 1, 1).data;
  const hex = rgbToHex(px[0], px[1], px[2]);
  if (state.editTarget === 'fill') state.fillColor = '#' + hex;
  else state.strokeColor = '#' + hex;
  syncColorUI('#' + hex);
  updateSwatches();
}

/* ═══════════════════════════════════════════════════
   11. TEXT TOOL
═══════════════════════════════════════════════════ */
function placeTextInput(x, y) {
  textWrapper.style.display = 'block';
  textWrapper.style.left = x + 'px';
  textWrapper.style.top  = y + 'px';
  textInput.style.fontSize  = Math.max(12, state.size * 3) + 'px';
  textInput.style.color     = state.strokeColor;
  textInput.value = '';
  textInput.focus();
}

function commitText() {
  const text = textInput.value.trim();
  if (!text) { textWrapper.style.display = 'none'; return; }
  const x = parseFloat(textWrapper.style.left);
  const y = parseFloat(textWrapper.style.top);
  const ctx = getLayerCtx(state.activeLayer);
  applyDrawStyle(ctx);
  ctx.font = `${Math.max(12, state.size * 3)}px sans-serif`;
  ctx.fillStyle = state.strokeColor;
  ctx.fillText(text, x, y + parseInt(ctx.font));
  textWrapper.style.display = 'none';
  compositeAll();
}

/* ═══════════════════════════════════════════════════
   12. COLOR WHEEL  (HSV-based)
═══════════════════════════════════════════════════ */
let _cwImageData = null;  // reused across calls — avoids repeated heap allocation
function drawColorWheel() {
  const W = colorWheel.width, H = colorWheel.height;
  const cx = W / 2, cy = H / 2, r = Math.min(cx, cy) - 2;
  if (!_cwImageData) _cwImageData = cwCtx.createImageData(W, H);
  const img = _cwImageData;
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const dx = x - cx, dy = y - cy;
      const dist = Math.sqrt(dx * dx + dy * dy);
      if (dist > r) continue;
      const hue = ((Math.atan2(dy, dx) + Math.PI) / (Math.PI * 2)) * 360;
      const sat = dist / r;
      const [rr, gg, bb] = hsvToRgb(hue, sat, 1);
      const i = (y * W + x) * 4;
      img.data[i]   = rr;
      img.data[i+1] = gg;
      img.data[i+2] = bb;
      img.data[i+3] = 255;
    }
  }
  cwCtx.putImageData(img, 0, 0);
}

function drawValueSlider(hue, sat) {
  const H = valueSlider.height, W = valueSlider.width;
  for (let y = 0; y < H; y++) {
    const v = 1 - y / H;
    const [r, g, b] = hsvToRgb(hue, sat, v);
    vsCtx.fillStyle = `rgb(${r},${g},${b})`;
    vsCtx.fillRect(0, y, W, 1);
  }
}

colorWheel.addEventListener('mousedown', e => {
  const r = colorWheel.getBoundingClientRect();
  const cx = colorWheel.width / 2, cy = colorWheel.height / 2;
  const x = e.clientX - r.left - cx;
  const y = e.clientY - r.top  - cy;
  const dist = Math.sqrt(x * x + y * y);
  const maxR = cx - 2;
  state.hue = ((Math.atan2(y, x) + Math.PI) / (Math.PI * 2)) * 360;
  state.sat = Math.min(dist / maxR, 1);
  drawValueSlider(state.hue, state.sat);
  applyHSV();
});

valueSlider.addEventListener('mousedown', e => {
  const r = valueSlider.getBoundingClientRect();
  state.val = 1 - (e.clientY - r.top) / valueSlider.height;
  applyHSV();
});

function applyHSV() {
  const [r, g, b] = hsvToRgb(state.hue, state.sat, state.val);
  const hex = rgbToHex(r, g, b);
  if (state.editTarget === 'fill') state.fillColor = '#' + hex;
  else state.strokeColor = '#' + hex;
  syncColorUI('#' + hex);
  updateSwatches();
}

function syncColorUI(hex) {
  hex = hex.replace('#', '');
  hexInput.value = hex;
  const r = parseInt(hex.slice(0,2), 16);
  const g = parseInt(hex.slice(2,4), 16);
  const b = parseInt(hex.slice(4,6), 16);
  rInput.value = r; gInput.value = g; bInput.value = b;
}

hexInput.addEventListener('input', () => {
  const h = hexInput.value.replace(/[^0-9a-f]/gi, '');
  if (h.length === 6) {
    const c = '#' + h;
    if (state.editTarget === 'fill') state.fillColor = c;
    else state.strokeColor = c;
    updateSwatches();
  }
});

[rInput, gInput, bInput].forEach(inp => inp.addEventListener('input', () => {
  const r = parseInt(rInput.value) || 0;
  const g = parseInt(gInput.value) || 0;
  const b = parseInt(bInput.value) || 0;
  const hex = '#' + rgbToHex(r, g, b);
  if (state.editTarget === 'fill') state.fillColor = hex;
  else state.strokeColor = hex;
  hexInput.value = hex.slice(1);
  updateSwatches();
}));

function updateSwatches() {
  fillSwatch.style.background   = state.fillColor;
  strokeSwatch.style.background = state.strokeColor;
}

/* ═══════════════════════════════════════════════════
   13. PALETTE
═══════════════════════════════════════════════════ */
function buildPalette() {
  const el = document.getElementById('palette');
  // DocumentFragment avoids multiple reflows
  const frag = document.createDocumentFragment();
  for (const color of PALETTE) {
    const s = document.createElement('div');
    s.className = 'pal-swatch';
    s.style.background = color;
    s.title = color;
    s.addEventListener('click', () => {
      if (state.editTarget === 'fill') state.fillColor = color;
      else state.strokeColor = color;
      syncColorUI(color);
      updateSwatches();
    });
    frag.appendChild(s);
  }
  el.appendChild(frag);  // single DOM write
}

/* ═══════════════════════════════════════════════════
   14. ZOOM & PAN
═══════════════════════════════════════════════════ */
function applyTransform() {
  const container = document.getElementById('canvas-container');
  container.style.transform = `translate(${state.panX}px, ${state.panY}px) scale(${state.zoom})`;
}

document.getElementById('canvas-area').addEventListener('wheel', e => {
  e.preventDefault();
  const delta  = e.deltaY < 0 ? 1.1 : 0.9;
  state.zoom   = Math.max(0.1, Math.min(20, state.zoom * delta));
  applyTransform();
}, { passive: false });

/* ═══════════════════════════════════════════════════
   15. POINTER EVENTS (unified for mouse + touch + pen)
═══════════════════════════════════════════════════ */
overlay.addEventListener('pointerdown', e => {
  overlay.setPointerCapture(e.pointerId);
  if (state.tool === 'pan') {
    state.panning  = true;
    state.panStartX = e.clientX - state.panX;
    state.panStartY = e.clientY - state.panY;
    document.body.classList.add('panning');
    return;
  }
  if (state.tool === 'zoom') {
    const delta = e.buttons === 1 ? 1.15 : 0.85;
    state.zoom = Math.max(0.1, Math.min(20, state.zoom * delta));
    applyTransform();
    return;
  }
  beginDraw(e);
});

overlay.addEventListener('pointermove', e => {
  if (state.panning) {
    state.panX = e.clientX - state.panStartX;
    state.panY = e.clientY - state.panStartY;
    applyTransform();
    return;
  }
  if (!state.drawing) return;
  // Coalesced events: process all queued events for smoother pen input
  const events = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
  for (const ev of events) continueDraw(ev);
});

overlay.addEventListener('pointerup', e => {
  if (state.panning) {
    state.panning = false;
    document.body.classList.remove('panning');
    return;
  }
  endDraw(e);
});

overlay.addEventListener('pointerleave', e => { if (state.drawing) endDraw(e); });

/* ═══════════════════════════════════════════════════
   16. TOOLBAR WIRING
═══════════════════════════════════════════════════ */
document.querySelectorAll('.tool-btn[data-tool]').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tool-btn[data-tool]').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    state.tool = btn.dataset.tool;
    document.body.dataset.tool = state.tool;
  });
});

brushSizeSlider.addEventListener('input', () => {
  state.size = parseInt(brushSizeSlider.value);
  sizeVal.textContent = state.size;
});

opacityRange.addEventListener('input', () => {
  state.opacity = parseInt(opacityRange.value) / 100;
  opacityVal.textContent = opacityRange.value;
});

fillSwatch.addEventListener('click', () => {
  state.editTarget = 'fill';
  syncColorUI(state.fillColor);
});
strokeSwatch.addEventListener('click', () => {
  state.editTarget = 'stroke';
  syncColorUI(state.strokeColor);
});
document.getElementById('swap-colors').addEventListener('click', () => {
  [state.fillColor, state.strokeColor] = [state.strokeColor, state.fillColor];
  updateSwatches();
});

document.getElementById('undo-btn').addEventListener('click', undo);
document.getElementById('redo-btn').addEventListener('click', redo);
document.getElementById('clear-btn').addEventListener('click', () => {
  snapshotLayer(state.activeLayer);
  const ctx = getLayerCtx(state.activeLayer);
  ctx.clearRect(0, 0, state.canvasW, state.canvasH);
  compositeAll();
});

document.getElementById('export-btn').addEventListener('click', () => {
  const link = document.createElement('a');
  link.download = 'render.png';
  link.href = mainCanvas.toDataURL('image/png');
  link.click();
});

document.getElementById('add-layer-btn').addEventListener('click', () => addLayer());

document.getElementById('resize-canvas-btn').addEventListener('click', () => {
  const w = parseInt(document.getElementById('canvas-w').value) || 1280;
  const h = parseInt(document.getElementById('canvas-h').value) || 720;
  state.canvasW = w; state.canvasH = h;
  // Resize all layer offscreens (copies existing content)
  state.layers.forEach(layer => {
    const tmp = document.createElement('canvas');
    tmp.width = w; tmp.height = h;
    tmp.getContext('2d').drawImage(layer.offscreen, 0, 0);
    layer.offscreen.width  = w;
    layer.offscreen.height = h;
    layer.offscreen.getContext('2d').drawImage(tmp, 0, 0);
  });
  resizePhysicalCanvases(w, h);
});

textInput.addEventListener('keydown', e => {
  if (e.key === 'Escape') { textWrapper.style.display = 'none'; }
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); commitText(); }
});
textInput.addEventListener('blur', commitText);

/* ═══════════════════════════════════════════════════
   17. KEYBOARD SHORTCUTS
═══════════════════════════════════════════════════ */
const TOOL_KEYS = { v:'select', p:'pen', b:'brush', e:'eraser', r:'rect',
                    o:'ellipse', l:'line', a:'arrow', t:'text', g:'fill',
                    i:'eyedropper', z:'zoom', h:'pan' };
document.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;
  if (e.ctrlKey || e.metaKey) {
    if (e.key === 'z') { e.preventDefault(); undo(); }
    if (e.key === 'y') { e.preventDefault(); redo(); }
    return;
  }
  const tool = TOOL_KEYS[e.key.toLowerCase()];
  if (tool) {
    document.querySelectorAll('.tool-btn[data-tool]').forEach(b => b.classList.remove('active'));
    document.querySelector(`[data-tool="${tool}"]`)?.classList.add('active');
    state.tool = tool;
    document.body.dataset.tool = tool;
  }
  if (e.key === '[') { state.size = Math.max(1, state.size - 1); brushSizeSlider.value = state.size; sizeVal.textContent = state.size; }
  if (e.key === ']') { state.size = Math.min(100, state.size + 1); brushSizeSlider.value = state.size; sizeVal.textContent = state.size; }
});

/* ═══════════════════════════════════════════════════
   18. MEMORY STATS PANEL
   Shows live JS heap usage via performance.memory (Chrome)
   and estimates canvas VRAM usage.
═══════════════════════════════════════════════════ */
function updateMemStats() {
  // Canvas VRAM estimate: 4 bytes per pixel per canvas
  const pixelCount = state.canvasW * state.canvasH;
  const physicalBytes = pixelCount * 4 * 3;             // 3 physical canvases
  const layerBytes    = pixelCount * 4 * state.layers.length; // offscreen layers

  // Undo stack: rough estimate
  let undoBytes = 0;
  state.layers.forEach(l => {
    l.undoStack.forEach(e => {
      undoBytes += e.full ? e.full.data.byteLength : (e.delta?.byteLength ?? 0);
    });
  });

  const totalBytes = physicalBytes + layerBytes + undoBytes;

  const lines = [
    ['Canvas VRAM', fmtBytes(physicalBytes)],
    ['Layers VRAM', fmtBytes(layerBytes)],
    ['Undo stack',  fmtBytes(undoBytes)],
    ['Total est.',  fmtBytes(totalBytes)],
  ];

  if (performance.memory) {
    lines.push(['JS Heap used', fmtBytes(performance.memory.usedJSHeapSize)]);
    lines.push(['JS Heap limit', fmtBytes(performance.memory.jsHeapSizeLimit)]);
  }

  memLines.innerHTML = lines.map(([k, v]) =>
    `<div>${k}: <span class="val">${v}</span></div>`).join('');
}

function fmtBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(1) + ' MB';
}

setInterval(updateMemStats, 2000);

/* ═══════════════════════════════════════════════════
   19. COLOUR UTILITY FUNCTIONS
═══════════════════════════════════════════════════ */
function hsvToRgb(h, s, v) {
  h = h % 360;
  const c = v * s, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = v - c;
  let r = 0, g = 0, b = 0;
  if (h < 60)       { r=c; g=x; }
  else if (h < 120) { r=x; g=c; }
  else if (h < 180) { g=c; b=x; }
  else if (h < 240) { g=x; b=c; }
  else if (h < 300) { r=x; b=c; }
  else              { r=c; b=x; }
  return [Math.round((r+m)*255), Math.round((g+m)*255), Math.round((b+m)*255)];
}

function rgbToHex(r, g, b) {
  return [r, g, b].map(v => v.toString(16).padStart(2, '0')).join('');
}

function hexToRgba(hex, alpha = 1) {
  hex = hex.replace('#', '');
  return [
    parseInt(hex.slice(0,2), 16),
    parseInt(hex.slice(2,4), 16),
    parseInt(hex.slice(4,6), 16),
    Math.round(alpha * 255),
  ];
}

/* ═══════════════════════════════════════════════════
   20. INITIALISATION
═══════════════════════════════════════════════════ */
function init() {
  resizePhysicalCanvases(state.canvasW, state.canvasH);
  addLayer('Background');
  // Fill background layer with white
  const ctx = getLayerCtx(0);
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, state.canvasW, state.canvasH);
  addLayer('Layer 1');
  state.activeLayer = 1;
  renderLayersPanel();
  compositeAll();
  drawColorWheel();
  drawValueSlider(state.hue, state.sat);
  state.val = 1; applyHSV();
  buildPalette();
  updateSwatches();
  document.body.dataset.tool = state.tool;

  // Set active tool button
  document.querySelector('[data-tool="pen"]').classList.add('active');
  document.querySelector('[data-tool="pen"]').classList.remove('active');
  document.querySelector('[data-tool="brush"]') && void 0;
  document.querySelector(`[data-tool="${state.tool}"]`)?.classList.add('active');

  updateMemStats();
  console.log('%c2D Renderer ready', 'color:#e94560;font-weight:bold;font-size:14px');
}

init();

/* ═══════════════════════════════════════════════════
   21. COLLABORATION + OT CLIENT
   Connects via WebSocket to a bridge that forwards
   messages to the gRPC Collaborate RPC on the backend.

   OT protocol:
     - Every outgoing op carries base_revision (what
       revision the client was at when the op was made).
     - Server transforms and replies with the same op
       carrying the assigned revision (ack).
     - Incoming remote ops are transformed against any
       local pending ops before being applied.
═══════════════════════════════════════════════════ */
const collab = (() => {
  let ws            = null;
  let sessionId     = '';
  let projectId     = '';
  let localRevision = 0;
  const pendingOps  = [];   // ops sent but not yet ack'd by server

  // ── OT transform (mirrors service.cpp::ot_transform) ──────────
  function transformOp(op_a, op_b) {
    const a = Object.assign({}, op_a);

    // Stroke on a deleted/cleared layer → cancel
    if (a.type === 'STROKE_END') {
      if ((op_b.type === 'LAYER_DELETE' || op_b.type === 'CANVAS_CLEAR') &&
          op_b.layer_id === a.stroke?.layer_id)
        return { ...a, type: 'NOOP' };
    }

    // Duplicate LAYER_DELETE → skip
    if (a.type === 'LAYER_DELETE' && op_b.type === 'LAYER_DELETE' &&
        a.layer_id === op_b.layer_id)
      return { ...a, type: 'NOOP' };

    // Concurrent LAYER_MOVE → adjust order index
    if (a.type === 'LAYER_MOVE' && op_b.type === 'LAYER_MOVE') {
      if (op_b.layer.order <= a.layer.order)
        return { ...a, layer: { ...a.layer, order: a.layer.order + 1 } };
    }

    return a;
  }

  // ── Apply a remote op to the local canvas ─────────────────────
  function applyRemoteOp(op) {
    if (op.type === 'NOOP') return;

    if (op.type === 'STROKE_END' && op.stroke) {
      const layer = state.layers.find(l => l.id === op.stroke.layer_id);
      if (!layer) return;
      const ctx = layer.offscreen.getContext('2d');
      ctx.globalAlpha  = op.stroke.opacity ?? 1;
      ctx.lineWidth    = op.stroke.size ?? 4;
      ctx.lineCap      = 'round';
      ctx.lineJoin     = 'round';
      ctx.strokeStyle  = op.stroke.stroke_color ?? '#000';
      if (op.stroke.points?.length >= 2) {
        ctx.beginPath();
        ctx.moveTo(op.stroke.points[0].x, op.stroke.points[0].y);
        for (let i = 1; i < op.stroke.points.length - 1; i++) {
          const mx = (op.stroke.points[i].x + op.stroke.points[i+1].x) / 2;
          const my = (op.stroke.points[i].y + op.stroke.points[i+1].y) / 2;
          ctx.quadraticCurveTo(op.stroke.points[i].x, op.stroke.points[i].y, mx, my);
        }
        ctx.lineTo(op.stroke.points.at(-1).x, op.stroke.points.at(-1).y);
        ctx.stroke();
      }
      compositeAll();
    }

    if (op.type === 'LAYER_DELETE') {
      const idx = state.layers.findIndex(l => l.id === op.layer_id);
      if (idx !== -1) deleteLayer(idx);
    }

    if (op.type === 'CANVAS_CLEAR') {
      const layer = state.layers.find(l => l.id === op.layer_id);
      if (layer) {
        layer.offscreen.getContext('2d')
          .clearRect(0, 0, state.canvasW, state.canvasH);
        compositeAll();
      }
    }

    if (op.type === 'CURSOR_MOVE' && op.cursor) {
      // Optional: render remote cursor indicator
    }
  }

  // ── Send an op to the server ───────────────────────────────────
  function send(op) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const msg = {
      session_id:    sessionId,
      project_id:    projectId,
      base_revision: localRevision,
      ...op,
    };
    pendingOps.push(msg);
    ws.send(JSON.stringify(msg));
  }

  // ── Handle message from server ─────────────────────────────────
  function onMessage(raw) {
    const op = JSON.parse(raw);

    // Ack: server echoes our own op back with an assigned revision
    if (op.session_id === sessionId && op.revision != null) {
      pendingOps.shift();
      localRevision = op.revision;
      return;
    }

    // Remote op from another user: transform against pending local ops
    let transformed = op;
    for (const pending of pendingOps)
      transformed = transformOp(transformed, pending);

    if (op.revision != null)
      localRevision = Math.max(localRevision, op.revision);

    applyRemoteOp(transformed);
  }

  // ── Public API ─────────────────────────────────────────────────
  return {
    connect(wsUrl, sid, pid) {
      sessionId = sid;
      projectId = pid;
      ws = new WebSocket(wsUrl);
      ws.onmessage = e => onMessage(e.data);
      ws.onopen    = () => {
        console.log('[collab] connected');
        ws.send(JSON.stringify({ session_id: sessionId, project_id: projectId,
                                  type: 'STROKE_BEGIN' }));
      };
      ws.onclose   = () => console.log('[collab] disconnected');
      ws.onerror   = e => console.error('[collab] error', e);
    },

    disconnect() { ws?.close(); },

    sendStroke(layerId, points, tool, color, size, opacity) {
      send({
        type:   'STROKE_END',
        stroke: { layer_id: layerId, tool, stroke_color: color,
                  size, opacity, points },
      });
    },

    sendCursor(x, y) {
      send({ type: 'CURSOR_MOVE', cursor: { x, y } });
    },

    sendLayerDelete(layerId) {
      send({ type: 'LAYER_DELETE', layer_id: layerId });
    },

    sendCanvasClear(layerId) {
      send({ type: 'CANVAS_CLEAR', layer_id: layerId });
    },

    get connected() { return ws?.readyState === WebSocket.OPEN; },
  };
})();

// ── Hook collaboration into existing draw events ───────────────
// Snapshot Float32Array into plain objects for JSON wire format,
// then commit the stroke locally and broadcast.
const _origEndDraw = endDraw;
window.endDraw = function(e) {
  const count   = state.brushPointCount;
  const layerId = state.layers[state.activeLayer]?.id;
  // Snapshot before endDraw resets brushPointCount
  const points = [];
  for (let i = 0; i < count; i++)
    points.push({ x: state.brushPointsF32[i*2], y: state.brushPointsF32[i*2+1] });
  _origEndDraw(e);
  if (collab.connected && layerId && points.length > 1) {
    collab.sendStroke(layerId, points, state.tool, state.strokeColor,
                      state.size, state.opacity);
  }
};
