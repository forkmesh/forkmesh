import { TILE } from './map.js';

export const T = TILE;
export const hash = (x, y, s = 0) => {
  let h = (x * 374761393 + y * 668265263 + s * 1442695041) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
};

export function px(c, color, x, y, w, h) {
  c.fillStyle = color;
  c.fillRect(x, y, w, h);
}
export function box(c, x, y, w, h, fill, edge) {
  px(c, edge, x, y, w, h);
  px(c, fill, x + 1, y + 1, w - 2, h - 2);
}
export function disc(c, cx, cy, r, color, sy = 1) {
  c.fillStyle = color;
  const ry = Math.round(r * sy);
  for (let dy = -ry; dy <= ry; dy++) {
    const w = Math.round(Math.sqrt(Math.max(0, 1 - (dy * dy) / (ry * ry || 1))) * r);
    c.fillRect(Math.round(cx - w), Math.round(cy + dy), w * 2, 1);
  }
}
export function tri(c, cx, top, h, halfW, color) {
  c.fillStyle = color;
  for (let r = 0; r < h; r++) {
    const w = Math.round((halfW * (r + 1)) / h);
    c.fillRect(cx - w, top + r, w * 2, 1);
  }
}
