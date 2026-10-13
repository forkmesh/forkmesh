import { TILE, W, H, OFFICE_W, PARK_X0, floor, obj, LABELS, ROOMS, DECOR, AGENT_STATIONS } from './map.js';
import { hash, px, box, disc, tri } from './draw.js';
import { PARK_FLOORS, PARK_OBJECTS, ROAD, drawParkArt, drawRoad } from './park/art.js';

const T = TILE;
export const COLORS = { grass: '#d9ecd4' };

const FLOORS = {
  '.': (c, x, y) => {
    px(c, '#f3e5cb', 0, 0, T, T);
    for (let i = 0; i < 4; i++) {
      px(c, '#e9d6b3', 0, i * 8 + 7, T, 1);
      px(c, '#e6d1ab', ((i * 13 + x * 7 + y * 3) % 4) * 8 + 4, i * 8, 1, 7);
    }
  },
  l: (c, x, y) => {
    px(c, '#f5ede1', 0, 0, T, T);
    px(c, '#ebe0cf', 0, 0, T, 1);
    px(c, '#ebe0cf', 0, 0, 1, T);
    px(c, '#ece2d3', 15, 11, 2, 10);
    px(c, '#ece2d3', 11, 15, 10, 2);
  },
  c: (c) => {
    c.beginPath();
    c.rect(0, 0, T, T);
    c.clip();
    const l = '#e9ebf6', d = '#d3d6e8';
    px(c, '#dfe1ef', 0, 0, T, T);
    for (const [cx, cy] of [[0, 0], [T, 0], [0, T], [T, T]]) {
      disc(c, cx, cy, 7, l);
      px(c, d, cx - 2, cy - 2, 4, 4);
    }
    px(c, l, 14, 4, 4, 24);
    px(c, l, 4, 14, 24, 4);
    px(c, l, 11, 11, 10, 10);
    px(c, d, 13, 13, 6, 6);
    px(c, l, 15, 15, 2, 2);
    for (const [dx, dy] of [[8, 8], [22, 8], [8, 22], [22, 22]]) px(c, d, dx, dy, 2, 2);
  },
  a: (c, x, y) => {
    px(c, '#9ca2de', 0, 0, T, T);
    for (let i = 0; i < 4; i++) px(c, '#a8aee6', ((i + y) % 2) * 16 + ((x % 2) * 8), i * 8, 16, 4);
  },
  b: (c, x, y) => {
    px(c, '#9bcbcf', 0, 0, T, T);
    for (let i = 0; i < 4; i++) px(c, '#a8d5d8', ((i + y) % 2) * 16 + ((x % 2) * 8), i * 8, 16, 4);
  },
  p: (c, x, y) => {
    px(c, '#e7f0e1', 0, 0, T, T);
    px(c, '#d9e6d2', 0, 0, T, 1);
    px(c, '#d9e6d2', (y % 2) * 16, 0, 1, T);
    if (hash(x, y, 9) > 0.7) px(c, '#cfe0c7', 6, 20, 3, 2);
    const g = (dx, dy) => (floor[y + dy]?.[x + dx] ?? 'g') === 'g';
    if (g(0, -1)) px(c, '#b5d6ad', 0, 0, T, 3);
    if (g(-1, 0)) px(c, '#b5d6ad', 0, 0, 3, T);
    if (g(1, 0)) px(c, '#b5d6ad', T - 3, 0, 3, T);
  },
  q: (c, x, y) => {
    px(c, '#30364f', 0, 0, T, T);
    px(c, '#3a4160', 0, 0, T, 1);
    px(c, '#3a4160', 0, 0, 1, T);
    if (STATION_SET.has(y * 1000 + x)) {
      disc(c, 16, 17, 12, '#262b40');
      disc(c, 16, 17, 10, '#3b45e0');
      disc(c, 16, 17, 8, '#262b40');
      disc(c, 16, 17, 3, '#5b8def');
    } else if ((x + y) % 4 === 0) px(c, '#5b8def55', 15, 15, 2, 2);
  },
  h: (c, x, y) => {
    px(c, '#d9c39b', 0, 0, T, T);
    if (hash(x, y, 5) > 0.5) px(c, '#cdb68b', 4 + ((x * 5) % 18), 6 + ((y * 7) % 16), 6, 2);
    if (hash(x, y, 6) > 0.6) px(c, '#e3d0ab', 18 - ((x * 3) % 10), 22 - ((y * 5) % 12), 4, 2);
    const g = (dx, dy) => (floor[y + dy]?.[x + dx] ?? 'g') === 'g';
    if (g(0, -1)) px(c, '#b5d6ad', 0, 0, T, 3);
    if (g(0, 1)) px(c, '#b5d6ad', 0, T - 3, T, 3);
    if (g(1, 0)) px(c, '#b5d6ad', T - 3, 0, 3, T);
  },
  k: (c) => {
    px(c, '#bac0c7', 0, 0, T, T);
    for (let i = 0; i < 4; i++) for (let j = 0; j < 4; j++) px(c, '#c2c8ce', j * 8, i * 8 + ((i + j) % 2) * 4, 8, 4);
  },
  m: (c) => {
    px(c, '#ecd9bd', 0, 0, T, T);
    for (let i = 0; i < 4; i++) {
      px(c, '#dfc9a6', 0, i * 8 + 7, T, 1);
      px(c, '#dfc9a6', (i % 2) * 8 + 7, i * 8, 1, 7);
      px(c, '#dfc9a6', (i % 2) * 8 + 23, i * 8, 1, 7);
    }
  },
  s: (c, x, y) => {
    px(c, '#ebe9f5', 0, 0, T, T);
    for (let i = 0; i < T; i += 2) {
      px(c, '#dad8ec', i, i, 2, 2);
      px(c, '#dad8ec', T - 2 - i, i, 2, 2);
    }
    const e = (dx, dy) => floor[y + dy]?.[x + dx] !== 's';
    if (e(0, -1)) px(c, '#cdcbe4', 0, 0, T, 3);
    if (e(0, 1)) px(c, '#cdcbe4', 0, T - 3, T, 3);
    if (e(-1, 0)) px(c, '#cdcbe4', 0, 0, 3, T);
    if (e(1, 0)) px(c, '#cdcbe4', T - 3, 0, 3, T);
  },
  w: (c, x, y) => {
    px(c, '#aab4d4', 0, 0, T, T);
    for (let i = 0; i < 4; i++) px(c, '#b5bfdc', ((i + y) % 2) * 16 + ((x % 2) * 8), i * 8, 16, 4);
  },
  f: (c, x) => {
    px(c, '#b9bfc4', 0, 0, T, T);
    px(c, '#c4cacf', (x % 2) * 16, 0, 8, T);
    px(c, '#c4cacf', (x % 2) * 16 + 20, 0, 2, T);
  },
  g: (c, x, y) => {
    px(c, COLORS.grass, 0, 0, T, T);
    for (let i = 0; i < 3; i++) {
      const r = hash(x, y, i);
      if (r < 0.45) continue;
      const tx = (hash(y, x, i) * 26) | 0, ty = (hash(x + 7, y, i) * 26) | 0;
      px(c, '#c9e2c3', tx, ty + 2, 1, 3);
      px(c, '#c9e2c3', tx + 2, ty, 1, 5);
      px(c, '#c9e2c3', tx + 4, ty + 2, 1, 3);
    }
    if (hash(x, y, 7) > 0.96) {
      px(c, '#ffffff', 14, 14, 3, 3);
      px(c, '#f2c94c', 15, 15, 1, 1);
    }
  },
};

const isWall = (x, y) => floor[y]?.[x] === '#';
const outside = (x, y) => (floor[y]?.[x] ?? 'g') === 'g';
const WALL_FACE = { k: ['#5a656e', '#4c565e', '#73808a'], m: ['#7f7bbd', '#6c68a8', '#9b97d4'] };
const hung = (x, y) => DECOR.some((d) => d.y === y && Math.abs(d.x - x - 0.5) < 1.5);

function drawWall(c, x, y) {
  const belowInside = !isWall(x, y + 1) && !outside(x, y + 1);
  px(c, '#4d5563', 0, 0, T, T);
  px(c, '#5a6271', 1, 1, T - 2, T - 2);
  if (!isWall(x, y - 1)) px(c, '#3d4450', 0, 0, T, 2);
  if (belowInside) {
    const run = isWall(x - 1, y) || isWall(x + 1, y);
    const [face, foot, lip] = (run && WALL_FACE[floor[y + 1][x]]) || ['#747d8c', '#666f7e', '#8a93a1'];
    px(c, face, 0, 8, T, 24);
    px(c, foot, 0, 28, T, 4);
    px(c, lip, 0, 8, T, 1);
    if (outside(x, y - 1) && x % 3 !== 0 && !hung(x, y)) {
      px(c, '#39404b', 2, 10, 28, 17);
      px(c, '#a9dde6', 4, 12, 11, 13);
      px(c, '#a9dde6', 17, 12, 11, 13);
      px(c, '#d7f1f5', 5, 13, 3, 5);
      px(c, '#d7f1f5', 18, 13, 3, 5);
    }
  } else if (!isWall(x, y + 1)) {
    px(c, '#5e6776', 0, 22, T, 10);
    px(c, '#4a515e', 0, 30, T, 2);
  }
}

const cafe = (x, y) => floor[y]?.[x] === 'c';
const WOOD = ['#efd09c', '#dcae78', '#a8693f'];

function woodChair(c, dir) {
  const [l, m, d] = WOOD;
  px(c, '#00000014', 6, 28, 20, 3);
  if (dir === 0) {
    px(c, d, 7, 24, 3, 6);
    px(c, d, 22, 24, 3, 6);
    box(c, 6, 11, 20, 15, l, d);
    px(c, m, 7, 22, 18, 3);
    box(c, 6, 1, 20, 12, m, d);
    px(c, l, 8, 3, 16, 3);
    px(c, d, 8, 8, 16, 1);
    return;
  }
  if (dir === 1) {
    px(c, d, 7, 25, 3, 6);
    px(c, d, 22, 25, 3, 6);
    box(c, 6, 4, 20, 14, l, d);
    box(c, 6, 15, 20, 13, m, d);
    px(c, l, 8, 17, 16, 3);
    px(c, d, 8, 22, 16, 1);
    return;
  }
  if (dir === 2) {
    c.translate(T, 0);
    c.scale(-1, 1);
  }
  px(c, d, 9, 25, 3, 6);
  px(c, d, 22, 25, 3, 6);
  box(c, 8, 11, 18, 15, l, d);
  px(c, m, 9, 22, 16, 3);
  box(c, 4, 0, 7, 26, m, d);
  px(c, l, 6, 2, 2, 21);
}

const SEAT_TONES = {
  k: ['#5a9bea', '#8a5a36', '#86b9f5', '#3f7bd6', '#8a5a36'],
  m: ['#e25652', '#a23532', '#f08580', '#cb4541', '#8f2a28'],
};

function logSeat(c, dir) {
  const side = dir > 1;
  px(c, '#00000018', 4, 26, 24, 4);
  if (side) {
    box(c, 9, 2, 14, 27, '#8a5a36', '#5e3b22');
    px(c, '#a5703f', 11, 4, 3, 23);
    disc(c, 16, 3, 5, '#c99a64');
    disc(c, 16, 3, 3, '#a87a48');
  } else {
    box(c, 2, 11, 28, 14, '#8a5a36', '#5e3b22');
    px(c, '#a5703f', 4, 13, 24, 3);
    disc(c, 3, 18, 5, '#c99a64');
    disc(c, 3, 18, 3, '#a87a48');
    disc(c, 29, 18, 5, '#c99a64');
    disc(c, 29, 18, 3, '#a87a48');
  }
}

const STATION_SET = new Set(AGENT_STATIONS.map(([x, y]) => y * 1000 + x));

function agentConsole(c) {
  px(c, '#00000022', 3, 27, 26, 4);
  box(c, 3, 8, 26, 20, '#3d4560', '#1f2333');
  box(c, 5, -6, 22, 15, '#1f2333', '#14171f');
  px(c, '#3b45e0', 7, -4, 18, 11);
  for (let i = 0; i < 3; i++) px(c, ['#22c55e', '#f59e0b', '#5b8def'][i], 9, -2 + i * 3, 8 + i * 2, 2);
  for (let i = 0; i < 4; i++) disc(c, 8 + i * 5, 17, 2, ['#e5484d', '#22c55e', '#f5d04c', '#5b8def'][i]);
  px(c, '#8a93ad', 6, 22, 20, 2);
}

function coasterGate(c) {
  px(c, '#00000022', 4, 27, 24, 4);
  box(c, 5, 6, 4, 22, '#b9553f', '#7a3424');
  box(c, 23, 6, 4, 22, '#b9553f', '#7a3424');
  box(c, 3, -8, 26, 14, '#f5d04c', '#b98a1f');
  px(c, '#e5484d', 6, -5, 20, 2);
  px(c, '#3b45e0', 6, -1, 20, 2);
  px(c, '#e5484d', 6, 3, 20, 2);
  px(c, '#7a4a2a', 9, 16, 14, 3);
}

function firePit(c) {
  disc(c, 16, 18, 15, '#00000014');
  for (let i = 0; i < 10; i++) {
    const a = (i / 10) * Math.PI * 2;
    const sx = Math.round(16 + Math.cos(a) * 12), sy = Math.round(17 + Math.sin(a) * 10);
    disc(c, sx, sy, 4, '#8e979e');
    disc(c, sx - 1, sy - 1, 2, '#b3bbc1');
  }
  disc(c, 16, 17, 8, '#3b2a20');
  box(c, 8, 15, 16, 4, '#7a4a2a', '#4f2f1a');
  box(c, 14, 10, 4, 14, '#6d4126', '#4f2f1a');
  disc(c, 16, 17, 4, '#f08a3c');
  disc(c, 16, 17, 2, '#ffd166');
}

function chair(c, dir, x, y) {
  if (floor[y]?.[x] === 'h') return logSeat(c, dir);
  if (cafe(x, y)) return woodChair(c, dir);
  const [seat, edge, shine, back, rim] = SEAT_TONES[floor[y]?.[x]] || ['#4a5266', '#2c3240', '#58617a', '#3a4254', '#262b37'];
  px(c, '#00000014', 7, 27, 18, 3);
  box(c, 8, 13, 16, 12, seat, edge);
  px(c, shine, 10, 15, 12, 4);
  if (dir === 0) box(c, 7, 4, 18, 11, back, rim);
  else if (dir === 1) box(c, 7, 21, 18, 8, back, rim);
  else if (dir === 2) box(c, 21, 8, 6, 17, back, rim);
  else box(c, 5, 8, 6, 17, back, rim);
}

function desk(c, monitor, x) {
  px(c, '#00000014', 2, 27, 28, 4);
  box(c, 0, 8, T, 20, '#e2e6ed', '#a4abb8');
  px(c, '#fafbfc', 1, 9, 30, 12);
  px(c, '#cfd5de', 1, 21, 30, 1);
  px(c, '#b9c0cc', 4, 24, 8, 1);
  px(c, '#b9c0cc', 20, 24, 8, 1);
  if (monitor) {
    box(c, 6, -5, 20, 15, '#5fb6ee', '#262b37');
    px(c, '#9fd8fb', 8, -3, 6, 3);
    px(c, '#3f97d6', 8, 6, 16, 2);
    px(c, '#262b37', 14, 10, 4, 3);
    box(c, 7, 14, 14, 4, '#3a4150', '#262b37');
    px(c, '#e8ebf0', 23, 14, 3, 4);
  } else {
    px(c, x % 2 ? '#f2d38b' : '#f4a7a7', 6, 11, 8, 6);
    box(c, 19, 10, 6, 8, '#8fc3ee', '#5a7ea0');
  }
}

function leafPlant(c) {
  px(c, '#00000018', 8, 28, 16, 3);
  px(c, '#3f8f4f', 15, -2, 2, 20);
  disc(c, 9, 4, 5, '#3f9a54', 1.7);
  disc(c, 23, 0, 5, '#3f9a54', 1.7);
  disc(c, 16, -12, 5, '#5fb86a', 1.8);
  disc(c, 20, 8, 4, '#5fb86a', 1.4);
  disc(c, 11, -6, 3, '#5fb86a', 1.6);
  disc(c, 8, 3, 2, '#8fd88c', 2);
  disc(c, 22, -2, 2, '#8fd88c', 2);
  disc(c, 15, -13, 2, '#8fd88c', 2);
  box(c, 8, 16, 16, 5, '#e58555', '#a84f2b');
  px(c, '#f3a47a', 9, 17, 14, 1);
  px(c, '#c9643a', 10, 21, 12, 8);
  px(c, '#a84f2b', 10, 21, 12, 1);
  px(c, '#a84f2b', 11, 28, 10, 1);
}

function plant(c, x, y) {
  if (cafe(x, y)) return leafPlant(c);
  px(c, '#00000018', 8, 28, 16, 3);
  box(c, 10, 18, 12, 11, '#474f5f', '#2e343f');
  px(c, '#565f71', 11, 19, 10, 2);
  const g = ['#4f9f5c', '#62b56c', '#79c77f'];
  for (let i = 0; i < 6; i++) {
    const r = hash(x, y, i), r2 = hash(y, x, i);
    disc(c, 9 + r * 14, 3 + r2 * 12, 5, g[i % 3]);
  }
  disc(c, 13, 6, 2, '#94d997');
}

function roundTable(c) {
  disc(c, 16, 28, 11, '#00000018', 0.3);
  px(c, '#6b7486', 14, 16, 4, 10);
  disc(c, 16, 26, 7, '#6b7486', 0.45);
  disc(c, 16, 25, 5, '#7d8799', 0.45);
  disc(c, 16, 15, 15, '#77839a', 0.78);
  disc(c, 16, 12, 15, '#a3aec2', 0.78);
  disc(c, 15, 11, 12, '#b3bdcf', 0.74);
  box(c, 8, 4, 8, 9, '#4a7bea', '#2f55b0');
  px(c, '#7fa4f5', 10, 5, 4, 2);
  px(c, '#2f55b0', 6, 6, 2, 5);
  box(c, 17, 7, 7, 10, '#3b404d', '#22252e');
  px(c, '#5a6071', 19, 8, 3, 2);
  px(c, '#22252e', 24, 9, 2, 5);
}

function table(c, x, y) {
  const n = (dx, dy) => obj[y + dy]?.[x + dx] === 'T';
  if (cafe(x, y) && !n(1, 0) && !n(-1, 0) && !n(0, 1) && !n(0, -1)) return roundTable(c);
  if (floor[y][x] === 'k') return readingTable(c);
  if (floor[y][x] === 'm') return mediaTable(c, x, y, n);
  const l = n(-1, 0) ? 0 : 3, r = n(1, 0) ? 0 : 3, t = n(0, -1) ? 0 : 3, b = n(0, 1) ? 0 : 6;
  px(c, '#a26d3e', l, t, T - l - r, T - t - b + (b ? 3 : 0));
  px(c, '#d9a56b', l + (l ? 1 : 0), t + (t ? 1 : 0), T - l - r - (l ? 1 : 0) - (r ? 1 : 0), T - t - b - (t ? 1 : 0));
  if (!n(0, -1) && !n(-1, 0)) {
    box(c, 12, 10, 7, 7, '#ffffff', '#b5bcc7');
    px(c, '#7a4a2a', 14, 12, 3, 3);
  }
  if (!n(0, 1) && n(1, 0)) px(c, '#e7bb86', 6, 8, 20, 2);
}

function bush(c, x, y) {
  px(c, '#00000012', 4, 26, 24, 4);
  disc(c, 10, 18, 9, '#4f9f5c');
  disc(c, 22, 18, 9, '#4f9f5c');
  disc(c, 16, 12, 10, '#62b56c');
  disc(c, 13, 9, 4, '#7fcb82');
  if (hash(x, y) > 0.5) {
    px(c, '#f6a5c0', 8, 14, 2, 2);
    px(c, '#f6a5c0', 22, 11, 2, 2);
  }
}

function pond(c, x, y) {
  const n = (dx, dy) => obj[y + dy]?.[x + dx] === 'W';
  px(c, '#4fb0d6', 0, 0, T, T);
  px(c, '#6cc4e4', 4 + ((x * 7) % 12), 8 + ((y * 5) % 10), 8, 1);
  const rock = (rx, ry) => { disc(c, rx, ry, 4, '#8e979e'); disc(c, rx - 1, ry - 1, 2, '#b3bbc1'); };
  if (!n(0, -1)) for (let i = 2; i < T; i += 7) rock(i, 1);
  if (!n(0, 1)) for (let i = 2; i < T; i += 7) rock(i, T - 2);
  if (!n(-1, 0)) for (let i = 2; i < T; i += 7) rock(1, i);
  if (!n(1, 0)) for (let i = 2; i < T; i += 7) rock(T - 2, i);
  if (hash(x, y, 3) > 0.5) disc(c, 16, 16, 4, '#5aa35d'), px(c, '#4fb0d6', 16, 14, 4, 2);
  if (hash(x, y, 4) > 0.4) px(c, '#f08a3c', 8, 20, 5, 2), px(c, '#ffffff', 9, 20, 1, 1);
}

function pine(c) {
  disc(c, 16, 29, 13, '#c3dfbf', 0.35);
  box(c, 12, 14, 8, 16, '#a4623d', '#7a4529');
  tri(c, 16, -40, 22, 13, '#3d8a58');
  tri(c, 16, -28, 24, 18, '#3d8a58');
  tri(c, 16, -14, 30, 23, '#3d8a58');
  tri(c, 14, -38, 18, 8, '#52a36c');
  tri(c, 13, -25, 18, 11, '#52a36c');
  tri(c, 12, -10, 22, 14, '#52a36c');
  px(c, '#2f7348', -7, 14, 46, 2);
}

function roundTree(c, birch) {
  disc(c, 16, 29, 17, '#c3dfbf', 0.3);
  if (birch) {
    box(c, 12, 2, 8, 28, '#eef1f1', '#aab4b6');
    px(c, '#6e7a7d', 14, 9, 3, 1);
    px(c, '#6e7a7d', 16, 18, 3, 1);
    px(c, '#eef1f1', 18, -2, 8, 5);
  } else box(c, 11, 0, 10, 30, '#9a5b37', '#6e3f25');
  const [d, m, l] = birch ? ['#5eae59', '#77c96d', '#97de89'] : ['#3f8a55', '#529f64', '#6db77a'];
  const blobs = [[4, -18, 14], [28, -18, 14], [16, -32, 16], [8, -6, 11], [25, -5, 11]];
  for (const [bx, by, r] of blobs) disc(c, bx, by + 3, r, d);
  for (const [bx, by, r] of blobs) disc(c, bx - 1, by, r - 2, m);
  for (const [bx, by, r] of blobs) disc(c, bx - 4, by - 4, r / 3, l);
}

function counter(c, x, y) {
  const n = (dx) => 'KC'.includes(obj[y][x + dx] || '-');
  px(c, '#00000014', 0, 29, T, 3);
  px(c, '#eef0f7', 0, -6, T, 16);
  px(c, '#fbfcfe', 0, -6, T, 2);
  px(c, '#cdd2df', 0, 10, T, 2);
  px(c, '#98a1b3', 0, 12, T, 17);
  px(c, '#7c8597', 0, 27, T, 2);
  box(c, 2, 14, 28, 11, '#a8b0c0', '#808a9c');
  for (const hx of [9, 21]) {
    px(c, '#eef0f7', hx - 1, 17, 4, 1);
    px(c, '#eef0f7', hx, 17, 2, 5);
  }
  if (!n(-1)) px(c, '#808a9c', 0, -6, 1, 35);
  if (!n(1)) {
    px(c, '#808a9c', T - 1, -6, 1, 35);
    box(c, 6, -3, 19, 10, '#c5ccd9', '#8b94a6');
    px(c, '#aab3c2', 8, -1, 15, 6);
    px(c, '#8b94a6', 14, -9, 3, 7);
    px(c, '#8b94a6', 14, -9, 7, 2);
  } else if (obj[y][x + 1] === 'K') {
    box(c, 7, -2, 6, 7, '#ffffff', '#b5bcc7');
    box(c, 15, -2, 6, 7, '#f6c9a8', '#c98f6d');
    px(c, '#7a4a2a', 9, 0, 2, 2);
  }
}

function couch(c, x, y) {
  const n = (dx) => obj[y][x + dx] === 'S';
  px(c, '#00000016', 0, 28, T, 3);
  px(c, '#b9553f', 0, -8, T, 36);
  px(c, '#d86f52', 0, -7, T, 16);
  px(c, '#e58a6c', 0, -7, T, 3);
  px(c, '#f09a7c', 1, 10, T - 2, 13);
  px(c, '#f6b196', 2, 11, T - 4, 3);
  px(c, '#b9553f', 0, 23, T, 1);
  px(c, '#cf6649', 0, 24, T, 4);
  if (!n(-1)) {
    box(c, 0, -2, 7, 30, '#d86f52', '#a94a36');
    px(c, '#e58a6c', 1, -1, 5, 3);
  }
  if (!n(1)) {
    box(c, T - 7, -2, 7, 30, '#d86f52', '#a94a36');
    px(c, '#e58a6c', T - 6, -1, 5, 3);
  }
}

function readingTable(c) {
  disc(c, 16, 28, 11, '#00000018', 0.3);
  px(c, '#7c8591', 14, 16, 4, 10);
  disc(c, 16, 26, 7, '#7c8591', 0.45);
  disc(c, 16, 25, 5, '#8e97a3', 0.45);
  disc(c, 16, 15, 15, '#8f99a5', 0.78);
  disc(c, 16, 12, 15, '#c6cdd5', 0.78);
  disc(c, 15, 11, 12, '#d4dae0', 0.74);
  box(c, 4, 6, 12, 10, '#fbfcfd', '#aeb6c0');
  px(c, '#aeb6c0', 9, 7, 1, 8);
  for (const lx of [6, 11]) for (const ly of [9, 11, 13]) px(c, '#d5dae1', lx, ly, 3, 1);
  box(c, 16, 3, 10, 12, '#4fae6a', '#2f7d48');
  px(c, '#7fd197', 18, 5, 6, 1);
  px(c, '#e8f6ec', 18, 9, 6, 2);
}

const GADGETS = [
  (c) => {
    box(c, 6, 3, 20, 14, '#2a2f3b', '#151821');
    px(c, '#e25652', 8, 5, 7, 3);
    px(c, '#4fae6a', 16, 5, 8, 3);
    px(c, '#f2c14e', 8, 9, 16, 4);
    px(c, '#151821', 13, 17, 6, 2);
    px(c, '#8d94a3', 10, 19, 12, 2);
  },
  (c) => {
    box(c, 7, 4, 18, 12, '#3a3f4d', '#1f232d');
    px(c, '#62e59a', 9, 6, 6, 1);
    px(c, '#f2c14e', 9, 8, 9, 1);
    px(c, '#8fc3ee', 9, 10, 5, 1);
    box(c, 5, 16, 22, 6, '#c5ccd9', '#8b94a6');
    for (let i = 0; i < 6; i++) px(c, '#8b94a6', 7 + i * 3, 18, 2, 2);
  },
  (c) => {
    box(c, 5, 4, 13, 16, '#f7f8fb', '#9aa1b0');
    for (const ly of [7, 10, 13]) px(c, '#cfd5de', 7, ly, 9, 1);
    box(c, 20, 8, 7, 11, '#dfe4ec', '#8b94a6');
    px(c, '#8fc3ee', 21, 10, 5, 6);
  },
  (c) => {
    box(c, 7, 4, 18, 15, '#5fa3ea', '#346fb5');
    px(c, '#d7ecfd', 9, 6, 9, 6);
    px(c, '#346fb5', 20, 7, 3, 3);
    px(c, '#ffffff', 9, 15, 14, 2);
  },
];

function mediaTable(c, x, y, n) {
  const l = n(-1, 0) ? 0 : 2, r = n(1, 0) ? 0 : 2, t = n(0, -1) ? 0 : 2, b = n(0, 1) ? 0 : 8;
  if (b) {
    px(c, '#00000014', l, T - 3, T - l - r, 3);
    if (l) px(c, '#7c8598', l + 2, T - 6, 3, 6);
    if (r) px(c, '#7c8598', T - r - 5, T - 6, 3, 6);
  }
  px(c, '#9aa2b3', l, t, T - l - r, T - t - b + (b ? 4 : 0));
  px(c, '#f0f1f8', l + (l ? 1 : 0), t + (t ? 1 : 0), T - l - r - (l ? 1 : 0) - (r ? 1 : 0), T - t - b - (t ? 1 : 0));
  GADGETS[(x * 3 + y * 5) % GADGETS.length](c);
}

const SOFA = ['#4f93e3', '#3a74c9', '#74aef0', '#2a57a3'];
const ARMCHAIR = ['#4a5264', '#394050', '#626c83', '#252a36'];

function lounge(c, x, y, dir) {
  const ch = obj[y][x];
  const j = (k) => (dir < 2 ? obj[y][x + k] : obj[y + k]?.[x]) === ch;
  const a = j(-1), b = j(1);
  const [seat, back, shine, edge] = a || b ? SOFA : ARMCHAIR;
  const R = (col, u, v, w, h) => {
    if (dir === 0) px(c, col, u, v, w, h);
    else if (dir === 1) px(c, col, u, T - v - h, w, h);
    else if (dir === 3) px(c, col, v, u, h, w);
    else px(c, col, T - v - h, u, h, w);
  };
  const u0 = a ? 0 : 2, u1 = b ? T : T - 2, i0 = u0 + (a ? 0 : 1), iw = u1 - (b ? 0 : 1) - i0;
  px(c, '#00000014', 2, 29, 28, 3);
  R(edge, u0, 1, u1 - u0, 29);
  R(back, i0, 2, iw, 9);
  R(shine, i0, 2, iw, 2);
  R(seat, i0, 12, iw, 17);
  R(shine, i0 + 3, 14, iw - 6, 2);
  if (!a) R(back, i0, 9, 5, 20), R(shine, i0, 9, 5, 2);
  if (!b) R(back, i0 + iw - 5, 9, 5, 20), R(shine, i0 + iw - 5, 9, 5, 2);
  if (b) R(edge, T - 1, 12, 1, 17);
}

const ART = {
  mondrian: (c) => {
    box(c, -25, 8, 50, 22, '#f6f3ec', '#23262f');
    px(c, '#d94a3d', -15, 10, 23, 11);
    px(c, '#f2c94c', 10, 10, 13, 6);
    px(c, '#3355b5', 2, 23, 14, 5);
    px(c, '#f2c94c', -23, 23, 8, 5);
    px(c, '#23262f', -15, 9, 2, 20);
    px(c, '#23262f', 8, 9, 2, 20);
    px(c, '#23262f', -24, 21, 48, 2);
    px(c, '#23262f', 10, 16, 14, 2);
    px(c, '#23262f', 0, 23, 2, 6);
    px(c, '#23262f', 16, 23, 2, 6);
  },
  tv: (c) => {
    box(c, -27, 8, 54, 18, '#3a4152', '#1b1f29');
    px(c, '#2a303d', -25, 10, 50, 13);
    px(c, '#4a5266', -23, 11, 16, 2);
    px(c, '#3a4152', -23, 14, 8, 1);
    px(c, '#8d94a3', -3, 24, 6, 1);
  },
  squiggle: (c) => {
    box(c, -27, 8, 54, 22, '#eef3fb', '#b4c0d4');
    for (const [col, list] of [
      ['#4a8fe0', [[-24, 11, 9, 2], [-24, 11, 2, 7], [-24, 16, 7, 2], [-19, 16, 2, 6], [-12, 11, 2, 9], [-12, 18, 9, 2], [-5, 12, 2, 8], [3, 10, 10, 2], [11, 10, 2, 6], [5, 14, 8, 2], [5, 14, 2, 7], [5, 19, 9, 2], [18, 12, 6, 2], [22, 12, 2, 10], [16, 20, 8, 2]]],
      ['#9aa7bb', [[-24, 24, 11, 2], [-15, 22, 2, 4], [-9, 23, 9, 2], [-2, 23, 2, 5], [17, 24, 8, 2], [15, 15, 2, 4], [-17, 11, 3, 2], [-8, 13, 2, 3]]],
    ]) for (const [ax, ay, aw, ah] of list) px(c, col, ax, ay, aw, ah);
  },
};

const OBJECTS = {
  v: (c, x, y) => chair(c, 0, x, y),
  '^': (c, x, y) => chair(c, 1, x, y),
  '<': (c, x, y) => chair(c, 2, x, y),
  '>': (c, x, y) => chair(c, 3, x, y),
  D: (c, x) => desk(c, false, x),
  M: (c, x) => desk(c, true, x),
  T: table,
  P: plant,
  Y: bush,
  W: pond,
  I: firePit,
  J: agentConsole,
  x: coasterGate,
  E: pine,
  O: (c) => roundTree(c, true),
  U: (c) => roundTree(c, false),
  K: counter,
  C: (c, x, y) => {
    counter(c, x, y);
    box(c, 11, -34, 10, 10, '#d9dfe9', '#9aa1b0');
    px(c, '#7a5238', 12, -29, 8, 4);
    box(c, 6, -25, 20, 31, '#2c303a', '#16181d');
    px(c, '#3c414d', 8, -23, 16, 5);
    px(c, '#ff5d5d', 9, -22, 2, 2);
    for (const bx of [13, 16, 19]) px(c, '#8d94a3', bx, -22, 2, 2);
    px(c, '#16181d', 10, -16, 12, 3);
    px(c, '#555b69', 14, -13, 4, 2);
    box(c, 13, -7, 6, 6, '#f4f6fa', '#b5bcc7');
    px(c, '#6b4a36', 14, -6, 4, 1);
    px(c, '#8d94a3', 8, 0, 16, 3);
    for (const gx of [10, 13, 16, 19]) px(c, '#555b69', gx, 0, 1, 3);
  },
  R: (c) => {
    px(c, '#00000018', 1, 29, 30, 3);
    box(c, 1, -38, 30, 68, '#30344a', '#1c1f2e');
    px(c, '#474c68', 2, -37, 28, 4);
    px(c, '#1c1f2e', 15, -32, 2, 61);
    px(c, '#3a3f58', 3, -31, 11, 2);
    px(c, '#3a3f58', 18, -31, 11, 2);
    for (const [hx, tx] of [[9, 11], [21, 19]]) {
      px(c, '#e9edf5', hx, -10, 2, 13);
      px(c, '#e9edf5', tx, -10, 2, 2);
      px(c, '#e9edf5', tx, 1, 2, 2);
    }
  },
  B: (c, x) => {
    box(c, 1, -12, 30, 42, '#8a6240', '#5a3e28');
    const cols = ['#e56b6b', '#5b9bdf', '#f2c14e', '#62bf84', '#a07ad9', '#ececec'];
    for (let r = 0; r < 3; r++) {
      px(c, '#5a3e28', 2, 1 + r * 13 - 1, 28, 2);
      for (let i = 0; i < 6; i++) px(c, cols[(i + r + x) % 6], 4 + i * 4, -9 + r * 13, 3, 9 - ((i + r) % 3));
    }
  },
  S: (c, x, y) => {
    if (cafe(x, y)) return couch(c, x, y);
    box(c, 0, 6, T, 24, '#d98f65', '#a8603c');
    px(c, '#e8a57c', 1, 13, 30, 11);
    px(c, '#c47b52', 1, 7, 30, 5);
  },
  s: (c) => {
    box(c, 0, 2, T, 26, '#d98f65', '#a8603c');
    px(c, '#e8a57c', 1, 3, 30, 12);
    px(c, '#c47b52', 1, 20, 30, 7);
  },
  j: (c, x, y) => {
    const [m, d] = [['#5b8def', '#3f6fd0'], ['#a07ad9', '#7f5bbd'], ['#ec6fa6', '#cf4f88']][(x + y) % 3];
    disc(c, 16, 27, 12, '#00000018', 0.3);
    disc(c, 16, 18, 13, d, 0.85);
    disc(c, 15, 16, 11, m, 0.8);
    disc(c, 11, 11, 4, '#ffffff33', 0.8);
  },
  t: (c, x, y) => {
    const n = (dx) => obj[y]?.[x + dx] === 't';
    const l = n(-1) ? 0 : 3, r = n(1) ? 0 : 3;
    px(c, '#00000014', l, 26, T - l - r, 4);
    px(c, '#b98a5a', l, 8, T - l - r, 18);
    px(c, '#e6c79a', l + (l ? 1 : 0), 9, T - l - r - (l ? 1 : 0) - (r ? 1 : 0), 13);
    if (!n(-1)) {
      box(c, 10, 11, 7, 7, '#ffffff', '#b5bcc7');
      px(c, '#7a4a2a', 12, 13, 3, 3);
    }
  },
  G: (c, x, y) => {
    const e = (dx, dy) => obj[y + dy]?.[x + dx] !== 'G';
    px(c, '#3aa36b', 0, 0, T, T);
    if (e(-1, 0)) px(c, '#ffffff', 0, 0, 2, T);
    if (e(0, -1)) px(c, '#ffffff', 0, 0, T, 2);
    if (e(1, 0)) px(c, '#ffffff', T - 2, 0, 2, T);
    if (e(0, 1)) px(c, '#ffffff', 0, T - 2, T, 2), px(c, '#277a4e', 0, T, T, 4);
    if (!e(-1, 0) && e(-2, 0)) px(c, '#f3f3f3', T - 1, -4, 2, T + 4);
  },
  Q: (c) => {
    box(c, 8, 10, 16, 20, '#eef1f5', '#a7afbc');
    box(c, 9, -7, 14, 18, '#7cc2f7', '#4d90c8');
    px(c, '#b3dcfb', 11, -5, 4, 9);
  },
  H: (c) => {
    box(c, 2, -10, 28, 26, '#ffffff', '#8c93a3');
    px(c, '#5b9bdf', 6, -5, 12, 2);
    px(c, '#e56b6b', 6, 0, 17, 2);
    px(c, '#62bf84', 6, 5, 9, 2);
  },
  A: (c, x) => {
    box(c, 3, -10, 26, 40, '#343a4b', '#1c202b');
    px(c, x % 2 ? '#f2c14e' : '#e56b6b', 5, -8, 22, 5);
    px(c, '#11141b', 6, 0, 20, 12);
    px(c, '#62e59a', 9, 3, 6, 4);
    px(c, '#e56b6b', 8, 16, 4, 4);
    px(c, '#5b9bdf', 18, 16, 4, 4);
  },
  n: (c, x, y) => lounge(c, x, y, 0),
  u: (c, x, y) => lounge(c, x, y, 1),
  d: (c, x, y) => lounge(c, x, y, 2),
  c: (c, x, y) => lounge(c, x, y, 3),
  o: (c) => {
    px(c, '#00000014', 4, 28, 22, 3);
    px(c, '#8a5a36', 6, 24, 3, 6);
    px(c, '#8a5a36', 20, 24, 3, 6);
    box(c, 3, 8, 23, 18, '#eccb97', '#b98d57');
    px(c, '#f6dfb6', 5, 10, 19, 3);
    px(c, '#d9b17a', 4, 21, 21, 4);
  },
  F: (c, x, y) => {
    if (obj[y][x - 1] === 'F') return;
    const cx = obj[y][x + 1] === 'F' ? T : T / 2, r = cx - 4;
    disc(c, cx, 23, r, '#00000018', 0.36);
    px(c, '#a8693f', cx - r + 9, 18, 3, 11);
    px(c, '#a8693f', cx + r - 12, 18, 3, 11);
    disc(c, cx, 15, r, '#b98d57', 0.4);
    disc(c, cx, 13, r, '#eccb97', 0.4);
    disc(c, cx - 2, 12, r - 6, '#f3d8a8', 0.36);
    px(c, '#5a6272', cx - 9, 9, 7, 3);
    px(c, '#5a6272', cx - 7, 4, 3, 6);
    disc(c, cx - 6, -2, 7, '#56657a');
    px(c, '#3c4a5c', cx - 12, -2, 13, 1);
    disc(c, cx - 8, -5, 3, '#8da0b8');
  },
  N: (c) => {
    px(c, '#00000014', 2, 29, 28, 3);
    box(c, 1, 2, 30, 27, '#e6e8f0', '#b3b9c7');
    px(c, '#ffffff', 2, 3, 28, 15);
    px(c, '#d5d9e3', 2, 18, 28, 1);
    box(c, 9, 6, 11, 8, '#f2c14e', '#b98a2a');
    px(c, '#fbe6a6', 11, 8, 7, 1);
  },
  L: (c) => {
    px(c, '#00000018', 2, 29, 28, 3);
    box(c, 2, 0, 28, 29, '#3d4460', '#23283b');
    px(c, '#4c5475', 3, 1, 26, 3);
    for (const dy of [7, 14, 21]) {
      px(c, '#23283b', 3, dy, 26, 1);
      px(c, '#aab2c5', 12, dy + 3, 8, 2);
    }
    box(c, 10, -9, 12, 10, '#e58555', '#a84f2b');
    px(c, '#f3a47a', 11, -8, 10, 1);
    disc(c, 16, -15, 7, '#3f9a54');
    disc(c, 10, -13, 4, '#5fb86a');
    disc(c, 22, -14, 4, '#5fb86a');
    disc(c, 16, -20, 4, '#5fb86a');
    disc(c, 14, -17, 2, '#8fd88c');
  },
  V: (c, x) => {
    px(c, '#00000018', 2, 29, 28, 3);
    px(c, '#2a2f3b', 4, 27, 4, 4);
    px(c, '#2a2f3b', 24, 27, 4, 4);
    if (x % 2) {
      box(c, 1, 6, 30, 22, '#4a5062', '#2a2f3b');
      px(c, '#2a2f3b', 2, 16, 28, 1);
      px(c, '#62e59a', 4, 10, 8, 3);
      px(c, '#f2c14e', 13, 10, 4, 3);
      px(c, '#e25652', 4, 20, 6, 3);
      px(c, '#8d94a3', 12, 20, 16, 2);
      box(c, 2, -6, 15, 13, '#2a2f3b', '#151821');
      px(c, '#e25652', 4, -4, 5, 2);
      px(c, '#4fae6a', 10, -4, 5, 2);
      px(c, '#f2c14e', 4, -1, 11, 4);
      box(c, 18, -3, 12, 11, '#5fa3ea', '#346fb5');
      px(c, '#d7ecfd', 20, -1, 8, 3);
      px(c, '#346fb5', 20, 4, 8, 1);
    } else {
      box(c, 1, 6, 30, 22, '#a678e0', '#6f48a8');
      px(c, '#b98cf0', 2, 7, 28, 3);
      for (const dy of [15, 21]) {
        px(c, '#8455c4', 2, dy, 28, 1);
        px(c, '#ece4fa', 10, dy + 2, 12, 2);
      }
      box(c, 4, -6, 16, 13, '#2a2f3b', '#151821');
      px(c, '#8d94a3', 6, -4, 8, 7);
      px(c, '#c5ccd9', 6, -4, 4, 3);
      px(c, '#c5ccd9', 10, -1, 4, 4);
      px(c, '#e25652', 16, -4, 2, 2);
      px(c, '#4fae6a', 16, -1, 2, 2);
      box(c, 6, 8, 14, 5, '#d9dfe9', '#9aa1b0');
      px(c, '#2a2f3b', 24, 1, 3, 9);
    }
  },
  X: (c) => {
    px(c, '#00000018', 2, 29, 28, 3);
    box(c, 2, -22, 28, 52, '#3b4152', '#1f232e');
    px(c, '#4a5166', 3, -21, 26, 4);
    px(c, '#1f232e', 15, -16, 2, 45);
    px(c, '#e9edf5', 11, 2, 2, 8);
    px(c, '#e9edf5', 19, 2, 2, 8);
  },
  Z: (c, x) => {
    px(c, '#00000018', 1, 29, 30, 3);
    box(c, 0, -2, T, 32, '#aab2c0', '#7b8494');
    px(c, '#c4cbd6', 1, -1, 30, 5);
    for (const dy of [6, 17]) {
      box(c, 3, dy, 26, 9, '#b7bfcb', '#8a93a3');
      px(c, '#6f7888', 12, dy + 3, 8, 2);
    }
    if (x % 2) {
      box(c, 5, -12, 20, 11, '#f1f3f7', '#a7afbc');
      px(c, '#cfd5de', 8, -9, 14, 2);
      px(c, '#3b4152', 8, -5, 14, 2);
    } else {
      box(c, 9, -10, 10, 9, '#f7f8fb', '#a7afbc');
      px(c, '#8fc3ee', 11, -8, 6, 4);
    }
  },
  r: (c, x, y) => {
    px(c, '#d7736f', 0, 0, T, T);
    px(c, '#e08a86', 2, 2, T - 4, T - 4);
    px(c, '#d7736f', 6, 6, T - 12, T - 12);
  },
};

function glassWall(c, r) {
  const x = (r.x0 + 1) * T, y = r.y0 * T, w = (r.x1 - r.x0 - 1) * T, m = 3, ph = 17;
  for (let i = 0; i < w; i += T) {
    px(c, '#4d5563', x + i, y, T, 8);
    px(c, '#5a6271', x + i + 1, y + 2, T - 2, 6);
    px(c, '#3d4450', x + i, y, T, 2);
  }
  px(c, '#747d8c', x, y + 8, w, 56);
  px(c, '#8a93a1', x, y + 8, w, 1);
  px(c, '#666f7e', x, y + 58, w, 6);
  px(c, '#0000000f', x, y + 64, w, 3);
  const cols = Math.max(1, Math.round((w - 92) / 80));
  const pw = Math.floor((w - 92 - (cols + 1) * m) / cols);
  const ww = cols * pw + (cols + 1) * m, wh = ph * 2 + m * 3, wx = x + 46, wy = y + 13;
  px(c, '#2b2f4a', wx, wy, ww, wh);
  for (let i = 0; i < cols; i++)
    for (let j = 0; j < 2; j++) {
      const gx = wx + m + i * (pw + m), gy = wy + m + j * (ph + m);
      px(c, '#a9dccd', gx, gy, pw, ph);
      px(c, '#c5ebdf', gx, gy, pw, 3);
      px(c, '#def5ee', gx + 6, gy + 5, 12, 2);
      px(c, '#def5ee', gx + 10, gy + 9, 7, 2);
    }
  px(c, '#e7ebf0', wx - 2, wy + wh, ww + 4, 2);
  px(c, '#59616f', wx - 2, wy + wh + 2, ww + 4, 1);
}

function sideDoor(c, r) {
  const x = r.x1;
  let a = -1, b = -1;
  for (let y = r.y0; y <= r.y1; y++) if (!isWall(x, y)) { if (a < 0) a = y; b = y; }
  if (a < 0) return;
  const [l, m, d] = WOOD;
  const X = x * T, Y0 = a * T, len = (b - a + 1) * T;
  box(c, X - 1, Y0 - 5, T + 2, 5, m, d);
  box(c, X - 1, Y0 + len, T + 2, 5, m, d);
  px(c, m, X, Y0, 2, len);
  px(c, m, X + T - 2, Y0, 2, len);
  box(c, X - 7, Y0 - len, 7, len - 3, l, d);
  px(c, m, X - 5, Y0 - len + 2, 2, len - 7);
  px(c, '#9aa1b0', X - 1, Y0 - 14, 3, 5);
  px(c, '#9aa1b0', X - 1, Y0 - len + 8, 3, 5);
  px(c, '#e9edf5', X - 9, Y0 - len + 6, 2, 4);
}

export function buildMapCanvas() {
  const cv = document.createElement('canvas');
  cv.width = W * T;
  cv.height = H * T;
  const c = cv.getContext('2d');
  const at = (x, y, fn) => {
    c.save();
    c.translate(x * T, y * T);
    fn();
    c.restore();
  };
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
      const f = floor[y][x];
      at(x, y, () => (f === '#' ? drawWall(c, x, y) : FLOORS[f](c, x, y)));
      if (obj[y][x] === 'r') at(x, y, () => OBJECTS.r(c, x, y));
    }
  glassWall(c, ROOMS.c);
  sideDoor(c, ROOMS.c);
  sideDoor(c, ROOMS.l);
  for (const d of DECOR) at(d.x, d.y, () => ART[d.art](c));
  drawRoad(c, ROAD);
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
      const o = obj[y][x];
      if (o && o !== 'r' && OBJECTS[o]) at(x, y, () => OBJECTS[o](c, x, y));
      else if (!o && x >= OFFICE_W && x < PARK_X0 && floor[y][x] === 'g' && (y < 14 || y > 23) && wantsTree(x, y)) at(x, y, () => OBJECTS[treeKind(x, y)](c, x, y));
    }
  drawParkArt(c);
  return cv;
}

export const OUTSKIRTS = 4;

const TREE_KINDS = ['E', 'O', 'U', 'E'];
const wantsTree = (x, y) => (x + y) % 2 === 0 && hash(x, y, 11) >= 0.35;
const treeKind = (x, y) => TREE_KINDS[(hash(y, x, 12) * TREE_KINDS.length) | 0];

function outskirtStrip(x0, y0, x1, y1, pad) {
  const cv = document.createElement('canvas');
  cv.width = (x1 - x0 + 1) * T;
  cv.height = (y1 - y0 + 1 + pad) * T;
  const c = cv.getContext('2d');
  const at = (x, y, fn) => {
    c.save();
    c.translate((x - x0) * T, (y - y0 + pad) * T);
    fn();
    c.restore();
  };
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) at(x, y, () => FLOORS.g(c, x, y));
  for (let y = y0; y <= y1; y++)
    for (let x = x0; x <= x1; x++) {
      if (y === H || !wantsTree(x, y)) continue;
      at(x, y, () => OBJECTS[treeKind(x, y)](c, x, y));
    }
  return { cv, x: x0 * T, y: (y0 - pad) * T };
}

export function buildOutskirts(m = OUTSKIRTS) {
  return [
    outskirtStrip(-m, -m, W + m - 1, -1, 0),
    outskirtStrip(-m, H, W + m - 1, H + m - 1, 2),
    outskirtStrip(-m, 0, -1, H - 1, 2),
    outskirtStrip(W, 0, W + m - 1, H - 1, 2),
  ];
}

Object.assign(FLOORS, PARK_FLOORS);
Object.assign(OBJECTS, PARK_OBJECTS);

const roomTags = new Map();
export function roomTag(text, dpr) {
  const key = `${text}|${dpr}`;
  let cv = roomTags.get(key);
  if (cv) return cv;
  cv = document.createElement('canvas');
  const c = cv.getContext('2d');
  const font = '600 12px system-ui, -apple-system, sans-serif';
  c.font = font;
  const w = Math.ceil(c.measureText(text).width) + 22, h = 24;
  cv.width = w * dpr;
  cv.height = h * dpr;
  c.scale(dpr, dpr);
  c.fillStyle = 'rgba(255,255,255,.96)';
  c.strokeStyle = 'rgba(30,34,45,.16)';
  c.beginPath();
  c.roundRect(0.5, 0.5, w - 1, h - 1, 11.5);
  c.fill();
  c.stroke();
  c.font = font;
  c.fillStyle = '#343a4a';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText(text, w / 2, h / 2 + 0.5);
  cv.cssW = w;
  cv.cssH = h;
  roomTags.set(key, cv);
  return cv;
}

export function drawLabels(c) {
  for (const l of LABELS) {
    const tag = roomTag(l.text, 1);
    c.drawImage(tag, Math.round(l.x * T - tag.cssW / 2), Math.round(l.y * T - tag.cssH / 2));
  }
}

const tagCache = new Map();
export function nameTag(text, dot, dpr) {
  const key = `${text}|${dot}|${dpr}`;
  let cv = tagCache.get(key);
  if (cv) return cv;
  cv = document.createElement('canvas');
  const c = cv.getContext('2d');
  const font = '600 11px system-ui, -apple-system, sans-serif';
  c.font = font;
  const w = Math.ceil(c.measureText(text).width) + (dot ? 22 : 14), h = 19;
  cv.width = w * dpr;
  cv.height = h * dpr;
  c.scale(dpr, dpr);
  c.fillStyle = 'rgba(24,26,34,.86)';
  c.beginPath();
  c.roundRect(0, 0, w, h, 6);
  c.fill();
  if (dot) {
    c.fillStyle = dot;
    c.beginPath();
    c.arc(9, h / 2, 3, 0, Math.PI * 2);
    c.fill();
  }
  c.font = font;
  c.fillStyle = '#fff';
  c.textBaseline = 'middle';
  c.fillText(text, dot ? 16 : 7, h / 2 + 0.5);
  cv.cssW = w;
  cv.cssH = h;
  if (tagCache.size > 500) tagCache.clear();
  tagCache.set(key, cv);
  return cv;
}
