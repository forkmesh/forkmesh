import { TILE as T, PARK_X0 as K, obj } from '../map.js';
import { hash, px, box, disc, tri } from '../draw.js';

export const ROAD = [[61.5, 16.5], [64.5, 16.5], [68.5, 16.5], [69.5, 21.5], [K + 1.5, 21.5], [K + 3.5, 20.5]];

export const ANCHORS = {
  crater: { x: K + 51.5, y: 11.8 },
  volcanoBase: 26,
  funnel: { x: K + 80.5, y: 14.5, r: 3.2 },
  flush: { x: K + 5, y: 33, r: 2.8 },
  flushDeck: { x: K + 8.7, y: 29.9, x0: K + 7.6, x1: K + 9.8 },
  racerPads: 4.5,
  skullPeak: { x: K + 69.5, y: 11.2 },
};

const isObj = (x, y, c) => obj[y]?.[x] === c;

function paver(c, x, y, a, b, line) {
  px(c, a, 0, 0, T, T);
  if ((x + y) % 2) px(c, b, 1, 1, T - 2, T - 2);
  px(c, line, 0, T - 1, T, 1);
  px(c, line, T - 1, 0, 1, T);
}

export const PARK_FLOORS = {
  D: (c, x, y) => {
    px(c, '#5d6370', 0, 0, T, T);
    if (hash(x, y, 2) > 0.6) px(c, '#666c79', (hash(x, y, 3) * 24) | 0, (hash(y, x, 3) * 24) | 0, 5, 3);
    if (x % 2 === 0) px(c, '#e8e8e8', 0, 4, 2, 22);
  },
  Z: (c, x, y) => paver(c, x, y, '#efe3cc', '#f4ead6', '#e0d0b2'),
  V: (c, x, y) => paver(c, x, y, '#d9c3ad', '#e2cfbb', '#c4a88f'),
  B: (c, x, y) => {
    px(c, '#f3dfac', 0, 0, T, T);
    for (let i = 0; i < 5; i++) px(c, i % 2 ? '#e8cf92' : '#f9ebc4', (hash(x, y, i) * 30) | 0, (hash(y, x, i) * 30) | 0, 2, 1);
  },
  K: (c, x, y) => {
    px(c, '#9fd8e0', 0, 0, T, T);
    px(c, '#b6e4ea', 2, 2, T - 4, T - 4);
    if ((x + y) % 3 === 0) disc(c, 16, 16, 3, '#f7a8c4');
    if ((x * 3 + y) % 5 === 0) disc(c, 8, 24, 2, '#ffe27a');
  },
  A: (c, x, y) => {
    px(c, '#1f4f7a', 0, 0, T, T);
    px(c, '#2a6496', 0, 0, T, 2);
    if (hash(x, y, 5) > 0.5) px(c, '#3d7fb5', (hash(x, y, 6) * 26) | 0, 10, 6, 1);
  },
};

function water(c, x, y, a = '#4fb0d6', b = '#6cc4e4') {
  px(c, a, 0, 0, T, T);
  px(c, b, 4 + ((x * 7) % 12), 8 + ((y * 5) % 10), 9, 1);
  px(c, b, 14 - ((x * 3) % 9), 22 - ((y * 7) % 8), 7, 1);
}

function river(c, x, y) {
  water(c, x, y, '#3fa3c9', '#7fd0ea');
  const edge = (dx, dy) => !isObj(x + dx, y + dy, '~');
  const bank = '#7c9a5c';
  if (edge(0, -1) && !isObj(x, y - 1, '')) px(c, bank, 0, 0, T, 3);
  if (edge(0, -1)) px(c, '#a7b98a', 0, 0, T, 2);
  if (edge(0, 1)) px(c, '#a7b98a', 0, T - 2, T, 2);
  if (edge(-1, 0)) px(c, '#a7b98a', 0, 0, 2, T);
  if (edge(1, 0)) px(c, '#a7b98a', T - 2, 0, 2, T);
}

function rock(c, x, y) {
  px(c, '#4f9f5c', 0, 0, T, T);
  disc(c, 8 + ((x * 5) % 10), 10, 9, '#3f8a55');
  disc(c, 22, 20 - ((y * 3) % 6), 9, '#62b56c');
  disc(c, 12, 24, 6, '#529f64');
  if (hash(x, y, 4) > 0.6) px(c, '#f6a5c0', 18, 8, 3, 3);
}

function tank(c, x, y) {
  px(c, '#1e6c9e', 0, 0, T, T);
  px(c, '#2f86bc', 0, 0, T, 4);
  if (!isObj(x, y + 1, 'k')) px(c, '#c9b48a', 0, T - 6, T, 6), disc(c, 8, T - 6, 4, '#e86a8f'), px(c, '#3fae6a', 22, T - 14, 2, 9);
  if (!isObj(x, y - 1, 'k')) px(c, '#bde6f7', 0, 0, T, 2);
  if (hash(x, y, 7) > 0.55) {
    const fx = (hash(x, y, 8) * 20) | 0, fy = 8 + ((hash(y, x, 8) * 12) | 0);
    px(c, ['#ffb347', '#ffe066', '#ff7aa2'][(x + y) % 3], fx, fy, 6, 3);
    px(c, '#ffffff', fx + 4, fy, 1, 1);
  }
}

function lane(c, x) {
  const colors = ['#e5484d', '#3b82f6', '#22c55e', '#f59e0b'];
  const col = colors[(x - (K + 92)) % 4] || '#3b82f6';
  px(c, '#e9eef5', 0, 0, T, T);
  px(c, col, 3, 0, T - 6, T);
  px(c, '#9fdcf2', 8, 0, T - 16, T);
  px(c, '#ffffff99', 12, (x * 7) % T, 4, 6);
}

function lounger(c) {
  px(c, '#00000014', 6, 26, 22, 4);
  box(c, 7, 8, 18, 18, '#ffffff', '#9fb4c7');
  px(c, '#3b9ad9', 9, 10, 14, 3);
  px(c, '#3b9ad9', 9, 16, 14, 3);
  box(c, 7, 2, 18, 8, '#f4f7fa', '#9fb4c7');
}

function umbrella(c, x, y) {
  px(c, '#00000018', 4, 26, 24, 4);
  px(c, '#8a6240', 15, -6, 2, 32);
  const col = ['#e5484d', '#f59e0b', '#3b82f6', '#22c55e'][(x + y) % 4];
  tri(c, 16, -18, 12, 16, col);
  for (let i = -14; i <= 14; i += 7) px(c, '#ffffff', 16 + i, -7, 3, 2);
}

function palm(c, x, y) {
  disc(c, 16, 29, 12, '#c3dfbf', 0.3);
  const lean = hash(x, y, 9) > 0.5 ? 1 : -1;
  for (let i = 0; i < 40; i += 4) box(c, 13 + Math.round((i / 40) * lean * 6), 28 - i, 6, 5, '#b08155', '#7f5a37');
  const tx = 16 + lean * 6, ty = -14;
  for (const [dx, dy, w] of [[-18, 4, 18], [4, 4, 18], [-14, -6, 14], [2, -6, 14], [-6, -10, 12]]) {
    px(c, '#2f8a4e', tx + dx, ty + dy, w, 4);
    px(c, '#46a865', tx + dx + 2, ty + dy, w - 4, 2);
  }
  disc(c, tx, ty + 4, 3, '#7a4a2a');
}

function parkedCar(c) {
  px(c, 'rgba(0,0,0,.2)', -4, 24, 40, 5);
  box(c, -3, 10, 38, 14, '#e5484d', '#9b2c2f');
  box(c, 6, 2, 20, 10, '#bfe6f5', '#5d7f99');
  px(c, '#1f2333', 1, 22, 8, 6);
  px(c, '#1f2333', 23, 22, 8, 6);
  px(c, '#ffd84d', 31, 13, 3, 3);
}

function rideGate(c) {
  px(c, '#00000022', 4, 27, 24, 4);
  box(c, 14, 0, 4, 28, '#8a6240', '#5e3f26');
  box(c, 2, -14, 28, 16, '#2b8fd6', '#1c5f91');
  px(c, '#ffffff', 6, -9, 20, 2);
  px(c, '#ffffff', 8, -5, 16, 2);
  px(c, '#ffe066', 10, -1, 12, 2);
}

function plain(color, edge) {
  return (c) => box(c, 0, 0, T, T, color, edge);
}

export const PARK_OBJECTS = {
  '~': river,
  q: rock,
  k: tank,
  '=': (c, x) => lane(c, x),
  z: plain('#b98a5a', '#80593a'),
  b: plain('#dff3fb', '#9cc9dd'),
  l: (c, x, y) => PARK_FLOORS.Z(c, x, y),
  f: (c, x, y) => water(c, x, y, '#2e9fd8', '#ffffff'),
  i: plain('#9fd8e0', '#6fb2bd'),
  w: plain('#f3dfac', '#d6bd84'),
  a: plain('#f3dfac', '#d6bd84'),
  h: lounger,
  m: umbrella,
  p: palm,
  e: parkedCar,
  y: rideGate,
};

function volcano(c) {
  const cx = (K + 51.5) * T, base = 26 * T, top = 11.5 * T;
  c.fillStyle = 'rgba(0,0,0,.12)';
  c.beginPath(); c.ellipse(cx + 10, base - 6, 7.8 * T, 1.6 * T, 0, 0, Math.PI * 2); c.fill();
  const layers = [['#6b5548', 7.6, 1.6], ['#7d6556', 6.4, 1.45], ['#8e7462', 5.2, 1.3]];
  for (const [col, w, k] of layers) {
    c.fillStyle = col;
    c.beginPath();
    c.moveTo(cx - w * T, base);
    c.quadraticCurveTo(cx - w * T * 0.35, top + (base - top) * 0.25 * k, cx - 1.6 * T, top);
    c.lineTo(cx + 1.6 * T, top);
    c.quadraticCurveTo(cx + w * T * 0.35, top + (base - top) * 0.25 * k, cx + w * T, base);
    c.closePath();
    c.fill();
  }
  c.fillStyle = '#4a3a31';
  c.beginPath(); c.ellipse(cx, top, 1.7 * T, 0.45 * T, 0, 0, Math.PI * 2); c.fill();
  c.fillStyle = '#ff7a1a';
  c.beginPath(); c.ellipse(cx, top + 2, 1.2 * T, 0.28 * T, 0, 0, Math.PI * 2); c.fill();
  c.fillStyle = '#ffcf4a';
  c.beginPath(); c.ellipse(cx - 6, top + 1, 0.6 * T, 0.12 * T, 0, 0, Math.PI * 2); c.fill();
  c.strokeStyle = '#ff7a1a';
  c.lineWidth = 5;
  for (const [dx, len] of [[-30, 2.6], [22, 3.4], [-4, 1.8]]) {
    c.beginPath();
    c.moveTo(cx + dx * 0.4, top + 8);
    c.quadraticCurveTo(cx + dx, top + len * T * 0.5, cx + dx * 1.3, top + len * T);
    c.stroke();
  }
  const stairTop = ANCHORS.crater.y * T + 10;
  const steps = Math.floor((base - 6 - stairTop) / 12);
  for (let i = 0; i <= steps; i++) {
    const y = base - 6 - i * 12, w = 20 - (i / steps) * 10;
    px(c, '#c9b7a3', Math.round(cx - w / 2), Math.round(y), Math.round(w), 6);
    px(c, '#a8927c', Math.round(cx - w / 2), Math.round(y + 5), Math.round(w), 2);
  }
  for (let i = 0; i < 8; i++) {
    const a = (i / 8) * Math.PI * 2;
    disc(c, cx + Math.cos(a) * 6.6 * T, base - 18 + Math.sin(a) * 1.1 * T, 9, '#4f9f5c');
    disc(c, cx + Math.cos(a) * 6.6 * T - 3, base - 22 + Math.sin(a) * 1.1 * T, 5, '#6fbf72');
  }
}

function skullRock(c) {
  const x = (K + 66) * T, y = 12 * T, w = 7 * T, h = 6 * T;
  c.fillStyle = '#8a8e96';
  c.beginPath();
  c.moveTo(x, y + h);
  c.quadraticCurveTo(x - 10, y + h * 0.2, x + w * 0.35, y - 26);
  c.quadraticCurveTo(x + w * 0.75, y - 40, x + w + 6, y + h * 0.4);
  c.lineTo(x + w, y + h);
  c.closePath();
  c.fill();
  c.fillStyle = '#a3a7ae';
  c.beginPath(); c.ellipse(x + w * 0.5, y + 30, 2.2 * T, 1.7 * T, 0, 0, Math.PI * 2); c.fill();
  c.fillStyle = '#2b2d33';
  c.beginPath(); c.ellipse(x + w * 0.38, y + 22, 18, 14, 0, 0, Math.PI * 2); c.fill();
  c.beginPath(); c.ellipse(x + w * 0.62, y + 22, 18, 14, 0, 0, Math.PI * 2); c.fill();
  tri(c, Math.round(x + w * 0.5), Math.round(y + 40), 16, 8, '#2b2d33');
  for (let i = 0; i < 6; i++) px(c, '#f2efe6', Math.round(x + w * 0.5 - 30 + i * 10), Math.round(y + 64), 7, 10);
  c.fillStyle = 'rgba(160,220,245,.85)';
  c.fillRect(x + w * 0.5 - 6, y + 76, 12, h - 76);
}

function racerTower(c) {
  const x = (K + 92) * T, y = 2 * T;
  box(c, x - 4, y - 30, 5 * T + 8, 3 * T + 30, '#c8955f', '#7a5534');
  box(c, x - 12, y - 52, 5 * T + 24, 26, '#2b8fd6', '#1c5f91');
  c.fillStyle = '#ffffff';
  c.font = '700 13px system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText('RIPTIDE RACERS', x + 2.5 * T, y - 39);
  const colors = ['#e5484d', '#3b82f6', '#22c55e', '#f59e0b'];
  for (let i = 0; i < 4; i++) box(c, x + i * T + 4, y + 2 * T + 6, T - 8, T - 8, colors[i], '#00000055');
}

function bucketTower(c) {
  const x = (K + 20) * T, y = 30 * T;
  for (const dx of [0, 4 * T - 8]) box(c, x + dx, y - 40, 8, 4 * T + 40, '#f59e0b', '#b06f06');
  box(c, x - 6, y + 40, 4 * T + 12, 10, '#22c55e', '#15803d');
  box(c, x - 6, y - 10, 4 * T + 12, 10, '#22c55e', '#15803d');
  c.strokeStyle = '#3b82f6';
  c.lineWidth = 10;
  c.beginPath();
  c.moveTo(x + 4 * T, y + 40);
  c.quadraticCurveTo(x + 5.5 * T, y + 3 * T, x + 5 * T, y + 4.6 * T);
  c.stroke();
  c.strokeStyle = '#e5484d';
  c.beginPath();
  c.moveTo(x, y - 6);
  c.quadraticCurveTo(x - 1.6 * T, y + T, x - T, y + 4.4 * T);
  c.stroke();
}

function funnelBowl(c) {
  const cx = ANCHORS.funnel.x * T, cy = ANCHORS.funnel.y * T;
  c.fillStyle = 'rgba(0,0,0,.12)';
  c.beginPath(); c.ellipse(cx + 8, cy + 10, 3.7 * T, 3.4 * T, 0, 0, Math.PI * 2); c.fill();
  const rings = [['#f97316', 3.6], ['#fdba74', 3.2], ['#fde7c8', 2.6], ['#bfe6f5', 1.9], ['#6cc4e4', 1.1], ['#2f86bc', 0.45]];
  for (const [col, r] of rings) { c.fillStyle = col; c.beginPath(); c.ellipse(cx, cy, r * T, r * T * 0.92, 0, 0, Math.PI * 2); c.fill(); }
}

function flowRider(c) {
  const x = (K + 66) * T, y = 35 * T, w = 9 * T, h = 5 * T;
  const g = c.createLinearGradient(x, y, x, y + h);
  g.addColorStop(0, '#bfe9fb');
  g.addColorStop(0.5, '#3aa7dc');
  g.addColorStop(1, '#1f6fa6');
  c.fillStyle = g;
  c.fillRect(x, y, w, h);
  box(c, x - 6, y - 6, w + 12, 6, '#d7dde6', '#8f99a8');
  box(c, x - 6, y + h, w + 12, 6, '#d7dde6', '#8f99a8');
  c.fillStyle = '#1c5f91';
  c.font = '700 12px system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'bottom';
  c.fillText('ENDLESS WAVE', x + w / 2, y - 8);
}

function shack(c) {
  const x = (K + 46) * T, y = 34 * T;
  box(c, x + 4, y + 18, 5 * T - 8, 3 * T - 18, '#c8955f', '#7a5534');
  box(c, x + 10, y + 34, 5 * T - 20, 14, '#f4ead6', '#c4a88f');
  c.fillStyle = '#d9b26a';
  c.beginPath(); c.moveTo(x - 10, y + 24); c.lineTo(x + 2.5 * T, y - 20); c.lineTo(x + 5 * T + 10, y + 24); c.closePath(); c.fill();
  for (let i = 0; i < 12; i++) px(c, '#b8904a', x - 6 + i * 15, y + 18, 2, 8);
  c.fillStyle = '#7a3424';
  c.font = '700 12px system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText('COCONUT SHACK', x + 2.5 * T, y + 6);
}

function cabana(c, x0) {
  const x = (K + x0) * T, y = 35 * T;
  for (let i = 0; i < 6; i++) px(c, i % 2 ? '#ffffff' : '#3b82f6', x + i * 16, y - 10, 16, 26);
  tri(c, x + 48, y - 26, 18, 52, '#1d4ed8');
  px(c, '#00000022', x, y + 16, 96, 6);
  box(c, x + 30, y + 22, 36, 18, '#ffffff', '#9fb4c7');
}

function royalFlush(c) {
  const cx = ANCHORS.flush.x * T, cy = ANCHORS.flush.y * T;
  c.fillStyle = 'rgba(0,0,0,.14)';
  c.beginPath(); c.ellipse(cx + 8, cy + 12, 3.2 * T, 2.9 * T, 0, 0, Math.PI * 2); c.fill();
  c.fillStyle = 'rgba(190,230,255,.55)';
  c.fillRect(cx - 12, cy, 24, 6.5 * T);
  c.strokeStyle = 'rgba(120,170,210,.9)';
  c.lineWidth = 2;
  c.strokeRect(cx - 12, cy, 24, 6.5 * T);
  const rings = [['#d4a017', 3.1], ['#ffffff', 2.85], ['#e9eef5', 2.5], ['#bfe6f5', 2.1], ['#6cc4e4', 1.4], ['#2f86bc', 0.6], ['#1b4f75', 0.3]];
  for (const [col, r] of rings) { c.fillStyle = col; c.beginPath(); c.ellipse(cx, cy, r * T, r * T * 0.9, 0, 0, Math.PI * 2); c.fill(); }
  const d = ANCHORS.flushDeck;
  box(c, d.x0 * T, (d.y - 0.5) * T, (d.x1 - d.x0) * T, 1.6 * T, '#c8955f', '#7a5534');
  for (let i = 0; i < 5; i++) box(c, cx + 3.2 * T + 2, cy - 2 * T + i * 10, 26, 8, '#d9b26a', '#9a7432');
  box(c, cx - 2.6 * T, cy - 4.4 * T, 5.2 * T, 26, '#7c3aed', '#4c1d95');
  c.fillStyle = '#ffd84d';
  c.font = '800 14px system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText('👑 ROYAL FLUSH', cx, cy - 4.4 * T + 13);
}

function arch(c) {
  const x = (K + 9) * T, y = 17 * T;
  box(c, x, y, 14, 8 * T, '#2b8fd6', '#1c5f91');
  box(c, x + 18, y - 34, 14, 34, '#2b8fd6', '#1c5f91');
  box(c, x - 10, y - 40, 34, 30, '#f59e0b', '#b06f06');
  box(c, x, y + 8 * T - 4, 14, 14, '#2b8fd6', '#1c5f91');
  c.save();
  c.translate(x + 7, y + 4 * T);
  c.rotate(-Math.PI / 2);
  c.fillStyle = '#ffffff';
  c.font = '800 15px system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'middle';
  c.fillText('FORKMESH WATER PARK', 0, 0);
  c.restore();
}

function bridges(c) {
  for (const [x0, y0, vertical] of [[50, 9, false], [50, 28, false], [40, 18, true], [61, 18, true]]) {
    const x = (K + x0) * T, y = y0 * T;
    px(c, '#3fa3c9', x, y, 2 * T, 2 * T);
    for (let i = 0; i < 8; i++) {
      if (vertical) box(c, x + i * 8, y + 2, 7, 2 * T - 4, '#b98a5a', '#80593a');
      else box(c, x + 2, y + i * 8, 2 * T - 4, 7, '#b98a5a', '#80593a');
    }
    if (vertical) { px(c, '#7a5534', x, y, 2 * T, 3); px(c, '#7a5534', x, y + 2 * T - 3, 2 * T, 3); }
    else { px(c, '#7a5534', x, y, 3, 2 * T); px(c, '#7a5534', x + 2 * T - 3, y, 3, 2 * T); }
  }
  const wx = (K + 50) * T, wy = 9 * T;
  for (let i = 0; i < 4; i++) px(c, 'rgba(220,245,255,.8)', wx + 6 + i * 14, wy - 30, 6, 30);
}

export function drawRoad(c, road) {
  c.lineCap = 'round';
  c.lineJoin = 'round';
  c.strokeStyle = '#5d6370';
  c.lineWidth = 2 * T - 6;
  c.beginPath();
  road.forEach(([x, y], i) => (i ? c.lineTo(x * T, y * T) : c.moveTo(x * T, y * T)));
  c.stroke();
  c.strokeStyle = '#f5d04c';
  c.lineWidth = 2;
  c.setLineDash([10, 10]);
  c.stroke();
  c.setLineDash([]);
}

export function drawParkArt(c) {
  bridges(c);
  volcano(c);
  skullRock(c);
  funnelBowl(c);
  racerTower(c);
  bucketTower(c);
  flowRider(c);
  shack(c);
  for (const x0 of [39, 53, 58]) cabana(c, x0);
  royalFlush(c);
  arch(c);
}
