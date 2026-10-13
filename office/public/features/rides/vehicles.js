import { cart } from '../coaster/art.js';

const rect = (c, color, x, y, w, h) => { c.fillStyle = color; c.fillRect(Math.round(x), Math.round(y), w, h); };

function ellipse(c, x, y, rx, ry, color) {
  c.fillStyle = color;
  c.beginPath();
  c.ellipse(x, y, rx, ry, 0, 0, Math.PI * 2);
  c.fill();
}

function car(c, x, y, dir, color = '#e5484d') {
  const f = dir < 0 ? -1 : 1;
  rect(c, 'rgba(0,0,0,.2)', x - 20, y + 4, 40, 4);
  rect(c, color, x - 19, y - 8, 38, 11);
  rect(c, '#ffffff55', x - 17, y - 7, 34, 2);
  rect(c, '#bfe6f5', x + f * 6 - 4, y - 16, 9, 8);
  rect(c, color, x - 6 - f * 4, y - 9, 12, 2);
  rect(c, '#1f2333', x - 14, y + 2, 7, 5);
  rect(c, '#1f2333', x + 7, y + 2, 7, 5);
  rect(c, '#9aa1b0', x - 12, y + 3, 3, 2);
  rect(c, '#9aa1b0', x + 9, y + 3, 3, 2);
  rect(c, '#ffd84d', x + f * 17 - 1, y - 6, 3, 3);
  rect(c, '#c2403c', x - f * 18 - 1, y - 6, 2, 3);
}

function tube(c, x, y, color) {
  ellipse(c, x, y + 1, 15, 6, 'rgba(0,0,0,.15)');
  ellipse(c, x, y - 2, 14, 6, color);
  ellipse(c, x, y - 3, 7, 2.5, '#2c6fa0');
  rect(c, '#ffffff88', x - 9, y - 6, 6, 2);
}

function raft(c, x, y) {
  ellipse(c, x, y, 19, 8, 'rgba(0,0,0,.15)');
  ellipse(c, x, y - 3, 18, 7, '#f5b041');
  ellipse(c, x, y - 4, 12, 4, '#2f8fbf');
  rect(c, '#e5484d', x - 18, y - 4, 4, 3);
  rect(c, '#e5484d', x + 14, y - 4, 4, 3);
}

function mat(c, x, y, color) {
  rect(c, 'rgba(0,0,0,.18)', x - 12, y + 1, 24, 3);
  rect(c, color, x - 12, y - 4, 24, 6);
  rect(c, '#ffffff66', x - 10, y - 3, 20, 1);
}

function board(c, x, y) {
  ellipse(c, x, y + 2, 17, 4, 'rgba(0,0,0,.2)');
  ellipse(c, x, y, 16, 4, '#ffe066');
  rect(c, '#e5484d', x - 14, y - 1, 28, 2);
}

function capsule(c, x, y, left) {
  c.fillStyle = 'rgba(190,230,255,.35)';
  c.fillRect(x - 13, y - 48, 26, 52);
  c.strokeStyle = 'rgba(80,140,190,.9)';
  c.lineWidth = 2;
  c.strokeRect(x - 13, y - 48, 26, 52);
  const n = Math.max(1, Math.ceil(left / 1000));
  c.fillStyle = '#e5484d';
  c.font = '700 14px system-ui, sans-serif';
  c.textAlign = 'center';
  c.textBaseline = 'bottom';
  c.fillText(String(n), x, y - 52);
}

const COLORS = ['#e5484d', '#3b82f6', '#22c55e', '#f59e0b'];

export function drawVehicle(c, kind, x, y, { dir = 1, id = '', phaseLeft = 0, phase = '' } = {}) {
  if (kind === 'cart') return cart(c, x, y);
  if (kind === 'car') return car(c, x, y, dir, id === 'drive-office' ? '#3b82f6' : '#e5484d');
  if (kind === 'tube') return tube(c, x, y, '#f97316');
  if (kind === 'raft') return raft(c, x, y);
  if (kind === 'mat') return mat(c, x, y, COLORS[(Number(id.slice(4)) - 1) % 4 || 0]);
  if (kind === 'surf') return board(c, x, y + 8);
  if (kind === 'body' && phase === 'capsule') return capsule(c, x, y, phaseLeft);
}
