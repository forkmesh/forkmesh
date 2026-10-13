import { html, setHtml, trusted, join } from '../html.js';
import { BW, BH, COLORS, STICKY_COLORS, STICKY_W, STICKY_H, boardId, parseBoardId, newBoard, applyOp, bounds, hitTest, simplify, splitPoints, sanitizeItem } from './whiteboard/model.js';
import { drawItem, drawStroke, drawThumb, renderPng, FONT } from './whiteboard/draw.js';

const P = {
  select: html`<path d="M4 4l7 17 2.5-7.5L21 11z"/>`,
  pen: html`<path d="M17 3a2.85 2.85 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5Z"/>`,
  hl: html`<path d="m9 11-6 6v3h9l3-3"/><path d="m22 12-4.6 4.6a2 2 0 0 1-2.8 0l-5.2-5.2a2 2 0 0 1 0-2.8L14 4"/>`,
  eraser: html`<path d="m7 21-4.3-4.3c-1-1-1-2.5 0-3.4l9.6-9.6c1-1 2.5-1 3.4 0l5.6 5.6c1 1 1 2.5 0 3.4L13 21"/><path d="M22 21H7"/><path d="m5 11 9 9"/>`,
  text: html`<path d="M4 7V4h16v3"/><path d="M9 20h6"/><path d="M12 4v16"/>`,
  sticky: html`<path d="M15.5 3H5a2 2 0 0 0-2 2v14c0 1.1.9 2 2 2h14a2 2 0 0 0 2-2V8.5L15.5 3Z"/><path d="M15 3v6h6"/>`,
  rect: html`<rect x="3" y="5" width="18" height="14" rx="2"/>`,
  ellipse: html`<ellipse cx="12" cy="12" rx="9" ry="7"/>`,
  arrow: html`<path d="M5 19 19 5"/><path d="M9 5h10v10"/>`,
  undo: html`<path d="M9 14 4 9l5-5"/><path d="M4 9h10.5a5.5 5.5 0 0 1 0 11H11"/>`,
  redo: html`<path d="m15 14 5-5-5-5"/><path d="M20 9H9.5a5.5 5.5 0 0 0 0 11H13"/>`,
  trash: html`<path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/>`,
  download: html`<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="m7 10 5 5 5-5"/><path d="M12 15V3"/>`,
  x: html`<path d="M18 6 6 18"/><path d="m6 6 12 12"/>`,
  board: html`<path d="M2 3h20"/><path d="M21 3v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V3"/><path d="m7 21 5-5 5 5"/>`,
  eye: html`<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7Z"/><circle cx="12" cy="12" r="3"/>`,
};
const svg = (n, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${P[n]}</svg>`;

const TOOLS = [
  ['select', 'Select and move', 'v'], ['pen', 'Pen', 'p'], ['hl', 'Highlighter', 'h'], ['eraser', 'Eraser', 'e'],
  ['text', 'Text', 't'], ['sticky', 'Sticky note', 'n'], ['rect', 'Rectangle', 'r'], ['ellipse', 'Ellipse', 'o'], ['arrow', 'Arrow', 'a'],
];
const SIZES = [2, 4, 8, 14];
const TEXT_SIZES = { 2: 18, 4: 24, 8: 36, 14: 56 };
const SHAPE = { rect: 'r', ellipse: 'e', arrow: 'a' };
const LIVE_MS = 50;
const CUR_MS = 50;

export default function setup(api) {
  const { esc, tint } = api;

  const thumbs = new Map();
  let boards = [];
  let cur = null;
  let board = newBoard();
  let sorted = null;
  let loading = false;
  let canEditNow = false;
  let tool = 'pen', color = COLORS[0], sticky = STICKY_COLORS[0], size = 4;
  let undo = [], redo = [];
  let seq = 0, idn = 0;
  let selected = new Set();
  let drag = null, editor = null;
  let hidden = new Set();
  let baseDirty = true, frameReq = 0;
  let scale = 1;
  let lastCurSent = 0, lastPresence = '';
  const cursors = new Map();
  const lives = new Map();

  const root = document.createElement('div');
  root.className = 'wb';
  root.hidden = true;
  setHtml(root, html`
    <div class="wb-card" role="dialog" aria-label="Whiteboard">
      <header class="wb-head">
        <span class="wb-ic">${svg('board', 16)}</span>
        <b class="wb-title"></b>
        <span class="wb-ro" hidden>${svg('eye', 13)}<span></span></span>
        <div class="wb-people"></div>
        <button class="icon-btn" data-act="export" data-tip="Export PNG" aria-label="Export PNG">${svg('download')}</button>
        <button class="icon-btn" data-act="close" data-tip="Close (Esc)" aria-label="Close">${svg('x')}</button>
      </header>
      <div class="wb-bar">
        <div class="wb-group wb-tools">${TOOLS.map(([t, tip, k]) => html`<button class="wb-btn" data-tool="${t}" data-tip="${tip} (${k.toUpperCase()})" aria-label="${tip}">${svg(t)}</button>`)}</div>
        <div class="wb-group wb-colors"></div>
        <div class="wb-group wb-sizes">${SIZES.map((s) => html`<button class="wb-btn" data-size="${s}" data-tip="Size ${s}" aria-label="Size ${s}"><i style="width:${Math.min(16, s + 2)}px;height:${Math.min(16, s + 2)}px"></i></button>`)}</div>
        <div class="wb-group">
          <button class="wb-btn" data-act="undo" data-tip="Undo (Ctrl+Z)" aria-label="Undo">${svg('undo')}</button>
          <button class="wb-btn" data-act="redo" data-tip="Redo (Ctrl+Shift+Z)" aria-label="Redo">${svg('redo')}</button>
          <button class="wb-btn" data-act="clear" data-tip="Clear board" aria-label="Clear board">${svg('trash')}</button>
        </div>
      </div>
      <p class="wb-note" hidden></p>
      <div class="wb-stage">
        <div class="wb-sheet">
          <canvas class="wb-base"></canvas>
          <canvas class="wb-top"></canvas>
          <div class="wb-loading">Loading board…</div>
        </div>
      </div>
      <div class="wb-confirm" hidden>
        <div class="wb-confirm-box">
          <b>Clear the whole board?</b>
          <p>Everything on it is removed for everyone. This can't be undone.</p>
          <div class="wb-confirm-actions">
            <button class="btn btn-sm" data-act="cancel-clear">Cancel</button>
            <button class="btn btn-sm wb-danger" data-act="do-clear">Clear board</button>
          </div>
        </div>
      </div>
    </div>`);
  document.body.append(root);
  const q = (s) => root.querySelector(s);
  const sheet = q('.wb-sheet'), stage = q('.wb-stage'), baseCv = q('.wb-base'), topCv = q('.wb-top');
  const base = baseCv.getContext('2d'), top = topCv.getContext('2d');

  const zoneOf = (b) => { const p = parseBoardId(b); return p ? api.zoneAt(p.x, p.y) : ''; };
  const roomName = (b) => api.ZONES[zoneOf(b)]?.name || 'Office';
  const newId = () => `${(api.me?.id || 'x').slice(0, 8)}${Date.now().toString(36).slice(-5)}${(idn++).toString(36)}`;
  const personById = (id) => (id === api.me?.id ? api.me : api.others.get(id));
  const itemsInOrder = () => sorted || (sorted = [...board.items.values()].sort((a, b) => (a.z || 0) - (b.z || 0)));
  const touched = () => { sorted = null; baseDirty = true; paint(); };

  function renderColors() {
    const list = tool === 'sticky' ? STICKY_COLORS : COLORS;
    const active = tool === 'sticky' ? sticky : color;
    setHtml(q('.wb-colors'), join(list.map((c) => html`<button class="wb-sw${c === active ? ' on' : ''}" data-color="${c}" aria-label="Colour ${c}" style="--sw:${c}"></button>`)));
  }
  function renderBar() {
    root.querySelectorAll('[data-tool]').forEach((b) => b.classList.toggle('on', b.dataset.tool === tool));
    root.querySelectorAll('[data-size]').forEach((b) => b.classList.toggle('on', +b.dataset.size === size));
    q('[data-act="undo"]').disabled = !undo.length;
    q('[data-act="redo"]').disabled = !redo.length;
    q('.wb-sizes').hidden = tool === 'select' || tool === 'eraser' || tool === 'sticky';
    q('.wb-colors').hidden = tool === 'select' || tool === 'eraser';
    renderColors();
    root.classList.toggle('ro', !canEditNow);
    q('.wb-ro').hidden = canEditNow;
    q('.wb-ro span').textContent = `View only · walk into ${roomName(cur)} to draw`;
    topCv.style.cursor = !canEditNow ? 'default' : tool === 'select' ? 'default' : tool === 'text' ? 'text' : 'crosshair';
  }
  function setTool(t) {
    if (!TOOLS.some(([n]) => n === t)) return;
    commitEditor();
    tool = t;
    if (t !== 'select') selected.clear();
    renderBar();
    paint();
  }

  function renderPresence() {
    if (!cur) return;
    const list = [api.me, ...api.others.values()].filter((o) => o && (o === api.me || o.ex?.wb?.b === cur));
    const sig = list.map((o) => `${o.id}:${o.ex?.wb?.d ? 1 : 0}:${o.name}`).join('|');
    if (sig === lastPresence) return;
    lastPresence = sig;
    setHtml(q('.wb-people'), html`${list.slice(0, 8).map((o) => html`<span class="wb-av${o.ex?.wb?.d ? ' drawing' : ''}" style="background:${tint(o.av)}" title="${(o === api.me ? `${o.name} (you)` : o.name)}${o.ex?.wb?.d ? ' · drawing' : ''}">${((o.name[0] || '?').toUpperCase())}${o.ex?.wb?.d ? html`<i>${svg('pen', 9)}</i>` : ''}</span>`)}${list.length > 8 ? html`<span class="wb-more">+${list.length - 8}</span>` : ''}`);
  }

  function layout() {
    if (root.hidden) return;
    const r = stage.getBoundingClientRect();
    scale = Math.max(0.05, Math.min((r.width - 16) / BW, (r.height - 16) / BH));
    const w = Math.round(BW * scale), h = Math.round(BH * scale), dpr = devicePixelRatio || 1;
    sheet.style.width = `${w}px`;
    sheet.style.height = `${h}px`;
    for (const c of [baseCv, topCv]) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
      c.style.width = `${w}px`;
      c.style.height = `${h}px`;
    }
    if (editor) placeEditor();
    baseDirty = true;
    paint();
  }
  const ro = new ResizeObserver(layout);
  ro.observe(stage);

  function paint() {
    if (!frameReq && !root.hidden) frameReq = requestAnimationFrame(render);
  }
  function setT(g) {
    const k = baseCv.width / BW;
    g.setTransform(k, 0, 0, k, 0, 0);
  }
  function render(now) {
    frameReq = 0;
    if (baseDirty) {
      baseDirty = false;
      base.setTransform(1, 0, 0, 1, 0, 0);
      base.clearRect(0, 0, baseCv.width, baseCv.height);
      setT(base);
      const moving = drag?.kind === 'move' ? selected : null;
      for (const it of itemsInOrder()) if (!hidden.has(it.id) && !moving?.has(it.id)) drawItem(base, it);
    }
    top.setTransform(1, 0, 0, 1, 0, 0);
    top.clearRect(0, 0, topCv.width, topCv.height);
    setT(top);
    if (drag?.kind === 'move') {
      top.save();
      top.translate(drag.dx, drag.dy);
      for (const it of itemsInOrder()) if (selected.has(it.id)) drawItem(top, it);
      top.restore();
    }
    const t = performance.now();
    for (const [k, l] of lives) {
      if (t - l.t > 10000) { lives.delete(k); continue; }
      drawStroke(top, l);
    }
    if (drag?.kind === 'stroke') drawStroke(top, drag.item);
    if (drag?.kind === 'shape') drawItem(top, drag.item);
    if (selected.size) {
      top.save();
      top.strokeStyle = '#3b45e0';
      top.lineWidth = 1.5 / scale;
      top.setLineDash([6 / scale, 4 / scale]);
      const ox = drag?.kind === 'move' ? drag.dx : 0, oy = drag?.kind === 'move' ? drag.dy : 0;
      for (const id of selected) {
        const it = board.items.get(id);
        if (!it) continue;
        const b = bounds(it);
        top.strokeRect(b.x0 - 4 + ox, b.y0 - 4 + oy, b.x1 - b.x0 + 8, b.y1 - b.y0 + 8);
      }
      top.restore();
    }
    if (drag?.kind === 'marquee') {
      top.save();
      top.fillStyle = 'rgba(59,69,224,.08)';
      top.strokeStyle = '#3b45e0';
      top.lineWidth = 1 / scale;
      const x = Math.min(drag.x0, drag.x1), y = Math.min(drag.y0, drag.y1), w = Math.abs(drag.x1 - drag.x0), h = Math.abs(drag.y1 - drag.y0);
      top.fillRect(x, y, w, h);
      top.strokeRect(x, y, w, h);
      top.restore();
    }
    let keep = lives.size > 0;
    for (const [id, c] of cursors) {
      const o = api.others.get(id);
      if (!o || o.ex?.wb?.b !== cur || t - c.t > 8000) { cursors.delete(id); continue; }
      keep = true;
      drawCursor(top, c, o);
    }
    if (keep) setTimeout(paint, 500);
  }
  function drawCursor(g, c, o) {
    const s = 1 / scale;
    g.save();
    g.translate(c.x, c.y);
    g.scale(s, s);
    const col = api.others.get(o.id) ? tint(o.av) : '#3b45e0';
    g.fillStyle = col;
    g.strokeStyle = '#1d2030';
    g.lineWidth = 1.2;
    g.beginPath();
    g.moveTo(0, 0);
    g.lineTo(0, 15);
    g.lineTo(4, 11);
    g.lineTo(8, 18);
    g.lineTo(10.5, 17);
    g.lineTo(6.5, 10);
    g.lineTo(12, 10);
    g.closePath();
    g.fill();
    g.stroke();
    g.font = `600 11px ${FONT}`;
    const w = g.measureText(o.name).width + 12;
    g.fillStyle = '#1d2030';
    g.beginPath();
    g.roundRect(12, 16, w, 18, 6);
    g.fill();
    g.fillStyle = '#fff';
    g.textBaseline = 'middle';
    g.fillText(o.name, 18, 25.5);
    g.restore();
  }

  function toBoard(e) {
    const r = topCv.getBoundingClientRect();
    return { x: ((e.clientX - r.left) / r.width) * BW, y: ((e.clientY - r.top) / r.height) * BH };
  }
  function topHit(x, y, r = 8 / scale) {
    const list = itemsInOrder();
    for (let i = list.length - 1; i >= 0; i--) if (!hidden.has(list[i].id) && hitTest(list[i], x, y, r)) return list[i];
    return null;
  }

  function sendOp(op) {
    const s = ++seq;
    api.send({ t: 'wb.op', b: cur, s, op });
  }
  function doOp(op, record, undoOp) {
    const res = applyOp(board, op, api.me?.id);
    if (!res.ok) return null;
    sendOp(res.op);
    if (record) {
      undo.push({ undo: undoOp(res), redo: res.op });
      if (undo.length > 200) undo.shift();
      redo = [];
    }
    for (const id of res.del) selected.delete(id);
    touched();
    renderBar();
    return res;
  }
  const clone = (it) => sanitizeItem(JSON.parse(JSON.stringify(it)));
  function addItems(items) {
    return doOp({ o: 'add', items }, true, (res) => ({ o: 'del', ids: res.op.items.map((i) => i.id) }));
  }
  function delItems(ids) {
    const olds = ids.map((id) => board.items.get(id)).filter(Boolean).map(clone).filter(Boolean);
    return doOp({ o: 'del', ids }, true, () => ({ o: 'add', items: olds }));
  }
  function moveItems(ids, dx, dy) {
    return doOp({ o: 'mv', ids, dx, dy }, true, (res) => ({ o: 'mv', ids: res.op.ids, dx: -res.op.dx, dy: -res.op.dy }));
  }
  function updText(id, text) {
    const old = board.items.get(id)?.text;
    return doOp({ o: 'upd', id, text }, true, () => ({ o: 'upd', id, text: old }));
  }
  function step(from, to) {
    if (!canEditNow) return;
    const entry = from.pop();
    if (!entry) return;
    const op = from === undo ? entry.undo : entry.redo;
    const res = applyOp(board, JSON.parse(JSON.stringify(op)), api.me?.id);
    if (res.ok) sendOp(res.op);
    to.push(entry);
    for (const id of res.del) selected.delete(id);
    touched();
    renderBar();
  }

  function flushLive(end) {
    const d = drag;
    if (!d || d.kind !== 'stroke') return;
    const fresh = d.item.pts.slice(d.sent);
    const overlap = d.sent ? d.item.pts.slice(d.sent - 3, d.sent) : [];
    if (!fresh.length && !end) return;
    const pts = overlap.concat(fresh);
    for (let i = 0; i < pts.length || (end && i === 0); i += 597) {
      api.send({ t: 'wb.live', b: cur, id: d.item.id, c: d.item.c, w: d.item.w, hl: d.item.hl ? 1 : 0, pts: pts.slice(i, i + 600), end: end && i + 600 >= pts.length ? 1 : 0 });
      if (!pts.length) break;
    }
    d.sent = d.item.pts.length;
    d.liveAt = performance.now();
  }

  function pointerDown(e) {
    if (!cur || loading || e.button > 0) return;
    if (drag && drag.pid !== e.pointerId) return;
    commitEditor();
    const p = toBoard(e);
    if (!canEditNow) return;
    try { topCv.setPointerCapture(e.pointerId); } catch {}
    const pr = e.pointerType === 'pen' ? Math.round(Math.max(0.05, e.pressure || 0.5) * 9) : 5;
    if (tool === 'pen' || tool === 'hl') {
      drag = { kind: 'stroke', pid: e.pointerId, sent: 0, liveAt: performance.now(), item: { id: newId(), k: 's', c: color, w: size, hl: tool === 'hl' ? 1 : 0, pts: [Math.round(p.x), Math.round(p.y), pr] } };
    } else if (tool === 'eraser') {
      drag = { kind: 'erase', pid: e.pointerId, ids: new Set() };
      eraseAt(p);
    } else if (SHAPE[tool]) {
      drag = { kind: 'shape', pid: e.pointerId, item: { id: newId(), k: SHAPE[tool], c: color, w: Math.max(2, size), x: Math.round(p.x), y: Math.round(p.y), x2: Math.round(p.x), y2: Math.round(p.y) } };
    } else if (tool === 'text') {
      openEditor({ kind: 't', x: Math.round(p.x), y: Math.round(p.y - TEXT_SIZES[size] * 0.6), sz: TEXT_SIZES[size], c: color, text: '' });
    } else if (tool === 'sticky') {
      openEditor({ kind: 'n', x: Math.round(Math.min(BW - STICKY_W, Math.max(0, p.x - STICKY_W / 2))), y: Math.round(Math.min(BH - STICKY_H, Math.max(0, p.y - STICKY_H / 2))), c: sticky, text: '' });
    } else if (tool === 'select') {
      const hit = topHit(p.x, p.y);
      if (hit) {
        if (e.shiftKey) {
          if (selected.has(hit.id)) selected.delete(hit.id);
          else selected.add(hit.id);
        } else if (!selected.has(hit.id)) selected = new Set([hit.id]);
        if (selected.has(hit.id)) {
          drag = { kind: 'move', pid: e.pointerId, x0: p.x, y0: p.y, dx: 0, dy: 0, moved: false };
          baseDirty = true;
        }
      } else {
        if (!e.shiftKey) selected.clear();
        drag = { kind: 'marquee', pid: e.pointerId, x0: p.x, y0: p.y, x1: p.x, y1: p.y };
      }
    }
    paint();
  }
  function eraseAt(p) {
    const hit = topHit(p.x, p.y, 12 / scale);
    if (hit && !drag.ids.has(hit.id)) {
      drag.ids.add(hit.id);
      hidden.add(hit.id);
      baseDirty = true;
    }
  }
  function pointerMove(e) {
    const p = toBoard(e);
    const now = performance.now();
    if (cur && now - lastCurSent > CUR_MS) {
      lastCurSent = now;
      api.send({ t: 'wb.cur', b: cur, x: Math.round(p.x), y: Math.round(p.y) });
    }
    if (!drag || drag.pid !== e.pointerId) return;
    if (drag.kind === 'stroke') {
      const evs = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
      const pts = drag.item.pts;
      for (const ev of evs.length ? evs : [e]) {
        const q2 = toBoard(ev);
        const lx = pts[pts.length - 3], ly = pts[pts.length - 2];
        if (Math.hypot(q2.x - lx, q2.y - ly) < 1.5) continue;
        const pr = ev.pointerType === 'pen' ? Math.round(Math.max(0.05, ev.pressure || 0.5) * 9) : 5;
        pts.push(Math.round(q2.x), Math.round(q2.y), pr);
      }
      if (now - drag.liveAt > LIVE_MS) flushLive(false);
    } else if (drag.kind === 'erase') {
      eraseAt(p);
    } else if (drag.kind === 'shape') {
      drag.item.x2 = Math.round(Math.min(BW, Math.max(0, p.x)));
      drag.item.y2 = Math.round(Math.min(BH, Math.max(0, p.y)));
      if (e.shiftKey && drag.item.k !== 'a') {
        const d = Math.max(Math.abs(drag.item.x2 - drag.item.x), Math.abs(drag.item.y2 - drag.item.y));
        drag.item.x2 = drag.item.x + Math.sign(drag.item.x2 - drag.item.x || 1) * d;
        drag.item.y2 = drag.item.y + Math.sign(drag.item.y2 - drag.item.y || 1) * d;
      }
    } else if (drag.kind === 'move') {
      drag.dx = Math.round(p.x - drag.x0);
      drag.dy = Math.round(p.y - drag.y0);
      if (Math.abs(drag.dx) + Math.abs(drag.dy) > 2) drag.moved = true;
    } else if (drag.kind === 'marquee') {
      drag.x1 = p.x;
      drag.y1 = p.y;
    }
    paint();
  }
  function pointerUp(e) {
    if (!drag || drag.pid !== e.pointerId) return;
    const d = drag;
    if (d.kind === 'stroke') {
      flushLive(true);
      drag = null;
      if (e.type !== 'pointercancel' || d.item.pts.length > 3) {
        const pts = simplify(d.item.pts, 0.8);
        const parts = splitPoints(pts);
        const items = parts.map((p, i) => ({ ...d.item, id: i ? newId() : d.item.id, pts: p }));
        const first = addItems([items[0]]);
        const entry = first && undo[undo.length - 1];
        for (const extra of items.slice(1)) {
          const res = applyOp(board, { o: 'add', items: [extra] }, api.me?.id);
          if (!res.ok) continue;
          sendOp(res.op);
          if (entry) {
            entry.undo = { o: 'del', ids: [...entry.undo.ids, extra.id] };
            entry.redo = { o: 'add', items: [...entry.redo.items, ...res.op.items] };
          }
        }
        touched();
      }
    } else if (d.kind === 'erase') {
      drag = null;
      const ids = [...d.ids];
      hidden = new Set();
      if (ids.length) delItems(ids);
      else touched();
    } else if (d.kind === 'shape') {
      drag = null;
      const it = d.item;
      if (Math.hypot(it.x2 - it.x, it.y2 - it.y) > 4) addItems([it]);
    } else if (d.kind === 'move') {
      drag = null;
      if (d.moved && (d.dx || d.dy)) moveItems([...selected], d.dx, d.dy);
      else touched();
    } else if (d.kind === 'marquee') {
      drag = null;
      const x0 = Math.min(d.x0, d.x1), x1 = Math.max(d.x0, d.x1), y0 = Math.min(d.y0, d.y1), y1 = Math.max(d.y0, d.y1);
      if (x1 - x0 > 3 || y1 - y0 > 3) {
        for (const it of itemsInOrder()) {
          const b = bounds(it);
          if (b.x0 < x1 && b.x1 > x0 && b.y0 < y1 && b.y1 > y0) selected.add(it.id);
        }
      }
    }
    try { topCv.releasePointerCapture(e.pointerId); } catch {}
    paint();
  }
  topCv.addEventListener('pointerdown', pointerDown);
  topCv.addEventListener('pointermove', pointerMove);
  topCv.addEventListener('pointerup', pointerUp);
  topCv.addEventListener('pointercancel', pointerUp);
  topCv.addEventListener('dblclick', (e) => {
    if (!canEditNow) return;
    const p = toBoard(e);
    const hit = topHit(p.x, p.y);
    if (hit && (hit.k === 't' || hit.k === 'n')) openEditor({ kind: hit.k, id: hit.id, x: hit.x, y: hit.y, sz: hit.sz, c: hit.c, text: hit.text });
  });

  function openEditor(spec) {
    commitEditor();
    const el = document.createElement(spec.kind === 'n' ? 'textarea' : 'input');
    el.className = `wb-editor wb-editor-${spec.kind}`;
    el.value = spec.text || '';
    el.maxLength = 280;
    el.placeholder = spec.kind === 'n' ? 'Write a note' : 'Type, then Enter';
    editor = { ...spec, el, done: false };
    if (spec.id) hidden.add(spec.id);
    baseDirty = true;
    sheet.append(el);
    placeEditor();
    el.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { e.preventDefault(); cancelEditor(); }
      else if (e.key === 'Enter' && (spec.kind === 't' || e.metaKey || e.ctrlKey)) { e.preventDefault(); commitEditor(); }
    });
    el.addEventListener('blur', () => setTimeout(commitEditor, 0));
    setTimeout(() => el.focus(), 0);
    paint();
  }
  function placeEditor() {
    const ed = editor, s = scale;
    ed.el.style.left = `${ed.x * s}px`;
    ed.el.style.top = `${ed.y * s}px`;
    if (ed.kind === 'n') {
      ed.el.style.width = `${STICKY_W * s}px`;
      ed.el.style.height = `${STICKY_H * s}px`;
      ed.el.style.background = ed.c;
      ed.el.style.fontSize = `${17 * s}px`;
      ed.el.style.padding = `${12 * s}px`;
    } else {
      ed.el.style.fontSize = `${ed.sz * s}px`;
      ed.el.style.color = ed.c;
      ed.el.style.minWidth = `${Math.max(80, 240 * s)}px`;
    }
  }
  function closeEditor() {
    const ed = editor;
    editor = null;
    if (!ed) return null;
    ed.done = true;
    if (ed.id) hidden.delete(ed.id);
    ed.el.remove();
    baseDirty = true;
    paint();
    return ed;
  }
  function cancelEditor() {
    closeEditor();
  }
  function commitEditor() {
    if (!editor || editor.done) return;
    const ed = closeEditor();
    const text = ed.el.value;
    if (!canEditNow) return;
    if (ed.id) {
      const it = board.items.get(ed.id);
      if (!it || it.text === text) return;
      if (ed.kind === 't' && !text.trim()) return delItems([ed.id]);
      return updText(ed.id, text);
    }
    if (ed.kind === 't' && !text.trim()) return;
    if (ed.kind === 't') addItems([{ id: newId(), k: 't', x: ed.x, y: ed.y, sz: ed.sz, c: ed.c, text }]);
    else addItems([{ id: newId(), k: 'n', x: ed.x, y: ed.y, c: ed.c, text }]);
  }

  root.addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b || !root.contains(b)) return;
    if (b.dataset.tool) return setTool(b.dataset.tool);
    if (b.dataset.size) { size = +b.dataset.size; return renderBar(); }
    if (b.dataset.color) {
      if (tool === 'sticky') sticky = b.dataset.color;
      else color = b.dataset.color;
      return renderBar();
    }
    const act = b.dataset.act;
    if (act === 'close') close();
    else if (act === 'undo') step(undo, redo);
    else if (act === 'redo') step(redo, undo);
    else if (act === 'export') exportPng();
    else if (act === 'clear' && canEditNow) q('.wb-confirm').hidden = false;
    else if (act === 'cancel-clear') q('.wb-confirm').hidden = true;
    else if (act === 'do-clear') {
      q('.wb-confirm').hidden = true;
      const res = applyOp(board, { o: 'clear' });
      if (res.ok) sendOp(res.op);
      undo = [];
      redo = [];
      selected.clear();
      touched();
      renderBar();
    }
  });
  root.addEventListener('pointerdown', (e) => { if (e.target === root) close(); });
  root.addEventListener('keydown', (e) => e.stopPropagation());
  root.addEventListener('keyup', (e) => e.stopPropagation());

  addEventListener('keydown', (e) => {
    if (root.hidden) return;
    if (!(e.target === document.body || e.target === document.documentElement || root.contains(e.target))) return;
    if (e.target.closest?.('.wb-editor')) return;
    e.stopPropagation();
    const mod = e.metaKey || e.ctrlKey;
    const k = e.key.toLowerCase();
    if (e.key === 'Escape') {
      e.preventDefault();
      if (!q('.wb-confirm').hidden) q('.wb-confirm').hidden = true;
      else if (selected.size) { selected.clear(); paint(); }
      else close();
      return;
    }
    if (mod && k === 'z') { e.preventDefault(); return e.shiftKey ? step(redo, undo) : step(undo, redo); }
    if (mod && k === 'y') { e.preventDefault(); return step(redo, undo); }
    if (mod) return;
    if ((e.key === 'Delete' || e.key === 'Backspace') && selected.size && canEditNow) { e.preventDefault(); return delItems([...selected]); }
    const t = TOOLS.find(([, , key]) => key === k);
    if (t) { e.preventDefault(); setTool(t[0]); }
  }, true);

  function exportPng() {
    const c = renderPng(itemsInOrder());
    c.toBlob((blob) => {
      if (!blob) return;
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `${roomName(cur).toLowerCase().replace(/\s+/g, '-')}-whiteboard-${new Date().toISOString().slice(0, 10)}.png`;
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 2000);
    }, 'image/png');
  }

  function open(b) {
    if (!boards.includes(b) && !parseBoardId(b)) return;
    if (cur && cur !== b) close();
    cur = b;
    board = newBoard();
    sorted = null;
    undo = [];
    redo = [];
    selected.clear();
    hidden = new Set();
    cursors.clear();
    lives.clear();
    loading = true;
    lastPresence = '';
    canEditNow = editable();
    q('.wb-title').textContent = `${roomName(b)} whiteboard`;
    q('.wb-loading').hidden = false;
    q('.wb-confirm').hidden = true;
    root.hidden = false;
    renderBar();
    renderPresence();
    layout();
    api.send({ t: 'wb.open', b });
  }
  function close() {
    if (!cur) return;
    commitEditor();
    api.send({ t: 'wb.close' });
    cur = null;
    drag = null;
    api.setNote(q('.wb-note'), null);
    root.hidden = true;
  }
  function resync() {
    if (cur) api.send({ t: 'wb.open', b: cur });
  }
  const editable = () => !!cur && !!api.me && api.zoneAt(api.me.x, api.me.y) === zoneOf(cur);

  function setThumb(b, th) {
    const c = document.createElement('canvas');
    c.width = 104;
    c.height = 64;
    const g = c.getContext('2d');
    g.fillStyle = '#ffffff';
    g.fillRect(0, 0, c.width, c.height);
    drawThumb(g, th, c.width, c.height);
    thumbs.set(b, th.length ? c : null);
  }

  api.onObject('H', (x, y) => open(boardId(x, y)));
  api.onInit((m, again) => {
    if (m.wb) {
      boards = m.wb.boards || [];
      for (const [b, th] of Object.entries(m.wb.thumbs || {})) setThumb(b, th);
    }
    if (again && cur) {
      cursors.clear();
      lives.clear();
      resync();
    }
  });
  api.onMessage('wb.state', (m) => {
    if (m.b !== cur) return;
    if (m.part === 0) {
      board = newBoard();
      hidden = new Set();
    }
    for (const it of m.items) {
      board.items.set(it.id, it);
      board.z = Math.max(board.z, it.z || 0);
    }
    for (const id of [...selected]) if (!board.items.has(id)) selected.delete(id);
    if (m.done) {
      loading = false;
      q('.wb-loading').hidden = true;
    }
    touched();
  });
  api.onMessage('wb.deny', (m) => {
    if (m.b !== cur) return;
    close();
    api.noticeError("Can't open this whiteboard", m.reason || 'You need access to this room.');
  });
  api.onMessage('wb.op', (m) => {
    if (m.b !== cur) return;
    const op = m.op;
    if (m.by === api.me?.id) {
      if (op.o === 'add') {
        for (const it of op.items) {
          const loc = board.items.get(it.id);
          if (loc) { loc.z = it.z; loc.by = it.by; }
          board.z = Math.max(board.z, it.z || 0);
        }
        touched();
      }
      return;
    }
    if (op.o === 'add') {
      for (const it of op.items) {
        board.items.set(it.id, it);
        board.z = Math.max(board.z, it.z || 0);
        lives.delete(`${m.by}:${it.id}`);
      }
    } else {
      const res = applyOp(board, op);
      for (const id of res.del) selected.delete(id);
      if (op.o === 'clear') {
        undo = [];
        redo = [];
        renderBar();
      }
    }
    touched();
  });
  api.onMessage('wb.nack', (m) => {
    if (m.b !== cur) return;
    if (m.err === 'noop') return;
    if (m.err === 'full') api.setNote(q('.wb-note'), { title: 'This whiteboard is full', body: 'Erase something or clear the board to keep drawing.', tone: 'error', dismiss: true });
    else if (m.err === 'rate') api.setNote(q('.wb-note'), { title: 'Slow down a little', body: 'Some of your last changes did not reach the board.', tone: 'error', dismiss: true });
    else if (m.err === 'view') { canEditNow = false; renderBar(); }
    resync();
  });
  api.onMessage('wb.cur', (m) => {
    if (m.b !== cur) return;
    cursors.set(m.id, { x: m.x, y: m.y, t: performance.now() });
    paint();
  });
  api.onMessage('wb.live', (m) => {
    if (m.b !== cur) return;
    const k = `${m.by}:${m.id}`;
    const l = lives.get(k) || { k: 's', c: m.c, w: m.w, hl: m.hl, pts: [] };
    const add = l.pts.length && m.pts.length >= 3 && m.pts[0] === l.pts[l.pts.length - 3] && m.pts[1] === l.pts[l.pts.length - 2] ? m.pts.slice(3) : m.pts;
    l.pts = l.pts.concat(add).slice(-6000);
    l.t = performance.now();
    if (board.items.has(m.id)) lives.delete(k);
    else lives.set(k, l);
    paint();
  });
  api.onMessage('wb.thumb', (m) => setThumb(m.b, m.th || []));
  api.onExtra((o, k) => {
    if (k === 'wb') renderPresence();
  });

  let lastTick = 0;
  api.onTick((now) => {
    if (!cur || now - lastTick < 300) return;
    lastTick = now;
    const ed = editable();
    if (ed !== canEditNow) {
      canEditNow = ed;
      if (!ed) {
        commitEditor();
        drag = null;
        selected.clear();
      }
      renderBar();
      paint();
    }
    renderPresence();
  });

  api.onDrawUnder((ctx) => {
    const T = api.T;
    for (const [b, c] of thumbs) {
      if (!c) continue;
      const p = parseBoardId(b);
      if (!p) continue;
      const x = p.x * T + 3, y = p.y * T - 9, w = 26, h = 24;
      ctx.fillStyle = '#ffffff';
      ctx.fillRect(x, y, w, h);
      ctx.imageSmoothingEnabled = true;
      ctx.drawImage(c, x, y + 4, w, 16);
      ctx.imageSmoothingEnabled = false;
    }
  });

  api.onDrawScreen((ctx, o, sx, above) => {
    const wb = o.ex?.wb;
    if (!wb) return above;
    const label = wb.d ? 'Drawing' : 'At whiteboard';
    ctx.save();
    ctx.font = `600 10.5px ${FONT}`;
    const w = Math.round(ctx.measureText(label).width + 26), h = 17;
    const x = Math.round(sx - w / 2), y = Math.round(above - h);
    ctx.fillStyle = wb.d ? '#3b45e0' : 'rgba(29,32,48,.82)';
    ctx.beginPath();
    ctx.roundRect(x, y, w, h, 8);
    ctx.fill();
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 1.3;
    ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.moveTo(x + 7, y + 12);
    ctx.lineTo(x + 13, y + 6);
    ctx.stroke();
    ctx.fillStyle = '#fff';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    ctx.fillText(label, x + 17, y + h / 2 + 0.5);
    ctx.restore();
    return y - 4;
  });

  api.personBadge((o) => (o.ex?.wb ? html` <span class="wb-badge" title="${o.ex.wb.d ? 'Drawing on' : 'Viewing'} the ${(roomName(o.ex.wb.b))} whiteboard">${svg('pen', 11)}</span>` : ''));
  api.paletteItems(() => boards.map((b) => ({ ic: svg('board', 14), label: `${roomName(b)} whiteboard`, sub: 'Open whiteboard', run: () => open(b) })));
  api.personActions((o) => {
    const wb = o.ex?.wb;
    if (!wb || o === api.me) return [];
    return [{ tip: `Open ${roomName(wb.b)} whiteboard`, icon: svg('board', 14), run: () => open(wb.b) }];
  });
  api.onLeave(() => close());
  window.officeWhiteboard = { open, close, get board() { return board; }, get current() { return cur; } };
}
