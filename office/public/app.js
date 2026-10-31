import { TILE as T, W, H, ZONES, ROOM_ANCHORS, LABELS, SCENES, sceneAt, isWalkable, zoneAt, chairDir, obj } from './map.js';
import { buildMapCanvas, buildOutskirts, OUTSKIRTS, nameTag, roomTag, drawLabels, COLORS } from './render.js';
import { avatarSprite, palette, SPRITE_W, SPRITE_FOOT } from './sprite.js';
import { Mesh } from './rtc.js';
import { createCallQualityMeter, mountCallQualityWidget, rtcStatsReader } from '../../app/public/call-quality-meter.js';
import { icon, hydrate } from './icons.js';
import { html, esc, setHtml } from './html.js';
import { setNote, noteLine, NOTE_MS } from './notes.js';
import { FEATURES } from './features/index.js';

const $ = (s) => document.querySelector(s);
const STEP_MS = 140;
const NEAR = 3;
const FAR = 5;
const MAX_PEERS = 8;
const EMOTES = ['👋', '❤️', '🎉', '🤣', '👏', '💃', '👍', '💯', '🔥'];
const KEYS = {
  arrowdown: [0, 1, 0], s: [0, 1, 0],
  arrowup: [0, -1, 1], w: [0, -1, 1],
  arrowleft: [-1, 0, 2], a: [-1, 0, 2],
  arrowright: [1, 0, 3], d: [1, 0, 3],
};

hydrate();
const cv = $('#cv');
const ctx = cv.getContext('2d');
const mm = $('#minimap');
const mapCv = buildMapCanvas();
const outskirtsCv = buildOutskirts();
const EDGE = (OUTSKIRTS - 1) * T;

let ws, me = null, mesh = null, retry = 0, leaving = false;
let zoom = 1.5, ox = 0, oy = 0, camX = 0, camY = 0, camFollow = true;
let path = [], held = [], lastStep = 0, lastProx = 0, peopleDirty = true, onlineOpen = true;
let popKind = null;
const others = new Map();
const local = { audio: null, video: null };
const deviceIds = { audioinput: null, videoinput: null };
let socketSource = null;
const hooks = { place: [], msg: new Map(), init: [], ex: [], tick: [], drawWorld: [], drawScreen: [], dot: [], proximity: [], wsParams: [], joinExtras: [], personBadges: [], personActions: [], palette: [], objects: new Map(), leave: [], peer: [], denied: [], drawUnder: [], person: [], meMenu: [], face: [], tagAlpha: [], emote: [], pose: [] };
const panels = new Set(['people', 'chat']);
const selfCam = Object.assign(document.createElement('video'), { muted: true, playsInline: true, autoplay: true });

const send = (m) => ws?.readyState === 1 && ws.send(JSON.stringify(m));
const unpack = (p) => ({ id: p[0], name: p[1], av: p[2], x: p[3], y: p[4], d: p[5], mic: !!p[6], cam: !!p[7], away: !!p[8], ex: p[9] || {}, rx: p[3], ry: p[4] });
const store = { get: (k) => { try { return localStorage.getItem(k); } catch { return null; } }, set: (k, v) => { try { localStorage.setItem(k, v); } catch {} } };
const initial = (o) => (o.name[0] || '?').toUpperCase();
const zoneName = (o) => ZONES[zoneAt(o.x, o.y)]?.name || '';
function tint(av) {
  const h = palette(av).shirt;
  const n = parseInt(h.slice(1), 16), mix = (v) => Math.round(v + (255 - v) * 0.55);
  return `rgb(${mix(n >> 16)},${mix((n >> 8) & 255)},${mix(n & 255)})`;
}

{
  const p = $('#preview');
  p.width = mapCv.width;
  p.height = mapCv.height;
  const pc = p.getContext('2d');
  pc.drawImage(mapCv, 0, 0);
  drawLabels(pc);
}
$('#join-form').onsubmit = (e) => e.preventDefault();

let connecting = false;
let deniedReason = '';
let bannerTimer = 0;
function showBanner() {
  if (!me || bannerTimer) return;
  bannerTimer = setTimeout(() => { bannerTimer = 0; $('#banner').hidden = false; }, 4000);
}
function hideBanner() {
  clearTimeout(bannerTimer);
  bannerTimer = 0;
  $('#banner').hidden = true;
}
async function connect() {
  if (connecting || leaving || !socketSource) return;
  connecting = true;
  let url = null;
  try {
    url = await socketSource();
  } catch {
    url = undefined;
  } finally {
    connecting = false;
  }
  if (url === null || leaving) return;
  if (url === undefined) {
    showBanner();
    setTimeout(connect, Math.min(10000, 500 * 2 ** retry++));
    return;
  }
  deniedReason = '';
  ws = new WebSocket(url);
  ws.onopen = () => {
    retry = 0;
    const join = { t: 'join', x: me?.x, y: me?.y };
    for (const fn of hooks.joinExtras) Object.assign(join, fn() || {});
    send(join);
  };
  ws.onmessage = (e) => e.data !== 'pong' && onMessage(JSON.parse(e.data));
  ws.onclose = (ev) => {
    if (leaving || ev.code === 4003 || ev.code === 4001) {
      if ((ev.code === 4003 && !deniedReason) || ev.code === 4001) for (const fn of hooks.denied) fn(ev.code === 4001 ? 'You opened the office somewhere else.' : 'Your access to this office ended.');
      return;
    }
    showBanner();
    mesh?.closeAll();
    others.clear();
    peopleDirty = true;
    setTimeout(connect, Math.min(5000, 400 * 2 ** retry++));
  };
}
setInterval(() => ws?.readyState === 1 && ws.send('ping'), 25000);

function onMessage(m) {
  switch (m.t) {
    case 'init': {
      const prev = me;
      me = unpack(m.you);
      me.mic = !!local.audio;
      me.cam = !!local.video;
      me.away = !!prev?.away;
      if (prev) { me.rx = prev.rx; me.ry = prev.ry; }
      else { camX = (me.x + 0.5) * T; camY = (me.y + 0.5) * T; }
      others.clear();
      for (const p of m.peers) others.set(p[0], unpack(p));
      mesh?.closeAll();
      mesh = new Mesh(me.id, (to, d) => send({ t: 'sig', to, d }), { onStream: showStream, onClose: removeTile, iceServers: api.iceServers, refreshIce: () => api.refreshIce?.() });
      mesh.local = { ...local };
      for (const fn of hooks.peer) fn(mesh);
      if (me.mic || me.cam || me.away) sendStatus();
      $('#join').hidden = true;
      $('#app').hidden = false;
      hideBanner();
      renderMe();
      peopleDirty = true;
      for (const fn of hooks.init) fn(m, !!prev);
      return;
    }
    case 'j': {
      const o = unpack(m.p);
      others.set(o.id, o);
      peopleDirty = true;
      return;
    }
    case 'l':
      others.delete(m.id);
      mesh?.close(m.id, false);
      peopleDirty = true;
      return;
    case 'mv':
      for (const [id, x, y, d] of m.u) {
        const o = others.get(id);
        if (!o) continue;
        if (zoneAt(o.x, o.y) !== zoneAt(x, y)) peopleDirty = true;
        o.x = x; o.y = y; o.d = d;
      }
      return;
    case 'you':
      me.x = m.x; me.y = m.y; path = []; pendingUse = null;
      for (const fn of hooks.msg.get('you') || []) fn(m);
      return;
    case 'st': {
      const o = others.get(m.id);
      if (o) { o.mic = m.mic; o.cam = m.cam; o.away = m.away; updateTile(m.id); peopleDirty = true; }
      return;
    }
    case 'emo': {
      const o = others.get(m.id);
      if (o) showEmote(o, m.e);
      return;
    }
    case 'say': {
      const o = others.get(m.id);
      if (o) { o.say = m.text; o.sayUntil = performance.now() + 7000; }
      return;
    }
    case 'wave':
      return hudNote({ key: 'wave', title: `${m.name} waved at you 👋`, body: 'Say hi back, or walk over.', actions: [{ label: 'Walk over', icon: 'walk', primary: true, run: () => walkTo(m.f) }] });
    case 'sig':
      return mesh?.handle(m.f, m.d);
    case 'ex': {
      const o = m.id === me?.id ? me : others.get(m.id);
      if (!o) return;
      if (m.v === null) delete o.ex[m.k];
      else o.ex[m.k] = m.v;
      peopleDirty = true;
      for (const fn of hooks.ex) fn(o, m.k, m.v);
      return;
    }
    case 'denied':
      deniedReason = m.reason || '';
      for (const fn of hooks.denied) fn(m.reason);
      if (!hooks.denied.length) noticeError("Couldn't join the office", m.reason);
      return;
    default:
      for (const fn of hooks.msg.get(m.t) || []) fn(m);
  }
}

const sendStatus = () => send({ t: 'st', mic: me.mic, cam: me.cam, away: me.away });
function showEmote(o, e) {
  o.emo = e;
  o.emoUntil = performance.now() + 3000;
  for (const fn of hooks.emote) fn(o, e);
}
function emote(e) {
  if (!me || !EMOTES.includes(e)) return;
  showEmote(me, e);
  send({ t: 'emo', e });
}

function step(dx, dy, dir) {
  const nx = me.x + dx, ny = me.y + dy;
  const moved = isWalkable(nx, ny);
  if (moved) { me.x = nx; me.y = ny; }
  const sit = chairDir(me.x, me.y);
  const nd = moved && sit !== undefined ? sit : dir;
  if (moved || nd !== me.d) {
    me.d = nd;
    send({ t: 'm', x: me.x, y: me.y, d: me.d });
  }
  if (moved && zoneAt(me.x - dx, me.y - dy) !== zoneAt(me.x, me.y)) peopleDirty = true;
  return moved;
}

function bfs(sx, sy, isGoal) {
  const prev = new Int32Array(W * H).fill(-1);
  const start = sy * W + sx;
  const q = [start];
  prev[start] = start;
  let goal = -1;
  for (let i = 0; i < q.length; i++) {
    const c = q[i], cx = c % W, cy = (c / W) | 0;
    if (isGoal(cx, cy)) { goal = c; break; }
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const nx = cx + dx, ny = cy + dy, n = ny * W + nx;
      if (isWalkable(nx, ny) && prev[n] === -1) { prev[n] = c; q.push(n); }
    }
  }
  if (goal < 0) return [];
  const out = [];
  for (let c = goal; c !== start; c = prev[c]) out.push([c % W, (c / W) | 0]);
  return out.reverse();
}
function walkToTile(tx, ty) {
  camFollow = true;
  path = isWalkable(tx, ty)
    ? bfs(me.x, me.y, (x, y) => x === tx && y === ty)
    : bfs(me.x, me.y, (x, y) => Math.abs(x - tx) + Math.abs(y - ty) <= 1);
}
function walkTo(id) {
  const o = others.get(id);
  if (!o) return;
  walkToTile(o.x, o.y);
  if (path.length > 1) path.pop();
}
function takeSeat() {
  const taken = new Set([...others.values()].map((o) => o.y * W + o.x));
  camFollow = true;
  path = bfs(me.x, me.y, (x, y) => chairDir(x, y) !== undefined && !taken.has(y * W + x));
  if (!path.length && chairDir(me.x, me.y) === undefined) notice('No free seats nearby');
}

addEventListener('keydown', (e) => {
  if (!me) return;
  const mod = e.metaKey || e.ctrlKey;
  if (mod && e.key.toLowerCase() === 'k') { e.preventDefault(); return openPalette(); }
  if (mod && e.key.toLowerCase() === 'f') { e.preventDefault(); showPanel('people', true); return $('#people-q').focus(); }
  if (e.key === 'Escape') { closePop(); closePalette(); }
  if (/^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) || e.target.isContentEditable) {
    if (e.key === 'Escape') e.target.blur();
    return;
  }
  if (e.key === 'Enter') { e.preventDefault(); showPanel('chat', true); $('#chat-input').focus(); return; }
  const k = e.key.toLowerCase();
  if (!KEYS[k] || mod) return;
  e.preventDefault();
  camFollow = true;
  if (!held.includes(k)) held.push(k);
});
addEventListener('keyup', (e) => { held = held.filter((k) => k !== e.key.toLowerCase()); });
addEventListener('blur', () => { held = []; });
document.addEventListener('visibilitychange', () => { held = []; });

let drag = null;
cv.addEventListener('pointerdown', (e) => {
  if (!me || e.button !== 0) return;
  drag = { x: e.clientX, y: e.clientY, cx: camX, cy: camY, moved: false };
  cv.setPointerCapture(e.pointerId);
});
cv.addEventListener('pointermove', (e) => {
  if (!drag) return;
  const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
  if (!drag.moved && Math.hypot(dx, dy) < 5) return;
  drag.moved = true;
  camFollow = false;
  cv.classList.add('dragging');
  camX = drag.cx - dx / zoom;
  camY = drag.cy - dy / zoom;
});
cv.addEventListener('pointerup', (e) => {
  if (!drag) return;
  const wasDrag = drag.moved;
  drag = null;
  cv.classList.remove('dragging');
  if (wasDrag) return;
  const r = cv.getBoundingClientRect();
  const tx = Math.floor((e.clientX - r.left - ox) / zoom / T), ty = Math.floor((e.clientY - r.top - oy) / zoom / T);
  const who = personAt(tx, ty);
  if (who && hooks.person.length) {
    for (const fn of hooks.person) fn(who);
    return;
  }
  const fn = hooks.objects.get(obj[ty]?.[tx]);
  if (fn) return useObject(tx, ty, fn);
  walkToTile(tx, ty);
});
function personAt(tx, ty) {
  if (me.x === tx && me.y === ty) return me;
  for (const o of others.values()) if (o.x === tx && o.y === ty) return o;
  return null;
}
let pendingUse = null;
function useObject(tx, ty, fn) {
  if (Math.abs(me.x - tx) + Math.abs(me.y - ty) <= 1) return fn(tx, ty);
  walkToTile(tx, ty);
  pendingUse = path.length ? { tx, ty, fn } : null;
}
cv.addEventListener('wheel', (e) => {
  e.preventDefault();
  const r = cv.getBoundingClientRect();
  setZoom(zoom - Math.sign(e.deltaY) * 0.25, e.clientX - r.left, e.clientY - r.top);
}, { passive: false });
function setZoom(z, ax, ay) {
  const next = Math.min(3, Math.max(0.75, z));
  if (next === zoom) return;
  if (ax !== undefined) {
    const mx = ax - cv.clientWidth / 2, my = ay - cv.clientHeight / 2;
    camX += mx / zoom - mx / next;
    camY += my / zoom - my / next;
    camFollow = false;
  }
  zoom = next;
}

$('#controls').onclick = (e) => {
  const b = e.target.closest('button');
  if (!b) return;
  if (b.dataset.z) setZoom(zoom + 0.25 * b.dataset.z);
  else if (b.id === 'c-seat') takeSeat();
  else if (b.id === 'c-locate') camFollow = true;
  else if (b.id === 'c-map') { mm.hidden = !mm.hidden; b.classList.toggle('on', !mm.hidden); drawMinimap(); }
  else if (b.id === 'c-more') openPop('keys');
};

function nearby(limit, useHysteresis) {
  const mz = zoneAt(me.x, me.y);
  const list = [];
  for (const o of others.values()) {
    const oz = zoneAt(o.x, o.y);
    const dist = Math.max(Math.abs(o.x - me.x), Math.abs(o.y - me.y));
    let ok = ZONES[mz]?.private || ZONES[oz]?.private
      ? oz === mz
      : dist <= (useHysteresis && mesh.has(o.id) ? FAR : NEAR);
    for (const fn of hooks.proximity) {
      const r = fn(o, ok, dist);
      if (r === true || r === false) ok = r;
    }
    if (ok) list.push([dist, o.id]);
  }
  list.sort((a, b) => a[0] - b[0]);
  return list.slice(0, limit).map((x) => x[1]);
}

let lastT = performance.now();
const all = [];
const vis = [];
const byDepth = (a, b) => a.ry - b.ry;
const WALK = [1, 0, 2, 0];
function frame(now) {
  requestAnimationFrame(frame);
  if (!me) return;
  const dt = Math.min(100, now - lastT);
  lastT = now;
  tick(now);

  const dpr = devicePixelRatio || 1;
  const cw = cv.clientWidth, ch = cv.clientHeight;
  if (cv.width !== Math.round(cw * dpr) || cv.height !== Math.round(ch * dpr)) {
    cv.width = Math.round(cw * dpr);
    cv.height = Math.round(ch * dpr);
  }

  all.length = 0;
  all.push(me);
  for (const o of others.values()) all.push(o);
  for (const o of all) approach(o, dt);
  for (const fn of hooks.place) for (const o of all) fn(o, now);

  if (camFollow) {
    const k = Math.min(1, dt / 90);
    camX += ((me.rx + 0.5) * T - camX) * k;
    camY += ((me.ry + 0.5) * T - camY) * k;
  }
  const sc = sceneAt(me.rx + 0.5);
  const bx0 = sc ? sc.x0 * T : 0, bx1 = sc ? sc.x1 * T : W * T;
  const mw = (bx1 - bx0) * zoom, mh = H * T * zoom;
  const hw = cw / 2 / zoom, hh = ch / 2 / zoom;
  camX = mw + 2 * EDGE * zoom <= cw ? (bx0 + bx1) / 2 : Math.min(bx1 + EDGE - hw, Math.max(bx0 + hw - EDGE, camX));
  camY = mh + 2 * EDGE * zoom <= ch ? (H * T) / 2 : Math.min(H * T + EDGE - hh, Math.max(hh - EDGE, camY));
  ox = Math.round((cw / 2 - camX * zoom) * dpr) / dpr;
  oy = Math.round((ch / 2 - camY * zoom) * dpr) / dpr;

  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = COLORS.grass;
  ctx.fillRect(0, 0, cv.width, cv.height);
  ctx.imageSmoothingEnabled = false;
  ctx.setTransform(dpr * zoom, 0, 0, dpr * zoom, ox * dpr, oy * dpr);
  for (const s of outskirtsCv) ctx.drawImage(s.cv, s.x, s.y);
  ctx.drawImage(mapCv, 0, 0);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const ls = Math.min(1, zoom);
  ctx.imageSmoothingEnabled = ls < 1;
  for (const l of LABELS) {
    const tag = roomTag(l.text, dpr);
    const lw = tag.cssW * ls, lh = tag.cssH * ls;
    ctx.drawImage(tag, Math.round(ox + l.x * T * zoom - lw / 2), Math.round(oy + l.y * T * zoom - lh / 2), lw, lh);
  }
  ctx.imageSmoothingEnabled = false;
  ctx.setTransform(dpr * zoom, 0, 0, dpr * zoom, ox * dpr, oy * dpr);

  const vx0 = -ox / zoom / T - 2, vy0 = -oy / zoom / T - 2;
  const vx1 = vx0 + cw / zoom / T + 4, vy1 = vy0 + ch / zoom / T + 4;
  vis.length = 0;
  for (const o of all) if (o.rx > vx0 && o.rx < vx1 && o.ry > vy0 && o.ry < vy1) vis.push(o);
  vis.sort(byDepth);

  ctx.fillStyle = 'rgba(59,69,224,.16)';
  ctx.beginPath();
  ctx.ellipse((me.rx + 0.5) * T, (me.ry + 1) * T - 3, 14, 6, 0, 0, Math.PI * 2);
  ctx.fill();
  for (const fn of hooks.drawUnder) fn(ctx, now, vis);

  const walkFrame = WALK[((now / 110) | 0) % 4];
  for (const o of vis) {
    const moving = o.rx !== o.x || o.ry !== o.y;
    let sitting = !moving && chairDir(o.x, o.y) !== undefined;
    let dir = o.d, stepFrame = moving ? walkFrame : 0, pose = 0;
    for (const fn of hooks.pose) {
      const r = fn(o, now, moving, sitting);
      if (r) { dir = r.dir ?? dir; stepFrame = r.step ?? stepFrame; pose = r.pose ?? pose; sitting = r.sit ?? sitting; }
    }
    const spr = avatarSprite(o.av, dir, stepFrame, sitting, pose);
    ctx.drawImage(spr, Math.round(o.rx * T + (T - SPRITE_W) / 2), Math.round(o.ry * T + T - SPRITE_FOOT - 2));
  }
  for (const fn of hooks.drawWorld) fn(ctx, now, vis);

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.textAlign = 'center';
  for (const o of vis) {
    const sx = ox + (o.rx * T + T / 2) * zoom;
    const sy = oy + (o.ry * T + T - SPRITE_FOOT + 10) * zoom;
    let dot = o.away ? '#f59e0b' : '#22c55e';
    for (const fn of hooks.dot) dot = fn(o, dot) || dot;
    let alpha = 1;
    for (const fn of hooks.tagAlpha) alpha = fn(o) ?? alpha;
    ctx.globalAlpha = alpha;
    const tag = nameTag(o === me ? 'You' : o.name, dot, dpr);
    const top = Math.round(sy - tag.cssH - 4);
    ctx.drawImage(tag, Math.round(sx - tag.cssW / 2), top, tag.cssW, tag.cssH);
    let above = top - 6;
    for (const fn of hooks.drawScreen) above = fn(ctx, o, sx, above, now) ?? above;
    const cam = camFor(o);
    if (cam) above = camBubble(cam, sx, above, o === me) - 6;
    if (o.sayUntil > now) above = bubble(o.say, sx, above) - 6;
    if (o.emoUntil > now) {
      ctx.font = '22px system-ui, sans-serif';
      ctx.textBaseline = 'alphabetic';
      ctx.fillText(o.emo, sx, above);
    }
  }
  ctx.globalAlpha = 1;
}
requestAnimationFrame(frame);

function camFor(o) {
  const v = o === me ? (local.video && selfCam) : o.cam && tiles.get(o.id)?.video;
  return v && v.readyState >= 2 && v.videoWidth ? v : null;
}
function camBubble(v, x, bottom, mirror) {
  const s = Math.round(Math.min(88, Math.max(48, 38 * zoom))), r = Math.round(s * 0.22), b = 3;
  const bx = Math.round(x - s / 2), by = Math.round(bottom - s - 6);
  ctx.fillStyle = '#5b4fd6';
  ctx.shadowColor = 'rgba(20,22,35,.22)';
  ctx.shadowBlur = 8;
  ctx.beginPath();
  ctx.roundRect(bx, by, s, s, r);
  ctx.moveTo(x - 6, by + s - 1);
  ctx.lineTo(x, by + s + 6);
  ctx.lineTo(x + 6, by + s - 1);
  ctx.fill();
  ctx.shadowBlur = 0;
  const vw = v.videoWidth, vh = v.videoHeight, c = Math.min(vw, vh);
  const ix = bx + b, iy = by + b, is = s - b * 2;
  ctx.save();
  ctx.beginPath();
  ctx.roundRect(ix, iy, is, is, r - b);
  ctx.clip();
  ctx.imageSmoothingEnabled = true;
  if (mirror) {
    ctx.translate(ix * 2 + is, 0);
    ctx.scale(-1, 1);
  }
  ctx.drawImage(v, (vw - c) / 2, (vh - c) / 2, c, c, ix, iy, is, is);
  ctx.restore();
  return by;
}

function bubble(text, x, bottom) {
  ctx.font = '500 12px -apple-system, system-ui, sans-serif';
  const words = text.split(/\s+/), lines = [];
  let cur = '';
  for (const w of words) {
    const t = cur ? cur + ' ' + w : w;
    if (ctx.measureText(t).width > 180 && cur) { lines.push(cur); cur = w; } else cur = t;
    if (lines.length === 3) break;
  }
  if (lines.length < 3 && cur) lines.push(cur);
  const w = Math.min(196, Math.max(...lines.map((l) => ctx.measureText(l).width)) + 18), h = lines.length * 16 + 10;
  const bx = Math.round(x - w / 2), by = Math.round(bottom - h - 5);
  ctx.fillStyle = '#ffffff';
  ctx.shadowColor = 'rgba(20,22,35,.18)';
  ctx.shadowBlur = 8;
  ctx.beginPath();
  ctx.roundRect(bx, by, w, h, 9);
  ctx.moveTo(x - 5, by + h);
  ctx.lineTo(x, by + h + 5);
  ctx.lineTo(x + 5, by + h);
  ctx.fill();
  ctx.shadowBlur = 0;
  ctx.fillStyle = '#1d2030';
  ctx.textBaseline = 'top';
  lines.forEach((l, i) => ctx.fillText(l, x, by + 6 + i * 16));
  return by;
}

function tick(now) {
  if (now - lastStep >= STEP_MS) {
    const k = held[held.length - 1];
    if (k) {
      path = [];
      step(...KEYS[k]);
      lastStep = now;
    } else if (path.length) {
      const [nx, ny] = path.shift();
      const dx = nx - me.x, dy = ny - me.y;
      if (Math.abs(dx) + Math.abs(dy) !== 1 || !step(dx, dy, dy > 0 ? 0 : dy < 0 ? 1 : dx < 0 ? 2 : 3)) path = [];
      lastStep = now;
      if (!path.length && pendingUse) {
        const u = pendingUse;
        pendingUse = null;
        if (Math.abs(me.x - u.tx) + Math.abs(me.y - u.ty) <= 1) u.fn(u.tx, u.ty);
      }
    }
  }
  for (const fn of hooks.tick) fn(now);
  if (now - lastProx > 250) {
    lastProx = now;
    mesh.want(new Set(nearby(MAX_PEERS, true)));
    updateZone();
    if (peopleDirty) renderPeople();
    if (!mm.hidden && !document.hidden) drawMinimap();
  }
}
setInterval(() => me && document.hidden && tick(performance.now()), 500);

function approach(o, dt) {
  const dx = o.x - o.rx, dy = o.y - o.ry;
  if (Math.abs(dx) > 3 || Math.abs(dy) > 3) { o.rx = o.x; o.ry = o.y; return; }
  const s = dt / STEP_MS;
  o.rx += Math.sign(dx) * Math.min(Math.abs(dx), s);
  o.ry += Math.sign(dy) * Math.min(Math.abs(dy), s);
}

let mmScene = null;
function drawMinimap() {
  if (!me) return;
  const dpr = devicePixelRatio || 1;
  const sc = sceneAt(me.x + 0.5) || SCENES[0];
  mmScene = sc;
  const sw = sc.x1 - sc.x0;
  const w = mm.clientWidth || 180, h = Math.round((w * H) / sw);
  if (mm.width !== w * dpr || mm.height !== h * dpr) { mm.width = w * dpr; mm.height = h * dpr; mm.style.height = h + 'px'; }
  const c = mm.getContext('2d');
  const s = (w * dpr) / (sw * T);
  c.setTransform(1, 0, 0, 1, 0, 0);
  c.imageSmoothingEnabled = true;
  c.drawImage(mapCv, sc.x0 * T, 0, sw * T, H * T, 0, 0, mm.width, mm.height);
  c.setTransform(s, 0, 0, s, -sc.x0 * T * s, 0);
  const vw = cv.clientWidth / zoom, vh = cv.clientHeight / zoom;
  c.strokeStyle = 'rgba(255,255,255,.9)';
  c.lineWidth = 2 / s * dpr;
  c.strokeRect(camX - vw / 2, camY - vh / 2, vw, vh);
  const dot = (o, color, r) => { c.fillStyle = color; c.beginPath(); c.arc((o.x + 0.5) * T, (o.y + 0.5) * T, r / s * dpr, 0, Math.PI * 2); c.fill(); };
  for (const o of others.values()) {
    let color = o.away ? '#f59e0b' : '#22c55e';
    for (const fn of hooks.dot) color = fn(o, color) || color;
    dot(o, color, 2.5);
  }
  dot(me, '#ffffff', 4);
  dot(me, '#3b45e0', 2.5);
}
mm.addEventListener('click', (e) => {
  const r = mm.getBoundingClientRect();
  const sc = mmScene || SCENES[0];
  walkToTile(sc.x0 + Math.floor(((e.clientX - r.left) / r.width) * (sc.x1 - sc.x0)), Math.floor(((e.clientY - r.top) / r.height) * H));
});

async function acquire(kind) {
  const id = deviceIds[kind === 'audio' ? 'audioinput' : 'videoinput'];
  const s = await navigator.mediaDevices.getUserMedia(
    kind === 'audio'
      ? { audio: { deviceId: id ? { exact: id } : undefined, echoCancellation: true, noiseSuppression: true, autoGainControl: true } }
      : { video: { deviceId: id ? { exact: id } : undefined, width: 320, height: 240, frameRate: 15 } },
  );
  return s.getTracks()[0];
}
async function setMedia(kind, on) {
  local[kind]?.stop();
  local[kind] = null;
  if (on) {
    try {
      local[kind] = await acquire(kind);
    } catch {
      noticeError(`Couldn't access your ${kind === 'audio' ? 'microphone' : 'camera'}`, 'Check your browser permissions.');
    }
  }
  await mesh.setTrack(kind, local[kind]);
  me.mic = !!local.audio;
  me.cam = !!local.video;
  selfCam.srcObject = local.video ? new MediaStream([local.video]) : null;
  selfCam.play().catch(() => {});
  sendStatus();
  renderMe();
  updateSelfTile();
  syncQuality();
}
$('#mic').onclick = () => setMedia('audio', !local.audio);
$('#cam').onclick = () => setMedia('video', !local.video);

function renderMe() {
  const mic = $('#mic'), cam = $('#cam');
  setHtml(mic, icon(me.mic ? 'mic' : 'mic-off', 18));
  setHtml(cam, icon(me.cam ? 'video' : 'video-off', 18));
  mic.classList.toggle('active', me.mic);
  cam.classList.toggle('active', me.cam);
  mic.dataset.tip = me.mic ? 'Mute' : 'Unmute';
  cam.dataset.tip = me.cam ? 'Stop video' : 'Start video';
  const b = $('#b-me');
  b.querySelector('span').textContent = initial(me);
  b.style.background = tint(me.av);
  b.querySelector('.dot').classList.toggle('away', me.away);
}

const tiles = new Map();
function tile(id) {
  let t = tiles.get(id);
  if (t) return t;
  const el = document.createElement('div');
  el.className = 'tile' + (id === 'me' ? ' me' : '');
  setHtml(el, html`<video autoplay playsinline></video><div class="ph"><span></span></div><label></label>`);
  const video = el.querySelector('video');
  if (id === 'me') video.muted = true;
  t = { el, video };
  tiles.set(id, t);
  id === 'me' ? $('#videos').prepend(el) : $('#videos').append(el);
  return t;
}
function paintTile(t, o, label) {
  t.el.classList.toggle('cam', o.cam);
  const ph = t.el.querySelector('.ph span');
  ph.textContent = initial(o);
  ph.style.background = tint(o.av);
  setHtml(t.el.querySelector('label'), html`${o.mic ? '' : icon('mic-off', 12)}${label}`);
}
function showStream(id, stream) {
  const o = others.get(id);
  if (!o) return;
  const t = tile(id);
  if (t.video.srcObject !== stream) t.video.srcObject = stream;
  t.video.play().catch(() => {});
  paintTile(t, o, o.name);
  updateSelfTile();
}
function updateTile(id) {
  const t = tiles.get(id), o = others.get(id);
  if (t && o) paintTile(t, o, o.name);
}
function removeTile(id) {
  tiles.get(id)?.el.remove();
  tiles.delete(id);
  updateSelfTile();
}
function updateSelfTile() {
  const anyone = tiles.size > (tiles.has('me') ? 1 : 0);
  if (!anyone) {
    tiles.get('me')?.el.remove();
    tiles.delete('me');
    return;
  }
  const t = tile('me');
  if (t.video.srcObject?.getVideoTracks()[0] !== local.video) t.video.srcObject = local.video ? new MediaStream([local.video]) : null;
  paintTile(t, me, 'You');
}

const POPS = {
  emo: () => html`<div class="emo-row">${EMOTES.map((e) => html`<button data-emo="${e}">${e}</button>`)}</div>`,
  say: () => {
    const n = nearby(50, false).length;
    return html`<div class="pop-h">Send nearby<button class="x" data-close>${icon('x', 14)}</button></div>
      <form class="say" id="say-form">${icon('nearby', 16)}<input id="say-input" maxlength="300" placeholder="Message…" autocomplete="off"></form>
      <div class="pop-note">${n ? `Reaches ${n} ${n === 1 ? 'person' : 'people'} near you.` : 'No one nearby right now.'} Messages here are not saved.</div>`;
  },
  me: () => html`<div class="menu">
      <div class="lbl">${me.name} · ${me.away ? 'Away' : 'Active'}</div>
      <button data-act="away">${icon(me.away ? 'sun' : 'moon', 14)}${me.away ? 'Set yourself as active' : 'Set yourself as away'}</button>
      <button data-act="seat">${icon('seat', 14)}Take a seat</button>
      ${meItems().map((a, i) => html`<button data-act="m${i}">${a.icon}${a.label}</button>`)}
      <hr><button data-act="leave">${icon('leave', 14)}Leave office</button></div>`,
  keys: () => html`<div class="pop-h">Shortcuts<button class="x" data-close>${icon('x', 14)}</button></div>
    <div class="keys">
      <span><kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd></span><span>Move</span>
      <span><kbd>Click</kbd></span><span>Walk there · drag to pan</span>
      <span><kbd>Enter</kbd></span><span>Chat</span>
      <span><kbd>⌘</kbd> <kbd>K</kbd></span><span>Find anything</span>
      <span><kbd>⌘</kbd> <kbd>F</kbd></span><span>Search people</span>
      <span><kbd>Esc</kbd></span><span>Close</span>
    </div>`,
};
const meItems = () => hooks.meMenu.flatMap((fn) => fn() || []);
const face = (o) => hooks.face.map((fn) => fn(o) || '');
async function devicePop(kind) {
  let list = [];
  try { list = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === kind); } catch {}
  const isMic = kind === 'audioinput', track = isMic ? local.audio : local.video;
  const current = track?.getSettings().deviceId || deviceIds[kind];
  const items = list.length
    ? list.map((d, i) => html`<button data-dev="${d.deviceId}"><span class="ck">${d.deviceId === current ? icon('check', 14) : ''}</span>${d.label || `${isMic ? 'Microphone' : 'Camera'} ${i + 1}`}</button>`)
    : html`<button disabled><span class="ck"></span>No devices found</button>`;
  return html`<div class="menu"><div class="lbl">${isMic ? 'Microphone' : 'Camera'}</div>${items}<hr>
    <button data-act="${isMic ? 'mic' : 'cam'}">${icon(isMic ? (me.mic ? 'mic-off' : 'mic') : me.cam ? 'video-off' : 'video', 14)}${
      isMic ? (me.mic ? 'Mute' : 'Unmute') : me.cam ? 'Stop video' : 'Start video'}</button></div>`;
}

async function openPop(kind, btn) {
  if (popKind === kind) return closePop();
  closePop();
  popKind = kind;
  const markup = kind === 'audioinput' || kind === 'videoinput' ? await devicePop(kind) : POPS[kind]();
  if (popKind !== kind) return;
  const pop = $('#pop');
  setHtml(pop, markup);
  pop.hidden = false;
  btn?.classList.add('open');
  pop.dataset.kind = kind;
  if (kind === 'say') {
    $('#say-input').focus();
    $('#say-form').onsubmit = (e) => {
      e.preventDefault();
      const text = $('#say-input').value.trim();
      if (!text) return;
      send({ t: 'say', text, to: nearby(50, false) });
      me.say = text;
      me.sayUntil = performance.now() + 7000;
      $('#say-input').value = '';
    };
  }
}
function closePop() {
  popKind = null;
  $('#pop').hidden = true;
  for (const b of document.querySelectorAll('#bar .open')) b.classList.remove('open');
}
$('#b-emo').onclick = (e) => openPop('emo', e.currentTarget);
$('#b-say').onclick = (e) => openPop('say', e.currentTarget);
$('#b-me').onclick = (e) => openPop('me', e.currentTarget);
for (const c of document.querySelectorAll('.chev')) c.onclick = () => openPop(c.dataset.dev);
$('#leave').onclick = leave;

$('#pop').onclick = async (e) => {
  const b = e.target.closest('button');
  if (!b) return;
  if (b.hasAttribute('data-close')) return closePop();
  if (b.dataset.emo) return emote(b.dataset.emo);
  if (b.dataset.dev) {
    const kind = $('#pop').dataset.kind;
    deviceIds[kind] = b.dataset.dev;
    closePop();
    const k = kind === 'audioinput' ? 'audio' : 'video';
    if (local[k]) setMedia(k, true);
    return;
  }
  const act = b.dataset.act;
  closePop();
  if (act === 'away') toggleAway();
  else if (act === 'seat') takeSeat();
  else if (act === 'leave') leave();
  else if (act === 'mic') setMedia('audio', !local.audio);
  else if (act === 'cam') setMedia('video', !local.video);
  else if (act?.[0] === 'm') meItems()[+act.slice(1)]?.run();
};
document.addEventListener('pointerdown', (e) => {
  if (popKind && !e.target.closest('#dock') && !e.target.closest('#controls')) closePop();
});

function toggleAway() {
  me.away = !me.away;
  sendStatus();
  renderMe();
  peopleDirty = true;
}

function showPanel(name, forceOpen) {
  const app = $('#app');
  const current = document.querySelector('#rail .rail-btn.on')?.dataset.panel;
  if (current === name && !forceOpen && !app.classList.contains('collapsed')) {
    app.classList.add('collapsed');
    return;
  }
  app.classList.remove('collapsed');
  for (const b of document.querySelectorAll('#rail [data-panel]')) b.classList.toggle('on', b.dataset.panel === name);
  for (const p of panels) { const el = $(`#panel-${p}`); if (el) el.hidden = name !== p; }
  if (name === 'chat') {
    $('#unread').hidden = true;
    $('#msgs').scrollTop = $('#msgs').scrollHeight;
  }
  document.dispatchEvent(new CustomEvent('office:panel', { detail: name }));
}
$('#rail').onclick = (e) => {
  const b = e.target.closest('button');
  if (b?.dataset.panel) showPanel(b.dataset.panel);
  else if (b?.id === 'open-search') openPalette();
};
$('#collapse').onclick = () => $('#app').classList.add('collapsed');
for (const b of document.querySelectorAll('.copy-link'))
  b.onclick = () => {
    const link = location.origin + location.pathname;
    navigator.clipboard?.writeText(link).catch(() => {});
    setNote($('#invite-note'), { title: 'Invite link copied', body: link, tone: 'ok' });
  };
$('#people-q').oninput = () => renderPeople();
$('#online-h').onclick = () => {
  onlineOpen = !onlineOpen;
  $('#online-h').classList.toggle('closed', !onlineOpen);
  renderPeople();
};

function renderPeople() {
  peopleDirty = false;
  const q = $('#people-q').value.trim().toLowerCase();
  const list = [me, ...[...others.values()].sort((a, b) => a.name.localeCompare(b.name))].filter((o) => !q || o.name.toLowerCase().includes(q));
  $('#count').textContent = others.size + 1;
  const ul = $('#people');
  ul.textContent = '';
  if (!onlineOpen) return;
  if (!list.length) return setHtml(ul, html`<li class="empty">No one matches that name.</li>`);
  for (const o of list) {
    const li = document.createElement('li');
    li.dataset.id = o.id;
    setHtml(li, html`<span class="av" style="background:${tint(o.av)}">${initial(o)}${face(o)}<b class="dot${o.away ? ' away' : ''}"></b></span>
      <span class="who"><b>${o.name}${o === me ? html` <span class="muted">(you)</span>` : ''}${hooks.personBadges.map((fn) => fn(o) || '')}</b><small>${o.away ? 'Away' : 'Active'} · ${zoneName(o)}</small></span>${
      o === me ? '' : html`<span class="acts">${personActs(o).map((a, i) => html`<button class="icon-btn" data-act="x${i}" data-tip="${a.tip}">${a.icon}</button>`)}<button class="icon-btn" data-act="wave" data-tip="Wave">${icon('wave', 14)}</button><button class="icon-btn" data-act="walk" data-tip="Walk over">${icon('walk', 14)}</button></span>`}`);
    ul.append(li);
  }
}
$('#people').onclick = (e) => {
  const b = e.target.closest('button');
  const id = e.target.closest('li')?.dataset.id;
  if (!b || !id) return;
  if (b.dataset.act === 'walk') walkTo(id);
  if (b.dataset.act[0] === 'x') personActs(others.get(id))[+b.dataset.act.slice(1)]?.run();
  if (b.dataset.act === 'wave') { send({ t: 'wave', to: id }); setNote($('#people-note'), { title: `You waved at ${others.get(id)?.name} 👋`, tone: 'ok', ms: NOTE_MS }); }
};

function personActs(o) {
  return o ? hooks.personActions.flatMap((fn) => fn(o) || []) : [];
}
let quality = null;
function meshPeers() {
  return mesh ? [...mesh.peers.values()] : [];
}
function syncQuality() {
  const host = $('#quality');
  if (!host) return;
  const live = meshPeers().length > 0;
  const want = live || Boolean(local.audio);
  if (quality && quality.live === live && want) return;
  quality?.widget?.destroy();
  quality?.meter.stop();
  quality = null;
  host.hidden = !want;
  if (!want) return;
  const meter = createCallQualityMeter({
    readStats: live ? rtcStatsReader(() => meshPeers().map((p) => p.pc)) : null,
    tracks: () => ({ mic: local.audio, peers: meshPeers().flatMap((p) => p.stream.getAudioTracks()) }),
  });
  const widget = mountCallQualityWidget(host, meter, {
    video: () => Boolean(local.video) || meshPeers().some((p) => p.stream.getVideoTracks().length),
    peerLabel: 'Nearby',
  });
  quality = { live, meter, widget };
}

let lastZoneKey = '';
function updateZone() {
  syncQuality();
  const z = ZONES[zoneAt(me.x, me.y)];
  const n = mesh.peers.size;
  const zoneKey = `${z?.name || ''}|${z?.private ? 1 : 0}|${n}`;
  if (zoneKey === lastZoneKey) return;
  lastZoneKey = zoneKey;
  setHtml($('#zone'), html`${z?.private ? icon('lock', 13) : icon('pin', 13)}${z?.name || ''}${n ? html`<span class="talk">· talking with ${n}</span>` : ''}`);
}

let palSel = 0, palItems = [];
function openPalette() {
  if (!me) return;
  closePop();
  $('#palette').hidden = false;
  $('#pal-q').value = '';
  renderPalette();
  $('#pal-q').focus();
}
function closePalette() {
  $('#palette').hidden = true;
}
function renderPalette() {
  const q = $('#pal-q').value.trim().toLowerCase();
  const all = [
    { ic: html`<span style="color:#f97316">${icon(me.away ? 'sun' : 'moon', 14)}</span>`, label: me.away ? 'Set yourself as active' : 'Set yourself as away', run: toggleAway },
    { ic: icon('seat', 14), label: 'Take a seat', run: takeSeat },
    ...Object.entries(ROOM_ANCHORS).map(([k, a]) => ({
      ic: icon(ZONES[k].private ? 'lock' : 'pin', 14), label: ZONES[k].name, sub: 'Walk to room', run: () => walkToTile(a.x, a.y),
    })),
    ...[...others.values()].map((o) => ({
      ic: html`<span class="ic" style="background:${tint(o.av)};color:#1d2030">${initial(o)}</span>`, raw: true,
      label: o.name, sub: `${o.away ? 'Away' : 'Active'} · ${zoneName(o)}`, run: () => walkTo(o.id),
    })),
    ...hooks.palette.flatMap((fn) => fn(q) || []),
  ];
  palItems = all.filter((i) => !q || i.label.toLowerCase().includes(q));
  palSel = Math.min(palSel, Math.max(0, palItems.length - 1));
  setHtml($('#pal-list'), palItems.length
    ? html`${palItems.map((i, n) => html`<li data-n="${n}" class="${n === palSel ? 'sel' : ''}">${i.raw ? i.ic : html`<span class="ic">${i.ic}</span>`}${i.label}${i.sub ? html`<span class="sub">${i.sub}</span>` : ''}</li>`)}`
    : html`<li class="none">Nothing found</li>`);
}
function runPalette(n) {
  const item = palItems[n];
  closePalette();
  item?.run();
}
$('#pal-q').oninput = () => { palSel = 0; renderPalette(); };
$('#pal-q').onkeydown = (e) => {
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    palSel = (palSel + (e.key === 'ArrowDown' ? 1 : -1) + palItems.length) % Math.max(1, palItems.length);
    renderPalette();
    $('#pal-list .sel')?.scrollIntoView({ block: 'nearest' });
  } else if (e.key === 'Enter') runPalette(palSel);
  else if (e.key === 'Escape') closePalette();
};
$('#pal-list').onclick = (e) => { const li = e.target.closest('li[data-n]'); if (li) runPalette(+li.dataset.n); };
$('#pal-list').onmousemove = (e) => {
  const li = e.target.closest('li[data-n]');
  if (li && +li.dataset.n !== palSel) { palSel = +li.dataset.n; renderPalette(); }
};
$('#palette').onclick = (e) => e.target.id === 'palette' && closePalette();

function hudNote(note) {
  return noteLine($('#notes'), note);
}
function notice(title, body, action, fn, iconName = 'walk') {
  return hudNote({ title, body, actions: action ? [{ label: action, run: fn, icon: iconName, primary: true }] : [] });
}
function noticeError(title, body) {
  return hudNote({ title, body, tone: 'error' });
}

function leave() {
  leaving = true;
  for (const fn of hooks.leave) fn();
  mesh?.closeAll();
  local.audio?.stop();
  local.video?.stop();
  ws?.close();
  location.reload();
}

window.office = { get me() { return me; }, get others() { return others; }, get mesh() { return mesh; }, get path() { return path; }, get held() { return held; }, walk: (x, y) => (walkToTile(x, y), path.length) };

const on = (list) => (fn) => { list.push(fn); return fn; };
function addPanel({ id, iconName, iconHtml, tip, html: markup }) {
  panels.add(id);
  const btn = document.createElement('button');
  btn.className = 'rail-btn';
  btn.dataset.panel = id;
  btn.dataset.tip = tip;
  setHtml(btn, iconHtml || icon(iconName, 18));
  $('#rail').append(btn);
  const sec = document.createElement('section');
  sec.id = `panel-${id}`;
  sec.className = 'panel';
  sec.hidden = true;
  setHtml(sec, markup || html``);
  $('#side').append(sec);
  return { button: btn, panel: sec };
}
function addDockButton({ id, iconName, iconHtml, tip, onClick, before = '#leave' }) {
  const b = document.createElement('button');
  if (id) b.id = id;
  b.className = 'tgl';
  b.dataset.tip = tip;
  setHtml(b, iconHtml || icon(iconName, 18));
  b.onclick = (e) => onClick(e, b);
  $('#bar').insertBefore(b, $(before).previousElementSibling?.classList.contains('sep') ? $(before).previousElementSibling : $(before));
  return b;
}
function addControl({ id, iconName, iconHtml, tip, onClick }) {
  const wrap = document.createElement('div');
  wrap.className = 'ctl';
  const b = document.createElement('button');
  if (id) b.id = id;
  b.dataset.tip = tip;
  setHtml(b, iconHtml || icon(iconName, 18));
  b.onclick = (e) => { e.stopPropagation(); onClick(e, b); };
  wrap.append(b);
  $('#controls').append(wrap);
  return b;
}
function css() {}
const api = {
  $, esc, html, setHtml, icon, tint, initial, zoneName, store, T, W, H, ZONES, ROOM_ANCHORS, zoneAt, isWalkable, chairDir, obj, EMOTES,
  get me() { return me; },
  get others() { return others; },
  get mesh() { return mesh; },
  get ws() { return ws; },
  get zoom() { return zoom; },
  get camera() { return { x: camX, y: camY, ox, oy, zoom }; },
  get path() { return path; },
  get mapCanvas() { return mapCv; },
  setZoom,
  lookAt: (x, y) => { camFollow = false; camX = (x + 0.5) * T; camY = (y + 0.5) * T; },
  onPlace: on(hooks.place),
  useObject: (tx, ty, fn) => me && useObject(tx, ty, fn),
  local, tiles, deviceIds,
  send, notice, noticeError, hudNote, setNote, toast: notice, walkTo, walkToTile, takeSeat, showPanel, openPalette, closePop, nearby, setMedia, sendStatus, toggleAway, renderMe, updateSelfTile, tile, paintTile, removeTile, connect, leave,
  worldToScreen: (x, y) => ({ x: ox + x * zoom, y: oy + y * zoom }),
  screenToTile: (cx, cy) => { const r = cv.getBoundingClientRect(); return { x: Math.floor((cx - r.left - ox) / zoom / T), y: Math.floor((cy - r.top - oy) / zoom / T) }; },
  refreshPeople: () => { peopleDirty = true; },
  setExtra: (k, v) => { if (me) { if (v === null || v === undefined) delete me.ex[k]; else me.ex[k] = v; send({ t: 'ex', k, v: v ?? null }); peopleDirty = true; } },
  onMessage: (t, fn) => { if (!hooks.msg.has(t)) hooks.msg.set(t, []); hooks.msg.get(t).push(fn); },
  onInit: on(hooks.init), onExtra: on(hooks.ex), onTick: on(hooks.tick), onDrawWorld: on(hooks.drawWorld), onDrawUnder: on(hooks.drawUnder), onDrawScreen: on(hooks.drawScreen),
  dotColor: on(hooks.dot), proximity: on(hooks.proximity), wsParams: on(hooks.wsParams), joinExtras: on(hooks.joinExtras),
  personBadge: on(hooks.personBadges), personActions: on(hooks.personActions), paletteItems: on(hooks.palette), onLeave: on(hooks.leave),
  onMesh: on(hooks.peer), onDenied: on(hooks.denied), onPerson: on(hooks.person), meMenu: on(hooks.meMenu), face: on(hooks.face), personActs, tagAlpha: on(hooks.tagAlpha),
  onEmote: on(hooks.emote), onPose: on(hooks.pose), emote,
  onObject: (char, fn) => hooks.objects.set(char, fn),
  addPanel, addDockButton, addControl, css,
  forkmesh: async () => { throw new Error('not signed in'); },
  set socketSource(fn) { socketSource = fn; },
  iceServers: [],
  account: null,
};
for (const setup of FEATURES) {
  try { setup(api); } catch (err) { console.error('office feature failed', err); }
}
