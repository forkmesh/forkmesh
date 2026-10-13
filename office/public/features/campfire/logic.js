import { FIRE_SEATS } from '../../map.js';

export const SEATS = FIRE_SEATS.length;
export const TIERS = { blaze: 'Blazing', ember: 'Glowing' };
const NAME_RE = /^[A-Za-z0-9_.-]{1,63}$/;

export function cleanTop(list) {
  const out = [];
  const seen = new Set();
  for (const t of Array.isArray(list) ? list : []) {
    const n = String(t?.n ?? '');
    const r = Number(t?.r);
    if (!NAME_RE.test(n) || !Number.isInteger(r) || r < 1 || r > SEATS) continue;
    if (seen.has(r) || !Object.hasOwn(TIERS, t?.t)) continue;
    seen.add(r);
    const s = Number(t?.s);
    out.push({ n, r, t: t.t, s: Number.isInteger(s) ? Math.max(0, Math.min(100, s)) : 0 });
  }
  return out.sort((a, b) => a.r - b.r);
}

export function topByName(top) {
  return new Map(top.map((t) => [t.n.toLowerCase(), t]));
}

export function accountOf(o) {
  return String(o?.ex?.fm?.a || '').toLowerCase();
}

export function seatPlan(top, people) {
  const here = new Set();
  for (const o of people || []) here.add(accountOf(o));
  return top.map((t) => {
    const [x, y] = FIRE_SEATS[t.r - 1];
    return { x, y, top: t, here: here.has(t.n.toLowerCase()) };
  });
}

export function describeTop(top) {
  if (!top.length) return 'Nobody is at the fire yet. The most active members of this organization earn a seat.';
  return top.map((t) => `#${t.r} ${t.n} (${TIERS[t.t]}, ${t.s})`).join(' · ');
}

export function flames(now) {
  const out = [];
  for (let i = 0; i < 5; i++) {
    const h = 8 + Math.sin(now / (110 + i * 23) + i * 1.7) * 3 + (i === 2 ? 5 : 0);
    out.push({ dx: (i - 2) * 3, h: Math.max(3, h) });
  }
  return out;
}
