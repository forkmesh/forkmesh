import { TILE as T, OFFICE_W as W, H } from '../../map.js';
import { loopCenters, STEP } from './track.js';

export function drawTrack(track) {
  const cv = document.createElement('canvas');
  cv.width = W * T;
  cv.height = H * T;
  const c = cv.getContext('2d');
  const pts = track.pts.filter((p) => !p.loop);
  c.lineCap = 'round';
  c.strokeStyle = 'rgba(0,0,0,.12)';
  c.lineWidth = 9;
  c.beginPath();
  pts.forEach((p, i) => (i ? c.lineTo(p.x * T, p.y * T + 3) : c.moveTo(p.x * T, p.y * T + 3)));
  c.stroke();
  for (let i = 0; i < pts.length; i += Math.round(1.2 / STEP)) {
    c.fillStyle = '#8a6240';
    c.fillRect(Math.round(pts[i].x * T) - 5, Math.round(pts[i].y * T) - 1, 10, 3);
  }
  for (const off of [-3, 3]) {
    c.strokeStyle = '#d64545';
    c.lineWidth = 2;
    c.beginPath();
    pts.forEach((p, i) => {
      const q = pts[Math.min(pts.length - 1, i + 1)], r = pts[Math.max(0, i - 1)];
      const dx = q.x - r.x, dy = q.y - r.y, n = Math.hypot(dx, dy) || 1;
      const x = p.x * T - (dy / n) * off, y = p.y * T + (dx / n) * off;
      i ? c.lineTo(x, y) : c.moveTo(x, y);
    });
    c.stroke();
  }
  for (const l of loopCenters(track)) {
    const cx = l.x * T, cy = l.y * T, r = l.r * T;
    c.strokeStyle = 'rgba(0,0,0,.14)';
    c.lineWidth = 9;
    c.beginPath();
    c.arc(cx + 2, cy + 3, r, 0, Math.PI * 2);
    c.stroke();
    c.fillStyle = '#7a7f8c';
    c.fillRect(cx - 2, cy, 4, r + 4);
    for (const off of [-3, 3]) {
      c.strokeStyle = '#e5484d';
      c.lineWidth = 2;
      c.beginPath();
      c.arc(cx, cy, r + off, 0, Math.PI * 2);
      c.stroke();
    }
    c.strokeStyle = '#8a6240';
    c.lineWidth = 1;
    for (let a = 0; a < Math.PI * 2; a += Math.PI / 12) {
      c.beginPath();
      c.moveTo(cx + Math.cos(a) * (r - 4), cy + Math.sin(a) * (r - 4));
      c.lineTo(cx + Math.cos(a) * (r + 4), cy + Math.sin(a) * (r + 4));
      c.stroke();
    }
  }
  return cv;
}

export function cart(ctx, x, y, color = '#e5484d') {
  ctx.fillStyle = 'rgba(0,0,0,.18)';
  ctx.fillRect(x - 11, y + 1, 22, 3);
  ctx.fillStyle = color;
  ctx.fillRect(x - 11, y - 9, 22, 10);
  ctx.fillStyle = '#f5d04c';
  ctx.fillRect(x - 11, y - 9, 22, 2);
  ctx.fillStyle = '#1f2333';
  ctx.fillRect(x - 8, y, 4, 3);
  ctx.fillRect(x + 4, y, 4, 3);
}
