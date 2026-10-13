export const CHEER_EMOTES = ['👏', '🎉', '💃'];
export const CLAP_MS = 2200;
export const DANCE_MS = 3000;
export const CHEER_MS = 2800;
export const HEAR_TILES = 8;
export const MAX_PARTS = 400;
export const CONFETTI = ['#ef5a4c', '#4a90e2', '#3cc47c', '#f5a623', '#9b6bdb', '#ec5f9b', '#f2c94c'];
const DANCE_DIRS = [0, 2, 0, 3];

export function poseFor(o, now) {
  if (o?.clapUntil > now) return { pose: ((now / 150) | 0) % 2 ? 2 : 1 };
  if (o?.danceUntil > now) {
    const k = (now / 240) | 0;
    return { pose: 3, dir: DANCE_DIRS[k % 4], step: k % 2 ? 1 : 2 };
  }
  return null;
}

export function burst(x, y, n, rng = Math.random) {
  const out = [];
  for (let i = 0; i < n; i++) {
    const a = -Math.PI / 2 + (rng() - 0.5) * 2.4;
    const v = 0.05 + rng() * 0.11;
    out.push({ x, y, vx: Math.cos(a) * v, vy: Math.sin(a) * v, c: CONFETTI[(rng() * CONFETTI.length) | 0], s: rng() < 0.5 ? 2 : 3, life: 900 + rng() * 900 });
  }
  return out;
}

export function stepParticles(list, dt) {
  let keep = 0;
  for (const p of list) {
    p.life -= dt;
    if (p.life <= 0) continue;
    p.vy += 0.00022 * dt;
    p.vx *= 1 - 0.0012 * dt;
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    list[keep++] = p;
  }
  list.length = keep;
  return list;
}

export function canHear(me, o, sameZone, range = HEAR_TILES) {
  if (!me || !o) return false;
  if (me === o || sameZone) return true;
  return Math.max(Math.abs(o.x - me.x), Math.abs(o.y - me.y)) <= range;
}
