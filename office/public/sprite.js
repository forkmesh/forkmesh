import { lookOf, SKIN, HAIR_COLOR, TOP, BOTTOM, SHOES } from './avatar.js';

export const SPRITE_W = 32, SPRITE_H = 48, SPRITE_FOOT = 44;
const INK = '#1d1d24';
const FRAME = '#2a2d3a';
const spriteCache = new Map();

function px(c, color, x, y, w, h) {
  c.fillStyle = color;
  c.fillRect(x, y, w, h);
}

export function palette(av) {
  const l = lookOf(av);
  return { skin: SKIN[l.skin], hair: HAIR_COLOR[l.hairColor], shirt: TOP[l.top], pants: BOTTOM[l.bottom], shoes: SHOES[l.shoes], style: l.hair, face: l.face, extra: l.extra };
}

function drawHair(c, p, dir, b) {
  const h = p.hair, s = p.style;
  if (s === 3) return;
  if (s === 6) {
    if (dir === 2 || dir === 3) px(c, h, 9, b - 7, 14, 6);
    else px(c, h, 13, b - 7, 6, dir === 1 ? 20 : 9);
    return;
  }
  if (s === 4) {
    px(c, h, 8, b - 7, 16, 2);
    px(c, h, 5, b - 5, 22, 8);
    if (dir === 1) px(c, h, 5, b + 3, 22, 9);
    else if (dir === 2) px(c, h, 15, b + 3, 12, 8);
    else if (dir === 3) px(c, h, 5, b + 3, 12, 8);
    else {
      px(c, h, 5, b + 3, 3, 6);
      px(c, h, 24, b + 3, 3, 6);
    }
    return;
  }
  px(c, h, 7, b - 3, 18, 5);
  if (dir === 1) px(c, h, 7, b - 3, 18, s === 1 ? 20 : 14);
  else if (dir === 2) px(c, h, 16, b - 3, 9, s === 1 ? 20 : 11);
  else if (dir === 3) px(c, h, 7, b - 3, 9, s === 1 ? 20 : 11);
  else {
    px(c, h, 7, b, 3, s === 1 ? 12 : 6);
    px(c, h, 22, b, 3, s === 1 ? 12 : 6);
  }
  if (s === 2) px(c, h, 10, b - 5, 12, 3);
  if (s === 7) px(c, h, 12, b - 8, 8, 5);
  if (s === 5) {
    if (dir === 0) px(c, h, 24, b + 2, 3, 9);
    else if (dir === 1) px(c, h, 14, b + 10, 4, 9);
    else if (dir === 2) px(c, h, 25, b + 1, 3, 11);
    else px(c, h, 4, b + 1, 3, 11);
  }
}

function drawFace(c, p, dir, b) {
  if (dir === 1) return;
  const h = p.hair, f = p.face;
  if (dir === 0) {
    px(c, INK, 12, b + 6, 2, 3);
    px(c, INK, 18, b + 6, 2, 3);
    if (f !== 1) {
      px(c, '#f29b9b66', 10, b + 9, 2, 1);
      px(c, '#f29b9b66', 20, b + 9, 2, 1);
    }
    if (f === 1) {
      px(c, h, 8, b + 9, 16, 4);
      px(c, h, 9, b + 13, 14, 1);
      px(c, p.skin, 13, b + 9, 6, 2);
    } else if (f === 2) {
      px(c, h, 12, b + 10, 8, 1);
      px(c, h, 11, b + 11, 2, 1);
      px(c, h, 19, b + 11, 2, 1);
    } else if (f === 3) px(c, h + '55', 8, b + 9, 16, 4);
    return;
  }
  const left = dir === 2;
  px(c, INK, left ? 10 : 20, b + 6, 2, 3);
  if (f === 1) px(c, h, left ? 8 : 15, b + 9, 9, 4);
  else if (f === 2) px(c, h, left ? 8 : 20, b + 10, 4, 1);
  else if (f === 3) px(c, h + '55', left ? 8 : 15, b + 9, 9, 4);
}

function drawExtra(c, p, dir, b) {
  const e = p.extra;
  if (e === 1 || e === 2) {
    if (dir === 1) return;
    const lens = (x) => {
      if (e === 2) {
        px(c, '#15171d', x, b + 5, 5, 5);
        px(c, '#ffffff44', x + 1, b + 6, 2, 1);
      } else {
        px(c, FRAME, x, b + 5, 5, 1);
        px(c, FRAME, x, b + 9, 5, 1);
        px(c, FRAME, x, b + 5, 1, 5);
        px(c, FRAME, x + 4, b + 5, 1, 5);
      }
    };
    if (dir === 0) {
      lens(10);
      lens(17);
      px(c, FRAME, 15, b + 6, 2, 1);
    } else {
      lens(dir === 2 ? 8 : 19);
      px(c, FRAME, 13, b + 6, 6, 1);
    }
  } else if (e === 3) {
    px(c, '#3b45e0', 7, b - 4, 18, 5);
    if (dir === 0) px(c, '#2a31a8', 8, b + 1, 16, 2);
    else if (dir === 1) px(c, '#2a31a8', 13, b + 1, 6, 1);
    else px(c, '#2a31a8', dir === 2 ? 3 : 20, b, 9, 2);
  } else if (e === 4) {
    px(c, '#f2c14e', 7, b - 5, 18, 5);
    px(c, '#d9a531', 7, b, 18, 2);
    px(c, '#ffffff', 14, b - 8, 4, 3);
  } else if (e === 5) {
    px(c, FRAME, 7, b - 5, 18, 2);
    if (dir < 2) {
      px(c, FRAME, 5, b + 2, 4, 8);
      px(c, FRAME, 23, b + 2, 4, 8);
      px(c, '#e5484d', 6, b + 4, 2, 4);
      px(c, '#e5484d', 24, b + 4, 2, 4);
    } else {
      px(c, FRAME, 13, b + 2, 7, 8);
      px(c, '#e5484d', 15, b + 4, 3, 4);
    }
  } else if (e === 6) {
    for (let r = 0; r < 8; r++) px(c, r % 3 === 1 ? '#f5d04c' : '#ec5f9b', 16 - ((r + 3) >> 1), b - 11 + r, r + 2, 1);
    px(c, '#ffffff', 15, b - 13, 2, 2);
  } else if (e === 7) {
    px(c, '#f2c14e', 8, b - 6, 16, 4);
    px(c, '#f2c14e', 8, b - 9, 3, 3);
    px(c, '#f2c14e', 14, b - 9, 4, 3);
    px(c, '#f2c14e', 21, b - 9, 3, 3);
    px(c, '#c9962a', 8, b - 3, 16, 1);
    px(c, '#e5484d', 15, b - 6, 2, 2);
  }
}

function drawArms(c, p, dir, b, pose) {
  if (pose === 1) {
    px(c, p.shirt, 4, b + 14, 5, 3);
    px(c, p.shirt, 23, b + 14, 5, 3);
    px(c, p.shirt, 3, b + 10, 3, 5);
    px(c, p.shirt, 26, b + 10, 3, 5);
    px(c, p.skin, 3, b + 8, 3, 2);
    px(c, p.skin, 26, b + 8, 3, 2);
  } else if (pose === 2) {
    px(c, p.shirt, 7, b + 14, 3, 4);
    px(c, p.shirt, 22, b + 14, 3, 4);
    if (dir === 1) return;
    px(c, p.shirt, 9, b + 16, 5, 3);
    px(c, p.shirt, 18, b + 16, 5, 3);
    px(c, p.skin, 13, b + 15, 6, 4);
  } else if (pose === 4 || pose === 5) {
    const up = pose === 4 ? 3 : 26, down = pose === 4 ? 23 : 6;
    px(c, p.skin, up, b - 7, 3, 21);
    px(c, p.shirt, up, b + 9, 3, 5);
    px(c, p.skin, down, b + 14, 3, 6);
  } else if (pose === 3) {
    px(c, p.shirt, 5, b + 6, 3, 10);
    px(c, p.shirt, 24, b + 6, 3, 10);
    px(c, p.skin, 5, b + 4, 3, 2);
    px(c, p.skin, 24, b + 4, 3, 2);
  } else {
    px(c, p.shirt, 6, b + 14, 3, 8);
    px(c, p.shirt, 23, b + 14, 3, 8);
    px(c, p.skin, 6, b + 22, 3, 2);
    px(c, p.skin, 23, b + 22, 3, 2);
  }
}

function drawBody(c, p, dir, step, sitting, pose) {
  const oy = sitting ? 4 : step === 1 ? -1 : 0;
  const b = 14 + oy;
  if (!sitting) {
    const l = step === 1 ? 2 : 0, r = step === 2 ? 2 : 0;
    px(c, p.pants, 11, 35 + oy - l, 4, 8);
    px(c, p.pants, 17, 35 + oy - r, 4, 8);
    px(c, p.shoes, 11, 42 + oy - l, 4, 2);
    px(c, p.shoes, 17, 42 + oy - r, 4, 2);
  }
  px(c, p.shirt, 9, b + 13, 14, 10);
  if (pose !== 2) drawArms(c, p, dir, b, pose);
  px(c, '#ffffff33', 10, b + 14, 12, 2);
  px(c, '#00000026', 9, b + 21, 14, 2);
  px(c, p.skin, 8, b, 16, 13);
  px(c, p.skin, 9, b + 13, 14, 1);
  drawFace(c, p, dir, b);
  drawHair(c, p, dir, b);
  drawExtra(c, p, dir, b);
  if (pose === 2) drawArms(c, p, dir, b, pose);
}

export function avatarSprite(av, dir, step, sitting, pose = 0) {
  const key = `${av}|${dir}|${step}|${sitting ? 1 : 0}|${pose}`;
  let cv = spriteCache.get(key);
  if (cv) return cv;
  const body = document.createElement('canvas');
  body.width = SPRITE_W;
  body.height = SPRITE_H;
  drawBody(body.getContext('2d'), palette(av), dir, step, sitting, pose);
  const sil = document.createElement('canvas');
  sil.width = SPRITE_W;
  sil.height = SPRITE_H;
  const s = sil.getContext('2d');
  s.drawImage(body, 0, 0);
  s.globalCompositeOperation = 'source-in';
  px(s, '#1b1e28', 0, 0, SPRITE_W, SPRITE_H);

  cv = document.createElement('canvas');
  cv.width = SPRITE_W;
  cv.height = SPRITE_H;
  const c = cv.getContext('2d');
  if (!sitting) {
    c.fillStyle = '#00000022';
    for (let dy = -3; dy <= 3; dy++) {
      const w = Math.round(Math.sqrt(Math.max(0, 1 - (dy * dy) / 9)) * 8);
      c.fillRect(16 - w, 44 + dy, w * 2, 1);
    }
  }
  for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) c.drawImage(sil, dx, dy);
  c.drawImage(body, 0, 0);
  if (spriteCache.size > 4000) spriteCache.clear();
  spriteCache.set(key, cv);
  return cv;
}
