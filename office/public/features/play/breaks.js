import { svg, goUse } from './ui.js';
import { coffeeEntry, logError, ASK_EVERY_MS, PACKAGE_URL } from './coffee-log.js';

const BREAK = { s: 'break', emoji: '☕', text: 'Coffee break' };
const BUBBLE_MS = 15000;

export function isCoffeeStatus(s) {
  return !!s && s.s === 'break' && s.emoji === '☕';
}

function wrap(ctx, text, max) {
  const words = text.split(/\s+/), lines = [];
  let cur = '';
  for (const w of words) {
    const t = cur ? cur + ' ' + w : w;
    if (ctx.measureText(t).width > max && cur) { lines.push(cur); cur = w; } else cur = t;
  }
  if (cur) lines.push(cur);
  return lines.slice(0, 4);
}

export default function breaks(api, arrive) {
  let onBreak = null;
  let askedAt = -Infinity;
  let hinted = false;
  let logging = false;

  async function offerCoffeeLog() {
    const t = performance.now();
    if (!api.calories || logging || t - askedAt < ASK_EVERY_MS) return;
    askedAt = t;
    let overview;
    try {
      const r = await api.calories.overview();
      if (!r.ok) return;
      overview = await r.json();
    } catch {
      return;
    }
    if (!overview || overview.installed === false) {
      if (hinted) return;
      hinted = true;
      return api.hudNote({ key: 'coffee-log', title: 'Track your coffee', body: 'With the Calorie Counter plugin you can log this coffee and send it to Apple Health.', actions: [{ label: 'Get the plugin', icon: 'plus', primary: true, run: () => window.open(PACKAGE_URL, '_blank', 'noopener') }] });
    }
    const entry = coffeeEntry(overview, new Date());
    const said = (title, body, tone) => api.hudNote({ key: 'coffee-log', title, body, tone });
    api.hudNote({ key: 'coffee-log', title: `Log ${entry.name.toLowerCase()} to Calorie Counter?`, body: `${entry.calories} kcal${entry.portion ? ' · ' + entry.portion : ''}. It reaches Apple Health the next time you open Calories on your iPhone.`, actions: [{ label: 'Log it', icon: 'plus', primary: true, run: async () => {
      logging = true;
      said('Logging coffee…', `${entry.calories} kcal`);
      try {
        const r = await api.calories.log(entry);
        const body = await r.json().catch(() => ({}));
        if (r.ok && body.ok) said('Coffee logged', `${entry.calories} kcal added to today's diary.`, 'ok');
        else said('Coffee not logged', logError(r.status, body), 'error');
      } catch {
        said('Coffee not logged', logError(0, null), 'error');
      } finally {
        logging = false;
      }
    } }] });
  }
  const bubbles = [];

  function grab() {
    const me = api.me;
    if (!me) return;
    if (!onBreak) {
      const prev = me.ex?.status;
      onBreak = { saved: isCoffeeStatus(prev) ? null : prev ? { ...prev } : null, zone: api.zoneAt(me.x, me.y), at: performance.now() };
      api.setExtra('status', { ...BREAK });
      api.setExtra('coffee', true);
    } else onBreak.at = performance.now();
    api.send({ t: 'play-coffee' });
    offerCoffeeLog();
  }

  function endBreak() {
    const me = api.me;
    const b = onBreak;
    onBreak = null;
    if (!me || !b) return;
    if (isCoffeeStatus(me.ex?.status)) api.setExtra('status', b.saved);
    if (me.ex?.coffee) api.setExtra('coffee', null);
  }

  api.onObject('C', grab);
  api.onObject('Q', () => api.send({ t: 'play-cooler' }));

  api.onMessage('play-coffee', (m) => {
    api.hudNote({ key: 'coffee', title: m.repeat ? 'Still on that coffee' : `Coffee #${m.n} today`, body: 'Status set to Coffee break until you leave the Coffee Huddle.' });
  });
  api.onMessage('play-icebreaker', (m) => {
    bubbles.splice(0, bubbles.length, { x: m.x, y: m.y, q: m.q, until: performance.now() + BUBBLE_MS });
    api.hudNote({ key: 'cooler', title: 'Water cooler question', body: `${m.q} (asked by ${m.by === api.me?.id ? 'you' : m.name})`, ms: BUBBLE_MS });
  });

  api.onInit((m, again) => {
    if (!again) onBreak = null;
  });

  api.onTick(() => {
    const me = api.me;
    if (onBreak && me && api.zoneAt(me.x, me.y) !== onBreak.zone) endBreak();
  });

  api.onDrawScreen((ctx, o, sx, above, now) => {
    if (!o.ex?.coffee) return;
    const fresh = o === api.me && onBreak && now - onBreak.at < 4000;
    const size = fresh ? 20 : 15;
    ctx.save();
    ctx.font = `${size}px system-ui, sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'alphabetic';
    ctx.fillText('☕', sx, above);
    const top = above - size;
    ctx.strokeStyle = 'rgba(120,124,140,.75)';
    ctx.lineWidth = 1.5;
    ctx.lineCap = 'round';
    for (let i = -1; i <= 1; i++) {
      const ph = now / 260 + i * 1.7;
      const bx = sx + i * 4;
      ctx.beginPath();
      for (let k = 0; k <= 8; k++) {
        const y = top - 1 - k * 1.3;
        const x = bx + Math.sin(ph + k * 0.7) * 1.8;
        if (k) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      }
      ctx.globalAlpha = 0.55 + 0.45 * Math.sin(ph);
      ctx.stroke();
    }
    ctx.restore();
    return top - 14;
  });

  api.onDrawWorld((ctx, now) => {
    const b = bubbles[0];
    if (!b) return;
    if (now > b.until) { bubbles.length = 0; return; }
    const dpr = devicePixelRatio || 1;
    const p = api.worldToScreen((b.x + 0.5) * api.T, b.y * api.T);
    ctx.save();
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.font = '500 12px -apple-system, system-ui, sans-serif';
    const lines = wrap(ctx, b.q, 190);
    const w = Math.min(210, Math.max(...lines.map((l) => ctx.measureText(l).width)) + 20), h = lines.length * 16 + 26;
    const bx = Math.round(p.x - w / 2), by = Math.round(p.y - h - 8);
    ctx.globalAlpha = Math.min(1, (b.until - now) / 800);
    ctx.fillStyle = '#ffffff';
    ctx.shadowColor = 'rgba(20,22,35,.18)';
    ctx.shadowBlur = 8;
    ctx.beginPath();
    ctx.roundRect(bx, by, w, h, 10);
    ctx.moveTo(p.x - 6, by + h);
    ctx.lineTo(p.x, by + h + 6);
    ctx.lineTo(p.x + 6, by + h);
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    ctx.fillStyle = '#3b45e0';
    ctx.font = '600 11px -apple-system, system-ui, sans-serif';
    ctx.fillText('Icebreaker', p.x, by + 7);
    ctx.fillStyle = '#1d2030';
    ctx.font = '500 12px -apple-system, system-ui, sans-serif';
    lines.forEach((l, i) => ctx.fillText(l, p.x, by + 22 + i * 16));
    ctx.restore();
  });

  api.paletteItems(() => [
    { ic: svg('coffee', 14), label: 'Grab a coffee', sub: 'Coffee Huddle', run: () => goUse(api, arrive, 'C', grab) },
    { ic: svg('droplet', 14), label: 'Water cooler question', sub: 'Icebreaker', run: () => goUse(api, arrive, 'Q', () => api.send({ t: 'play-cooler' })) },
  ]);
}
