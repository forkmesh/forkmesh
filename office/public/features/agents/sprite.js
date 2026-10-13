import { MODELS, EMBLEMS } from './model.js';

export const BOT_W = 18, BOT_H = 24, BOT_FOOT = 22, LIGHT_X = 9, LIGHT_Y = 2;
const cache = new Map();

function px(c, color, x, y, w, h) {
  c.fillStyle = color;
  c.fillRect(x, y, w, h);
}
function box(c, x, y, w, h, fill, edge) {
  px(c, edge, x, y, w, h);
  px(c, fill, x + 1, y + 1, w - 2, h - 2);
}
function shade(hex, k) {
  const n = parseInt(hex.slice(1), 16);
  const f = (v) => Math.max(0, Math.min(255, Math.round(k < 0 ? v * (1 + k) : v + (255 - v) * k)));
  return `rgb(${f(n >> 16)},${f((n >> 8) & 255)},${f(n & 255)})`;
}

function drawBot(c, key, blink) {
  const m = MODELS[key] || MODELS.terra;
  const shell = m.shell, dark = shade(shell, -0.45), lite = shade(shell, 0.35);
  px(c, '#8b93a3', 8, 2, 2, 4);
  box(c, 2, 5, 14, 9, shell, dark);
  px(c, lite, 3, 6, 12, 1);
  box(c, 4, 7, 10, 5, '#161b26', '#0d1017');
  if (blink) {
    px(c, m.glow, 6, 10, 2, 1);
    px(c, m.glow, 10, 10, 2, 1);
  } else {
    px(c, m.glow, 6, 8, 2, 3);
    px(c, m.glow, 10, 8, 2, 3);
  }
  px(c, dark, 1, 8, 1, 3);
  px(c, dark, 16, 8, 1, 3);
  px(c, dark, 7, 14, 4, 1);
  box(c, 3, 15, 12, 6, shell, dark);
  px(c, lite, 4, 16, 10, 1);
  box(c, 6, 15, 7, 7, '#f3f5f9', dark);
  const e = EMBLEMS[key] || EMBLEMS.terra;
  for (let y = 0; y < 5; y++) for (let x = 0; x < 5; x++) if (e[y][x] === '1') px(c, dark, 7 + x, 16 + y, 1, 1);
  px(c, dark, 1, 16, 2, 4);
  px(c, dark, 15, 16, 2, 4);
  px(c, '#2b303b', 4, 21, 4, 2);
  px(c, '#2b303b', 11, 21, 4, 2);
}

export function botSprite(key, blink = false) {
  const k = `${key}|${blink ? 1 : 0}`;
  let cv = cache.get(k);
  if (cv) return cv;
  const body = document.createElement('canvas');
  body.width = BOT_W;
  body.height = BOT_H;
  drawBot(body.getContext('2d'), key, blink);
  const sil = document.createElement('canvas');
  sil.width = BOT_W;
  sil.height = BOT_H;
  const s = sil.getContext('2d');
  s.drawImage(body, 0, 0);
  s.globalCompositeOperation = 'source-in';
  px(s, '#1b1e28', 0, 0, BOT_W, BOT_H);
  cv = document.createElement('canvas');
  cv.width = BOT_W;
  cv.height = BOT_H;
  const c = cv.getContext('2d');
  for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) c.drawImage(sil, dx, dy);
  c.drawImage(body, 0, 0);
  cache.set(k, cv);
  return cv;
}

const urls = new Map();
export function botDataUrl(key) {
  let u = urls.get(key);
  if (u) return u;
  const src = botSprite(key);
  const cv = document.createElement('canvas');
  cv.width = BOT_W * 2;
  cv.height = BOT_H * 2;
  const c = cv.getContext('2d');
  c.imageSmoothingEnabled = false;
  c.drawImage(src, 0, 0, BOT_W * 2, BOT_H * 2);
  u = cv.toDataURL();
  urls.set(key, u);
  return u;
}

export function drawLight(ctx, x, y, color, glow) {
  if (glow > 0) {
    ctx.globalAlpha = glow;
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(x, y, 4.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.globalAlpha = 1;
  }
  ctx.fillStyle = '#1b1e28';
  ctx.fillRect(x - 2, y - 2, 4, 4);
  ctx.fillStyle = color;
  ctx.fillRect(x - 1.5, y - 1.5, 3, 3);
}
