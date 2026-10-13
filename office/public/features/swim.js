import { html } from '../html.js';
import { TILE as T, W } from '../map.js';
import { SPRITE_W, SPRITE_FOOT } from '../sprite.js';
import { inWater, seedOf, strokePose, bob, nearestWater, WATERLINE } from './swim/logic.js';

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const WAVES = html`<path d="M2 12c2 0 2-2 4-2s2 2 4 2 2-2 4-2 2 2 4 2 2-2 4-2"/><path d="M2 18c2 0 2-2 4-2s2 2 4 2 2-2 4-2 2 2 4 2 2-2 4-2"/>`;

export default function setup(api) {
  const wet = new Map();
  const drops = [];
  let last = 0;
  let told = false;

  const splash = (o) => {
    const cx = (o.rx + 0.5) * T, cy = (o.ry + 0.5) * T;
    for (let i = 0; i < 14; i++) {
      const a = Math.PI + (i / 13) * Math.PI;
      drops.push({ x: cx, y: cy, vx: Math.cos(a) * (0.03 + Math.random() * 0.05), vy: Math.sin(a) * (0.06 + Math.random() * 0.06), life: 420 });
    }
  };

  api.onPose((o, now) => {
    if (!inWater(o)) return null;
    return { pose: strokePose(now, seedOf(o.id)), step: 0, sit: true };
  });

  api.onDrawWorld((ctx, now, vis) => {
    const dt = Math.min(50, now - last);
    last = now;
    for (const o of vis) {
      const swimming = inWater(o);
      if (swimming && !wet.get(o.id)) {
        splash(o);
        if (o === api.me && !told) {
          told = true;
          api.notice('Splash! You are swimming', 'Walk out of the pond to dry off.');
        }
      }
      wet.set(o.id, swimming);
      if (!swimming) continue;
      const seed = seedOf(o.id);
      const x = Math.round(o.rx * T + (T - SPRITE_W) / 2);
      const top = Math.round(o.ry * T + T - SPRITE_FOOT - 2) + WATERLINE + bob(now, seed);
      const bottom = Math.round(o.ry * T + T);
      ctx.fillStyle = 'rgba(79,176,214,.94)';
      ctx.fillRect(x, top, SPRITE_W, bottom - top);
      ctx.fillStyle = 'rgba(108,196,228,.95)';
      ctx.fillRect(x + 2, top, SPRITE_W - 4, 2);
      const r = 9 + ((now / 90 + seed * 20) % 6);
      ctx.strokeStyle = 'rgba(233,247,252,.75)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.ellipse(x + SPRITE_W / 2, top + 1, r, 3, 0, 0, Math.PI * 2);
      ctx.stroke();
    }
    if (!drops.length) return;
    ctx.fillStyle = '#d6f0fa';
    for (let i = drops.length - 1; i >= 0; i--) {
      const d = drops[i];
      d.life -= dt;
      if (d.life <= 0) { drops.splice(i, 1); continue; }
      d.vy += 0.0004 * dt;
      d.x += d.vx * dt;
      d.y += d.vy * dt;
      ctx.fillRect(Math.round(d.x), Math.round(d.y), 2, 2);
    }
  });

  api.paletteItems(() => [{
    ic: svg(WAVES, 14),
    label: 'Go for a swim', sub: 'Garden pond',
    run: () => {
      if (!api.me) return;
      const taken = new Set([...api.others.values()].map((o) => o.y * W + o.x));
      const spot = nearestWater(api.me, taken);
      if (spot) api.walkToTile(spot.x, spot.y);
    },
  }]);
}
