import { html } from '../html.js';
import { TILE as T, FIRE, FIRE_SEATS, W, H, obj } from '../map.js';
import { SEATS, TIERS, cleanTop, topByName, accountOf, seatPlan, describeTop, flames } from './campfire/logic.js';

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const FLAME = html`<path d="M12 22c4 0 7-3 7-7 0-4-3-6-4-10-2 2-3 4-3 6-1-1-2-2-2-4-2 2-5 5-5 8 0 4 3 7 7 7z"/>`;

export default function setup(api) {
  let top = [];
  let byName = new Map();
  let fire = null;
  for (let y = 0; y < H && !fire; y++) for (let x = 0; x < W; x++) if (obj[y][x] === FIRE) { fire = { x, y }; break; }

  const set = (list) => {
    top = cleanTop(list);
    byName = topByName(top);
    api.refreshPeople();
  };
  const rankOf = (o) => byName.get(accountOf(o));
  const people = () => [api.me, ...api.others.values()].filter(Boolean);

  api.onInit((m) => set(m.fire));
  api.onMessage('fire', (m) => set(m.top));
  api.onMessage('fire-locked', (m) => api.notice('The campfire is for top members',
    `Only the ${SEATS} most active members of ${String(m.org || 'this organization')} sit here. The ranking updates every 10 minutes.`));

  api.personBadge((o) => {
    const t = rankOf(o);
    return t ? html` <span class="cf-badge cf-${t.t}" title="${`Top member #${t.r}: ${TIERS[t.t]}, fire score ${t.s}`}">${svg(FLAME, 11)}${t.r}</span>` : '';
  });

  api.onDrawUnder((ctx, now) => {
    if (fire) {
      const cx = (fire.x + 0.5) * T, cy = (fire.y + 0.5) * T;
      const glow = ctx.createRadialGradient(cx, cy, 2, cx, cy, T * 2.2);
      glow.addColorStop(0, 'rgba(255,170,60,.30)');
      glow.addColorStop(1, 'rgba(255,170,60,0)');
      ctx.fillStyle = glow;
      ctx.fillRect(cx - T * 2.2, cy - T * 2.2, T * 4.4, T * 4.4);
      for (const f of flames(now)) {
        ctx.fillStyle = '#f08a3c';
        ctx.fillRect(Math.round(cx + f.dx - 2), Math.round(cy - f.h), 4, Math.round(f.h));
        ctx.fillStyle = '#ffd166';
        ctx.fillRect(Math.round(cx + f.dx - 1), Math.round(cy - f.h * 0.6), 2, Math.round(f.h * 0.6));
      }
    }
    ctx.font = '600 7px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (const s of seatPlan(top, people())) {
      if (s.here) continue;
      const label = `${s.top.r}. ${s.top.n}`;
      const w = Math.min(70, ctx.measureText(label).width + 8);
      const x = (s.x + 0.5) * T, y = s.y * T + 4;
      ctx.fillStyle = s.top.t === 'blaze' ? 'rgba(122,52,18,.85)' : 'rgba(84,62,40,.78)';
      ctx.beginPath();
      ctx.roundRect(x - w / 2, y - 5, w, 10, 5);
      ctx.fill();
      ctx.fillStyle = '#fff4e0';
      ctx.fillText(label, x, y, w - 6);
    }
  });

  api.paletteItems(() => {
    const items = [{ ic: svg(FLAME, 14), label: 'Campfire: top members', sub: `${top.length} of ${SEATS} seats`, run: () => api.notice('Top members at the campfire', describeTop(top)) }];
    const mine = api.me && rankOf(api.me);
    if (mine) {
      const [x, y] = FIRE_SEATS[mine.r - 1];
      items.push({ ic: svg(FLAME, 14), label: 'Sit at the campfire', sub: `Your seat, #${mine.r}`, run: () => api.walkToTile(x, y) });
    }
    return items;
  });
}
