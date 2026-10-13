import { W, H, obj, WATER } from '../../map.js';

export const WATERLINE = 30;

export function inWater(o) {
  const x = Math.round(o?.rx ?? o?.x), y = Math.round(o?.ry ?? o?.y);
  return obj[y]?.[x] === WATER;
}

export function seedOf(id) {
  let h = 0;
  for (const ch of String(id || '')) h = (h * 31 + ch.charCodeAt(0)) | 0;
  return Math.abs(h % 1000) / 1000;
}

export function strokePose(now, seed) {
  return Math.floor(now / 420 + seed * 2) % 2 ? 5 : 4;
}

export function bob(now, seed) {
  return Math.round(Math.sin(now / 300 + seed * 6.28) * 1.5);
}

export function waterTiles() {
  const out = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (obj[y][x] === WATER) out.push([x, y]);
  return out;
}

export function nearestWater(from, taken = new Set()) {
  let best = null, bestD = Infinity;
  for (const [x, y] of waterTiles()) {
    if (taken.has(y * W + x)) continue;
    const d = Math.abs(x - from.x) + Math.abs(y - from.y);
    if (d < bestD) { best = { x, y }; bestD = d; }
  }
  return best;
}
