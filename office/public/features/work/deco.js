export const T = 32;
export const HEAD = 16;
export const SPRITE_W = T * 2;
export const SPRITE_H = HEAD + T * 2;
export const SLOTS = ['top', 'top', 'screen', 'floor'];
export const SLOT_LABEL = ['Desk left', 'Desk right', 'Screen', 'Floor'];
export const DESK_COLORS = [null, ['#e3c08f', '#b98a5a'], ['#a9744a', '#7a4f2e'], ['#ffffff', '#d5dae3'], ['#4a5266', '#2c3240'], ['#bfe6d3', '#86c1a6']];
export const DESK_NAMES = ['Standard', 'Oak', 'Walnut', 'White', 'Charcoal', 'Mint'];
export const CHAIR_COLORS = [null, '#d9534f', '#4a90e2', '#3cc47c', '#9b6bdb', '#f5a623', '#ec5f9b', '#26b5a5'];
export const CHAIR_NAMES = ['Standard', 'Red', 'Blue', 'Green', 'Purple', 'Orange', 'Pink', 'Teal'];
export const ITEMS = [
  null,
  { name: 'Plant', kind: 'top' },
  { name: 'Coffee mug', kind: 'top' },
  { name: 'Desk lamp', kind: 'top' },
  { name: 'Books', kind: 'top' },
  { name: 'Rubber duck', kind: 'top' },
  { name: 'Photo frame', kind: 'top' },
  { name: 'Headphones', kind: 'top' },
  { name: 'Trophy', kind: 'top' },
  { name: 'Code editor', kind: 'screen' },
  { name: 'Sunset', kind: 'screen' },
  { name: 'Charts', kind: 'screen' },
  { name: 'Tall plant', kind: 'floor' },
  { name: 'Backpack', kind: 'floor' },
  { name: 'Cat', kind: 'floor' },
];
export const EMPTY = { desk: 0, chair: 0, slots: [0, 0, 0, 0] };

const int = (v) => (Number.isInteger(v) ? v : NaN);

export function cleanDeco(v) {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return undefined;
  const desk = int(v.desk ?? 0), chair = int(v.chair ?? 0);
  if (!(desk >= 0 && desk < DESK_COLORS.length) || !(chair >= 0 && chair < CHAIR_COLORS.length)) return undefined;
  if (!Array.isArray(v.slots) || v.slots.length !== SLOTS.length) return undefined;
  const slots = [];
  for (let i = 0; i < SLOTS.length; i++) {
    const id = int(v.slots[i]);
    if (id !== 0 && ITEMS[id]?.kind !== SLOTS[i]) return undefined;
    slots.push(id);
  }
  return { desk, chair, slots };
}

export function decoEmpty(d) {
  return !d || (d.desk === 0 && d.chair === 0 && d.slots.every((s) => s === 0));
}

export const itemsFor = (slot) => ITEMS.map((it, id) => ({ id, ...it })).filter((it) => it.id && it.kind === SLOTS[slot]);

export function shade(hex, k) {
  const n = parseInt(hex.slice(1), 16);
  const f = (v) => Math.max(0, Math.min(255, Math.round(v * k)));
  return '#' + ((f(n >> 16) << 16) | (f((n >> 8) & 255) << 8) | f(n & 255)).toString(16).padStart(6, '0');
}

function px(c, color, x, y, w, h) {
  c.fillStyle = color;
  c.fillRect(x, y, w, h);
}

const TOP = {
  1: (c, x, y) => {
    px(c, '#4f9f5c', x + 1, y - 12, 10, 6);
    px(c, '#79c77f', x + 3, y - 14, 6, 3);
    px(c, '#c9643a', x + 3, y - 6, 6, 6);
    px(c, '#e58555', x + 2, y - 7, 8, 2);
  },
  2: (c, x, y) => {
    px(c, '#ffffff', x + 3, y - 8, 6, 8);
    px(c, '#b5bcc7', x + 9, y - 6, 2, 4);
    px(c, '#7a4a2a', x + 4, y - 7, 4, 1);
    px(c, '#d5dae3', x + 3, y - 1, 6, 1);
  },
  3: (c, x, y) => {
    px(c, '#2c3240', x + 2, y - 2, 8, 2);
    px(c, '#2c3240', x + 5, y - 11, 2, 9);
    px(c, '#f2c14e', x + 2, y - 15, 8, 5);
    px(c, '#fff3b0', x + 3, y - 10, 6, 1);
  },
  4: (c, x, y) => {
    px(c, '#e56b6b', x + 1, y - 3, 10, 3);
    px(c, '#5b9bdf', x + 2, y - 6, 9, 3);
    px(c, '#62bf84', x + 1, y - 9, 10, 3);
    px(c, '#ffffff', x + 2, y - 2, 6, 1);
  },
  5: (c, x, y) => {
    px(c, '#f7d046', x + 1, y - 6, 8, 6);
    px(c, '#f7d046', x + 5, y - 10, 5, 5);
    px(c, '#f08a3c', x + 10, y - 8, 2, 2);
    px(c, '#1d1d24', x + 7, y - 9, 1, 1);
  },
  6: (c, x, y) => {
    px(c, '#8a6240', x + 1, y - 12, 10, 12);
    px(c, '#bfe3f5', x + 2, y - 11, 8, 6);
    px(c, '#62bf84', x + 2, y - 5, 8, 4);
    px(c, '#ffd66b', x + 7, y - 10, 2, 2);
  },
  7: (c, x, y) => {
    px(c, '#2a2d3a', x + 2, y - 11, 8, 2);
    px(c, '#2a2d3a', x + 1, y - 9, 3, 8);
    px(c, '#2a2d3a', x + 8, y - 9, 3, 8);
    px(c, '#e5484d', x + 2, y - 7, 1, 4);
    px(c, '#e5484d', x + 9, y - 7, 1, 4);
  },
  8: (c, x, y) => {
    px(c, '#f2c14e', x + 2, y - 13, 8, 6);
    px(c, '#f2c14e', x, y - 12, 2, 3);
    px(c, '#f2c14e', x + 10, y - 12, 2, 3);
    px(c, '#f2c14e', x + 5, y - 7, 2, 4);
    px(c, '#8a6240', x + 3, y - 3, 6, 3);
    px(c, '#fff3b0', x + 3, y - 12, 2, 3);
  },
};

const SCREEN = {
  9: (c, x, y) => {
    px(c, '#1d2030', x, y, 18, 13);
    px(c, '#9fd8fb', x + 2, y + 2, 9, 1);
    px(c, '#8b93ff', x + 4, y + 5, 10, 1);
    px(c, '#62e59a', x + 4, y + 8, 6, 1);
    px(c, '#f2c14e', x + 2, y + 10, 8, 1);
  },
  10: (c, x, y) => {
    px(c, '#3b2a6b', x, y, 18, 5);
    px(c, '#e8588a', x, y + 5, 18, 3);
    px(c, '#f5a623', x, y + 8, 18, 2);
    px(c, '#ffd66b', x + 6, y + 4, 6, 4);
    px(c, '#2bb7c9', x, y + 10, 18, 3);
  },
  11: (c, x, y) => {
    px(c, '#ffffff', x, y, 18, 13);
    px(c, '#5b9bdf', x + 2, y + 7, 3, 4);
    px(c, '#62bf84', x + 6, y + 4, 3, 7);
    px(c, '#f2c14e', x + 10, y + 6, 3, 5);
    px(c, '#e56b6b', x + 14, y + 2, 3, 9);
    px(c, '#b5bcc7', x + 1, y + 11, 16, 1);
  },
};

const FLOOR = {
  12: (c, x, y) => {
    px(c, '#00000018', x + 9, y + 28, 14, 3);
    px(c, '#4f9f5c', x + 8, y + 4, 16, 14);
    px(c, '#62b56c', x + 11, y - 2, 10, 10);
    px(c, '#79c77f', x + 13, y - 6, 6, 6);
    px(c, '#474f5f', x + 11, y + 18, 10, 11);
    px(c, '#565f71', x + 12, y + 19, 8, 2);
  },
  13: (c, x, y) => {
    px(c, '#00000018', x + 8, y + 28, 16, 3);
    px(c, '#22252e', x + 13, y + 9, 6, 3);
    px(c, '#3b6fd0', x + 9, y + 12, 14, 17);
    px(c, '#2f55b0', x + 11, y + 18, 10, 7);
    px(c, '#7fa4f5', x + 12, y + 14, 8, 1);
  },
  14: (c, x, y) => {
    px(c, '#00000018', x + 6, y + 28, 20, 3);
    px(c, '#d98a45', x + 4, y + 16, 3, 10);
    px(c, '#f0a35c', x + 7, y + 19, 16, 10);
    px(c, '#f0a35c', x + 17, y + 12, 10, 9);
    px(c, '#f0a35c', x + 17, y + 9, 3, 3);
    px(c, '#f0a35c', x + 24, y + 9, 3, 3);
    px(c, '#1d1d24', x + 19, y + 15, 1, 2);
    px(c, '#1d1d24', x + 24, y + 15, 1, 2);
    px(c, '#ffffff', x + 20, y + 18, 4, 2);
  },
};

function drawSurface(c, x, y, top, edge) {
  px(c, edge, x, y + 8, T, 20);
  px(c, top, x + 1, y + 9, T - 2, 12);
  px(c, shade(top, 0.9), x + 1, y + 21, T - 2, 6);
  px(c, shade(edge, 0.85), x + 4, y + 24, 8, 1);
  px(c, shade(edge, 0.85), x + 20, y + 24, 8, 1);
}

function drawMonitor(c, x, y) {
  px(c, '#262b37', x + 6, y - 5, 20, 15);
  px(c, '#5fb6ee', x + 7, y - 4, 18, 13);
  px(c, '#9fd8fb', x + 8, y - 3, 6, 3);
  px(c, '#3f97d6', x + 8, y + 6, 16, 2);
  px(c, '#262b37', x + 14, y + 10, 4, 3);
  px(c, '#262b37', x + 7, y + 14, 14, 4);
  px(c, '#3a4150', x + 8, y + 15, 12, 2);
  px(c, '#e8ebf0', x + 23, y + 14, 3, 4);
}

function drawChair(c, x, y, color) {
  const dark = shade(color, 0.72);
  px(c, dark, x + 8, y + 13, 16, 12);
  px(c, color, x + 9, y + 14, 14, 10);
  px(c, shade(color, 1.15), x + 10, y + 15, 12, 4);
  px(c, shade(color, 0.5), x + 7, y + 21, 18, 8);
  px(c, dark, x + 8, y + 22, 16, 6);
}

export function drawDeco(c, deco) {
  const y = HEAD;
  const desk = DESK_COLORS[deco.desk];
  const [left, right, screen, floor] = deco.slots;
  const side = desk || left || right;
  if (desk) {
    drawSurface(c, 0, y, desk[0], desk[1]);
    drawMonitor(c, 0, y);
  }
  if (side) drawSurface(c, T, y, ...(desk || ['#fafbfc', '#a4abb8']));
  if (SCREEN[screen]) SCREEN[screen](c, 7, y - 4);
  if (TOP[left]) TOP[left](c, T + 3, y + 20);
  if (TOP[right]) TOP[right](c, T + 17, y + 20);
  if (CHAIR_COLORS[deco.chair]) drawChair(c, 0, y + T, CHAIR_COLORS[deco.chair]);
  if (FLOOR[floor]) FLOOR[floor](c, T, y + T);
}

export const CROP = {
  top: [T + 1, HEAD - 6, 14, 28],
  screen: [5, HEAD - 6, 22, 17],
  floor: [T, HEAD + T - 8, T, T + 8],
};

export function sampleFor(id) {
  const slots = [0, 0, 0, 0];
  const kind = ITEMS[id]?.kind;
  slots[SLOTS.indexOf(kind)] = id;
  return { desk: 0, chair: 0, slots };
}
