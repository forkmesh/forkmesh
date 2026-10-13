export const BW = 1600;
export const BH = 1000;
export const MAX_ITEMS = 4000;
export const MAX_BYTES = 1500000;
export const MAX_PTS = 2700;
export const MAX_TEXT = 280;
export const MAX_BATCH = 60;
export const MAX_IDS = 600;
export const STICKY_W = 180;
export const STICKY_H = 130;
export const KINDS = new Set(['s', 't', 'n', 'r', 'e', 'a']);
export const COLORS = ['#1d2030', '#e5484d', '#f59e0b', '#22a35a', '#3b45e0', '#8e4ec6', '#ffffff'];
export const STICKY_COLORS = ['#fff3a3', '#ffd6e0', '#cdeffd', '#d3f5d0', '#e8dcff'];

const ID_RE = /^[A-Za-z0-9_-]{1,32}$/;
const HEX_RE = /^#[0-9a-fA-F]{6}$/;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const num = (v, lo, hi) => (Number.isFinite(+v) ? clamp(Math.round(+v), lo, hi) : null);
const cleanText = (s) => String(s ?? '').replace(/[\u0000-\u0009\u000b-\u001f\u007f]/g, '').slice(0, MAX_TEXT);

export function boardId(x, y) {
  return `${x}.${y}`;
}
export function parseBoardId(b) {
  const m = /^(\d{1,3})\.(\d{1,3})$/.exec(String(b ?? ''));
  return m ? { x: +m[1], y: +m[2] } : null;
}
export function findBoards(obj) {
  const out = [];
  for (let y = 0; y < obj.length; y++) for (let x = 0; x < obj[y].length; x++) if (obj[y][x] === 'H') out.push(boardId(x, y));
  return out;
}

export function sanitizeItem(it) {
  if (!it || typeof it !== 'object' || !KINDS.has(it.k) || !ID_RE.test(String(it.id ?? ''))) return null;
  const c = HEX_RE.test(it.c) ? it.c.toLowerCase() : COLORS[0];
  const out = { id: String(it.id), k: it.k, c };
  if (it.k === 's') {
    if (!Array.isArray(it.pts) || it.pts.length < 3 || it.pts.length > MAX_PTS) return null;
    const pts = [];
    for (let i = 0; i + 2 < it.pts.length; i += 3) {
      const x = num(it.pts[i], -50, BW + 50), y = num(it.pts[i + 1], -50, BH + 50), p = num(it.pts[i + 2], 0, 9);
      if (x === null || y === null || p === null) return null;
      pts.push(x, y, p);
    }
    if (!pts.length) return null;
    out.w = num(it.w, 1, 60) ?? 4;
    out.pts = pts;
    if (it.hl) out.hl = 1;
    return out;
  }
  const x = num(it.x, 0, BW), y = num(it.y, 0, BH);
  if (x === null || y === null) return null;
  out.x = x;
  out.y = y;
  if (it.k === 't') {
    const text = cleanText(it.text).trim();
    if (!text) return null;
    out.text = text;
    out.sz = num(it.sz, 10, 96) ?? 24;
    return out;
  }
  if (it.k === 'n') {
    out.text = cleanText(it.text);
    return out;
  }
  const x2 = num(it.x2, 0, BW), y2 = num(it.y2, 0, BH);
  if (x2 === null || y2 === null) return null;
  out.x2 = x2;
  out.y2 = y2;
  out.w = num(it.w, 1, 40) ?? 4;
  return out;
}

export function itemBytes(it) {
  return JSON.stringify(it).length;
}

export function newBoard() {
  return { items: new Map(), z: 0, bytes: 0 };
}

export function loadBoard(list) {
  const b = newBoard();
  const sorted = [...list].sort((a, c) => (a.z || 0) - (c.z || 0));
  for (const it of sorted) {
    if (!it || !it.id) continue;
    b.items.set(it.id, it);
    b.bytes += itemBytes(it);
    b.z = Math.max(b.z, it.z || 0);
  }
  return b;
}

export function boardItems(b) {
  return [...b.items.values()];
}

export function applyOp(board, op, by) {
  const res = { ok: false, put: [], del: [], op: null, err: null };
  if (!op || typeof op !== 'object') return (res.err = 'bad', res);
  switch (op.o) {
    case 'add': {
      if (!Array.isArray(op.items) || !op.items.length || op.items.length > MAX_BATCH) return (res.err = 'bad', res);
      const added = [];
      for (const raw of op.items) {
        const it = sanitizeItem(raw);
        if (!it || board.items.has(it.id)) continue;
        const size = itemBytes(it) + 24;
        if (board.items.size >= MAX_ITEMS || board.bytes + size > MAX_BYTES) { res.err = 'full'; break; }
        it.z = ++board.z;
        if (by) it.by = String(by).slice(0, 12);
        board.items.set(it.id, it);
        board.bytes += itemBytes(it);
        added.push(it);
      }
      if (!added.length) return res;
      res.put = added;
      res.op = { o: 'add', items: added };
      break;
    }
    case 'del': {
      if (!Array.isArray(op.ids) || op.ids.length > MAX_IDS) return (res.err = 'bad', res);
      const ids = [];
      for (const id of op.ids) {
        const it = board.items.get(String(id));
        if (!it) continue;
        board.items.delete(it.id);
        board.bytes -= itemBytes(it);
        ids.push(it.id);
      }
      if (!ids.length) return res;
      res.del = ids;
      res.op = { o: 'del', ids };
      break;
    }
    case 'mv': {
      const dx = num(op.dx, -BW, BW), dy = num(op.dy, -BH, BH);
      if (!Array.isArray(op.ids) || op.ids.length > MAX_IDS || dx === null || dy === null || (!dx && !dy)) return (res.err = 'bad', res);
      const ids = [];
      for (const id of op.ids) {
        const it = board.items.get(String(id));
        if (!it) continue;
        const before = itemBytes(it);
        moveItem(it, dx, dy);
        board.bytes += itemBytes(it) - before;
        ids.push(it.id);
        res.put.push(it);
      }
      if (!ids.length) return res;
      res.op = { o: 'mv', ids, dx, dy };
      break;
    }
    case 'upd': {
      const it = board.items.get(String(op.id ?? ''));
      if (!it || (it.k !== 't' && it.k !== 'n')) return res;
      const text = cleanText(op.text);
      if (it.k === 't' && !text.trim()) return res;
      const before = itemBytes(it);
      it.text = it.k === 't' ? text.trim() : text;
      board.bytes += itemBytes(it) - before;
      res.put = [it];
      res.op = { o: 'upd', id: it.id, text: it.text };
      break;
    }
    case 'clear': {
      res.del = [...board.items.keys()];
      board.items.clear();
      board.bytes = 0;
      res.op = { o: 'clear' };
      break;
    }
    default:
      res.err = 'bad';
      return res;
  }
  res.ok = true;
  return res;
}

export function moveItem(it, dx, dy) {
  if (it.k === 's') {
    for (let i = 0; i < it.pts.length; i += 3) {
      it.pts[i] = clamp(it.pts[i] + dx, -50, BW + 50);
      it.pts[i + 1] = clamp(it.pts[i + 1] + dy, -50, BH + 50);
    }
    return;
  }
  it.x = clamp(it.x + dx, 0, BW);
  it.y = clamp(it.y + dy, 0, BH);
  if (it.x2 !== undefined) {
    it.x2 = clamp(it.x2 + dx, 0, BW);
    it.y2 = clamp(it.y2 + dy, 0, BH);
  }
}

export function textWidth(it) {
  return Math.max(20, it.text.length * it.sz * 0.56);
}

export function bounds(it) {
  if (it.k === 's') {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (let i = 0; i < it.pts.length; i += 3) {
      x0 = Math.min(x0, it.pts[i]); y0 = Math.min(y0, it.pts[i + 1]);
      x1 = Math.max(x1, it.pts[i]); y1 = Math.max(y1, it.pts[i + 1]);
    }
    const pad = (it.hl ? it.w * 3 : it.w) / 2 + 2;
    return { x0: x0 - pad, y0: y0 - pad, x1: x1 + pad, y1: y1 + pad };
  }
  if (it.k === 't') return { x0: it.x, y0: it.y, x1: it.x + textWidth(it), y1: it.y + it.sz * 1.25 };
  if (it.k === 'n') return { x0: it.x, y0: it.y, x1: it.x + STICKY_W, y1: it.y + STICKY_H };
  const pad = it.w / 2 + 4;
  return { x0: Math.min(it.x, it.x2) - pad, y0: Math.min(it.y, it.y2) - pad, x1: Math.max(it.x, it.x2) + pad, y1: Math.max(it.y, it.y2) + pad };
}

function segDist(px, py, ax, ay, bx, by) {
  const dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
  const t = l ? clamp(((px - ax) * dx + (py - ay) * dy) / l, 0, 1) : 0;
  return Math.hypot(px - (ax + t * dx), py - (ay + t * dy));
}

export function hitTest(it, x, y, r = 6) {
  const b = bounds(it);
  if (x < b.x0 - r || x > b.x1 + r || y < b.y0 - r || y > b.y1 + r) return false;
  if (it.k === 's') {
    const tol = r + (it.hl ? it.w * 1.5 : it.w / 2);
    if (it.pts.length === 3) return Math.hypot(x - it.pts[0], y - it.pts[1]) <= tol;
    for (let i = 3; i < it.pts.length; i += 3) if (segDist(x, y, it.pts[i - 3], it.pts[i - 2], it.pts[i], it.pts[i + 1]) <= tol) return true;
    return false;
  }
  if (it.k === 'a') return segDist(x, y, it.x, it.y, it.x2, it.y2) <= r + it.w / 2 + 2;
  if (it.k === 'r' || it.k === 'e') {
    const tol = r + it.w / 2 + 2;
    const x0 = Math.min(it.x, it.x2), x1 = Math.max(it.x, it.x2), y0 = Math.min(it.y, it.y2), y1 = Math.max(it.y, it.y2);
    if (it.k === 'r') {
      return segDist(x, y, x0, y0, x1, y0) <= tol || segDist(x, y, x1, y0, x1, y1) <= tol || segDist(x, y, x1, y1, x0, y1) <= tol || segDist(x, y, x0, y1, x0, y0) <= tol;
    }
    const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = Math.max(1, (x1 - x0) / 2), ry = Math.max(1, (y1 - y0) / 2);
    const d = Math.hypot((x - cx) / rx, (y - cy) / ry);
    return Math.abs(d - 1) * Math.min(rx, ry) <= tol;
  }
  return true;
}

export function simplify(pts, eps = 1.2) {
  const n = pts.length / 3;
  if (n <= 2) return pts.slice();
  const keep = new Uint8Array(n);
  keep[0] = keep[n - 1] = 1;
  const stack = [[0, n - 1]];
  while (stack.length) {
    const [a, b] = stack.pop();
    let best = -1, bi = -1;
    for (let i = a + 1; i < b; i++) {
      const d = segDist(pts[i * 3], pts[i * 3 + 1], pts[a * 3], pts[a * 3 + 1], pts[b * 3], pts[b * 3 + 1]) + Math.abs(pts[i * 3 + 2] - pts[a * 3 + 2]) * 0.4;
      if (d > best) { best = d; bi = i; }
    }
    if (best > eps) {
      keep[bi] = 1;
      stack.push([a, bi], [bi, b]);
    }
  }
  const out = [];
  for (let i = 0; i < n; i++) if (keep[i]) out.push(pts[i * 3], pts[i * 3 + 1], pts[i * 3 + 2]);
  return out;
}

export function splitPoints(pts, max = MAX_PTS - 3) {
  if (pts.length <= max) return [pts];
  const out = [];
  let i = 0;
  while (i < pts.length) {
    const start = out.length ? i - 3 : i;
    out.push(pts.slice(start, start + max));
    i = start + max;
  }
  return out.filter((p) => p.length >= 3);
}

export function thumbOf(board, limit = 160) {
  const items = boardItems(board).slice(-limit);
  const q = (v, s) => clamp(Math.round((v / s) * 63), 0, 63);
  const out = [];
  for (const it of items) {
    if (it.k === 's') {
      const pts = simplify(it.pts, 12);
      const flat = [];
      const step = Math.max(1, Math.ceil(pts.length / 3 / 12));
      for (let i = 0; i < pts.length; i += 3 * step) flat.push(q(pts[i], BW), q(pts[i + 1], BH));
      flat.push(q(pts[pts.length - 3], BW), q(pts[pts.length - 2], BH));
      out.push(['s', it.c, flat, it.hl ? 1 : 0]);
    } else if (it.k === 't') {
      const b = bounds(it);
      out.push(['t', it.c, [q(b.x0, BW), q(b.y0, BH), q(b.x1, BW), q(b.y1, BH)]]);
    } else if (it.k === 'n') {
      out.push(['n', it.c, [q(it.x, BW), q(it.y, BH), q(it.x + STICKY_W, BW), q(it.y + STICKY_H, BH)]]);
    } else {
      out.push([it.k, it.c, [q(it.x, BW), q(it.y, BH), q(it.x2, BW), q(it.y2, BH)]]);
    }
  }
  return out;
}

export function chunkItems(items, maxChars = 200000) {
  const chunks = [];
  let cur = [], size = 0;
  for (const it of items) {
    const s = itemBytes(it);
    if (cur.length && size + s > maxChars) {
      chunks.push(cur);
      cur = [];
      size = 0;
    }
    cur.push(it);
    size += s;
  }
  if (cur.length || !chunks.length) chunks.push(cur);
  return chunks;
}

export function bucket(state, key, cap, rate, cost = 1, now = Date.now()) {
  const b = state[key] || (state[key] = { t: cap, at: now });
  b.t = Math.min(cap, b.t + ((now - b.at) / 1000) * rate);
  b.at = now;
  if (b.t < cost) return false;
  b.t -= cost;
  return true;
}
