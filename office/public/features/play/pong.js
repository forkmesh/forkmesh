import { html, setHtml, trusted } from '../../html.js';
import { overlay, svg } from './ui.js';
import { findTables, pongSide, endTiles, COURT_W, COURT_H, PAD_H, PAD_X, BALL_R, WIN_SCORE, clamp } from './logic.js';

const PAD_SEND_MS = 40;
const KEY_SPEED = 1.6;

export function lerpBall(prev, cur, now) {
  if (!cur) return null;
  if (!prev) return cur.b;
  const span = cur.at - prev.at || 33;
  const a = clamp((now - cur.at) / span, 0, 1);
  const [px, py] = prev.b, [cx, cy] = cur.b;
  if (Math.abs(cx - px) > 0.5 || Math.abs(cy - py) > 0.5) return cur.b;
  return [px + (cx - px) * a, py + (cy - py) * a];
}

export default function pong(api, arrive) {
  const tables = findTables(api.obj, 'G');
  const games = new Map();
  let ui = null, target = COURT_H / 2, lastSend = 0, sent = null, keyDir = 0;

  const mine = () => [...games.values()].find((g) => api.me && g.ids.includes(api.me.id));
  const tableAt = (x, y) => tables.find((t) => x >= t.x0 && x <= t.x1 && y >= t.y0 && y <= t.y1);

  function recv(g) {
    if (g.s === 'gone') { games.delete(g.id); if (ui && ui.id === g.id && ui.state?.s !== 'over') ui.h.close(); return; }
    const old = games.get(g.id);
    g.at = performance.now();
    g.prev = old && old.s === g.s ? { b: old.b, at: old.at } : null;
    games.set(g.id, g);
    const meIn = api.me && g.ids.includes(api.me.id);
    if (meIn && !ui) openUi(g);
    if (ui && ui.id === g.id) ui.update(g);
    if (g.s === 'over' && g.w && old?.s !== 'over') {
      const wi = g.w === 'L' ? 0 : 1;
      const won = api.me && g.ids[wi] === api.me.id;
      if (meIn && !(ui && ui.id === g.id)) api.hudNote({ key: 'pong', title: won ? 'You won at ping-pong' : 'Good game', body: `${g.n[0] || 'Left'} ${g.sc[0]} : ${g.sc[1]} ${g.n[1] || 'Right'}${g.why === 'left' ? ' (a player left)' : ''}`, tone: won ? 'ok' : 'info' });
    }
  }

  function join(t) {
    api.send({ t: 'play-pong-join', table: t.id });
  }

  function use(tx, ty) {
    const me = api.me;
    const t = tableAt(tx, ty);
    if (!me || !t) return;
    const g = games.get(t.id);
    const taken = (s) => { const id = g && g.s !== 'over' && g.ids[s === 'L' ? 0 : 1]; return !!id && id !== me.id; };
    const side = pongSide(t, me.x, me.y);
    if (side && !taken(side)) return join(t);
    const free = ['L', 'R'].filter((s) => !taken(s));
    const opts = (free.length ? free : ['L', 'R']).flatMap((s) => endTiles(t, s)).filter(([x, y]) => api.isWalkable(x, y));
    opts.sort((a, b) => Math.abs(a[0] - me.x) + Math.abs(a[1] - me.y) - (Math.abs(b[0] - me.x) + Math.abs(b[1] - me.y)));
    const dest = opts[0];
    if (!dest) return api.noticeError('No free end at this table');
    api.walkToTile(dest[0], dest[1]);
    arrive((m) => { const sd = pongSide(t, m.x, m.y); return !!sd && !taken(sd); }, () => join(t));
  }

  function drawCourt(c, x, y, w, h, g, now, big) {
    const sx = w / COURT_W, sy = h / COURT_H;
    c.fillStyle = big ? '#1f5f8b' : 'rgba(31,95,139,.82)';
    c.fillRect(x, y, w, h);
    c.strokeStyle = 'rgba(255,255,255,.8)';
    c.lineWidth = big ? 3 : 1;
    c.strokeRect(x + c.lineWidth / 2, y + c.lineWidth / 2, w - c.lineWidth, h - c.lineWidth);
    c.setLineDash(big ? [8, 8] : [2, 2]);
    c.beginPath();
    c.moveTo(x + w / 2, y);
    c.lineTo(x + w / 2, y + h);
    c.stroke();
    c.setLineDash([]);
    c.fillStyle = '#ffffff';
    const pw = Math.max(2, (big ? 0.03 : 0.04) * sx);
    for (const [i, px] of [[0, PAD_X], [1, COURT_W - PAD_X]]) {
      const py = g.p[i];
      c.fillRect(x + px * sx - (i ? 0 : pw), y + (py - PAD_H / 2) * sy, pw, PAD_H * sy);
    }
    if (g.s === 'play' && !g.wait) {
      const b = lerpBall(g.prev, { b: g.b, at: g.at }, now);
      c.beginPath();
      c.arc(x + b[0] * sx, y + b[1] * sy, Math.max(big ? 5 : 2, BALL_R * sy), 0, Math.PI * 2);
      c.fillStyle = '#ffd54a';
      c.fill();
    }
  }

  function openUi(g) {
    target = COURT_H / 2;
    const h = overlay(api, {
      title: 'Ping-pong',
      iconName: 'table',
      modal: false,
      body: html`<div class="pl-pong">
        <div class="pl-pong-top"><span class="pl-pn l"></span><b class="pl-psc">0 : 0</b><span class="pl-pn r"></span></div>
        <canvas class="pl-court" width="480" height="240"></canvas>
        <div class="pl-pong-foot"><span class="pl-pmsg muted"></span><span class="pl-pbtns"><button class="btn btn-sm pl-again" hidden>Play again</button><button class="btn btn-sm pl-quit">Leave</button></span></div>
      </div>`,
      onKey(k, down) {
        if (k === 'arrowup' || k === 'w') keyDir = down ? -1 : keyDir === -1 ? 0 : keyDir;
        if (k === 'arrowdown' || k === 's') keyDir = down ? 1 : keyDir === 1 ? 0 : keyDir;
      },
      onClose() {
        const cur = ui;
        ui = null;
        keyDir = 0;
        if (cur && !cur.quiet && games.get(cur.id)?.s !== 'over') api.send({ t: 'play-pong-leave' });
      },
    });
    const cv = h.q('.pl-court');
    const c = cv.getContext('2d');
    const setFromPointer = (e) => {
      const r = cv.getBoundingClientRect();
      target = clamp(((e.clientY - r.top) / r.height) * COURT_H, PAD_H / 2, COURT_H - PAD_H / 2);
    };
    cv.addEventListener('pointerdown', (e) => { e.preventDefault(); cv.setPointerCapture(e.pointerId); setFromPointer(e); });
    cv.addEventListener('pointermove', (e) => { if (e.pointerType === 'mouse' || cv.hasPointerCapture(e.pointerId)) setFromPointer(e); });
    h.q('.pl-quit').onclick = () => h.close();
    h.q('.pl-again').onclick = () => { const t = tables.find((x) => x.id === ui?.id); if (t) join(t); };
    ui = {
      h, id: g.id, state: g,
      update(s) {
        ui.state = s;
        h.q('.pl-pn.l').textContent = s.n[0] || 'Waiting…';
        h.q('.pl-pn.r').textContent = s.n[1] || 'Waiting…';
        h.q('.pl-psc').textContent = `${s.sc[0]} : ${s.sc[1]}`;
        const meSide = s.ids[0] === api.me?.id ? 0 : 1;
        let msg = '';
        if (s.s === 'wait') msg = 'Waiting for someone at the other end of the table';
        else if (s.s === 'play') msg = s.wait ? 'Get ready…' : `First to ${WIN_SCORE}. Drag or use arrow keys`;
        else if (s.s === 'over') msg = `${s.ids[s.w === 'L' ? 0 : 1] === api.me?.id ? 'You win' : `${s.n[s.w === 'L' ? 0 : 1] || 'Opponent'} wins`}${s.why === 'left' ? ' (a player left)' : ''}`;
        h.q('.pl-pmsg').textContent = msg;
        h.q('.pl-again').hidden = s.s !== 'over';
        h.el.classList.toggle('pl-right', meSide === 1);
      },
    };
    ui.update(g);
    const loop = (now) => {
      if (!ui || ui.h !== h) return;
      requestAnimationFrame(loop);
      const s = games.get(ui.id) || ui.state;
      const dt = 1 / 60;
      if (keyDir) target = clamp(target + keyDir * KEY_SPEED * dt, PAD_H / 2, COURT_H - PAD_H / 2);
      if (s.s === 'play' && now - lastSend > PAD_SEND_MS && sent !== target) {
        lastSend = now;
        sent = target;
        api.send({ t: 'play-pong-pad', y: Math.round(target * 1000) / 1000 });
      }
      const dpr = devicePixelRatio || 1;
      const w = cv.clientWidth, hh = cv.clientHeight;
      if (cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(hh * dpr); }
      c.setTransform(dpr, 0, 0, dpr, 0, 0);
      c.clearRect(0, 0, w, hh);
      drawCourt(c, 0, 0, w, hh, s, now, true);
      const mi = s.ids[0] === api.me?.id ? 0 : 1;
      c.strokeStyle = 'rgba(111,120,255,.9)';
      c.setLineDash([3, 3]);
      c.lineWidth = 1;
      c.beginPath();
      const gx = mi ? w - 4 : 4;
      c.moveTo(gx, (target - PAD_H / 2) * hh);
      c.lineTo(gx, (target + PAD_H / 2) * hh);
      c.stroke();
      c.setLineDash([]);
    };
    requestAnimationFrame(loop);
  }

  api.onObject('G', use);
  api.onInit((m) => {
    const was = ui && tables.find((t) => t.id === ui.id);
    if (ui) { ui.quiet = true; ui.h.close(); }
    games.clear();
    for (const g of m.play?.pong || []) recv(g);
    const me = api.me;
    if (was && me && !mine() && pongSide(was, me.x, me.y)) join(was);
  });
  api.onMessage('play-pong', recv);

  api.onDrawWorld((ctx, now) => {
    if (!games.size) return;
    const T = api.T;
    for (const g of games.values()) {
      const t = tables.find((x) => x.id === g.id);
      if (!t) continue;
      const x = t.x0 * T + 3, y = t.y0 * T + 3, w = (t.x1 - t.x0 + 1) * T - 6, h = (t.y1 - t.y0 + 1) * T - 6;
      ctx.save();
      drawCourt(ctx, x, y, w, h, g, now, false);
      const dpr = devicePixelRatio || 1;
      const p = api.worldToScreen(x + w / 2, y);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      const label = g.s === 'wait' ? `${g.n[0] || g.n[1]} is waiting for a partner` : `${g.n[0]} ${g.sc[0]} : ${g.sc[1]} ${g.n[1]}`;
      ctx.font = '600 11px -apple-system, system-ui, sans-serif';
      const tw = ctx.measureText(label).width + 14;
      ctx.fillStyle = 'rgba(29,32,48,.86)';
      ctx.beginPath();
      ctx.roundRect(Math.round(p.x - tw / 2), Math.round(p.y - 26), tw, 20, 6);
      ctx.fill();
      ctx.fillStyle = '#ffffff';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(label, p.x, p.y - 16);
      ctx.restore();
    }
  });

  api.paletteItems(() => [{ ic: svg('table', 14), label: 'Play ping-pong', sub: 'Hallway table', run: () => tables[0] && use(tables[0].x0, tables[0].y0) }]);
}
