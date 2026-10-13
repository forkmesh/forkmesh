import { BW, BH, STICKY_W, STICKY_H } from './model.js';

export const FONT = '-apple-system, BlinkMacSystemFont, Inter, "Segoe UI", system-ui, sans-serif';
const pw = (w, p) => w * (0.4 + p / 7.5);

function smoothPath(g, pts) {
  g.beginPath();
  g.moveTo(pts[0], pts[1]);
  const n = pts.length / 3;
  if (n === 2) {
    g.lineTo(pts[3], pts[4]);
    return;
  }
  for (let i = 1; i < n - 1; i++) {
    const x = pts[i * 3], y = pts[i * 3 + 1];
    g.quadraticCurveTo(x, y, (x + pts[i * 3 + 3]) / 2, (y + pts[i * 3 + 4]) / 2);
  }
  g.lineTo(pts[(n - 1) * 3], pts[(n - 1) * 3 + 1]);
}

export function drawStroke(g, it) {
  const pts = it.pts;
  if (!pts || pts.length < 3) return;
  g.save();
  g.lineCap = 'round';
  g.lineJoin = 'round';
  g.strokeStyle = it.c;
  g.fillStyle = it.c;
  if (it.hl) {
    g.globalAlpha = 0.35;
    g.lineWidth = it.w * 3;
    if (pts.length === 3) {
      g.beginPath();
      g.arc(pts[0], pts[1], it.w * 1.5, 0, Math.PI * 2);
      g.fill();
    } else {
      smoothPath(g, pts);
      g.stroke();
    }
    g.restore();
    return;
  }
  if (pts.length === 3) {
    g.beginPath();
    g.arc(pts[0], pts[1], pw(it.w, pts[2]) / 2, 0, Math.PI * 2);
    g.fill();
    g.restore();
    return;
  }
  let flat = true;
  for (let i = 5; i < pts.length; i += 3) if (pts[i] !== pts[2]) { flat = false; break; }
  if (flat) {
    g.lineWidth = pw(it.w, pts[2]);
    smoothPath(g, pts);
    g.stroke();
  } else {
    for (let i = 3; i < pts.length; i += 3) {
      g.lineWidth = pw(it.w, (pts[i - 1] + pts[i + 2]) / 2);
      g.beginPath();
      g.moveTo(pts[i - 3], pts[i - 2]);
      g.lineTo(pts[i], pts[i + 1]);
      g.stroke();
    }
  }
  g.restore();
}

function wrap(g, text, maxW) {
  const lines = [];
  for (const para of String(text).split('\n')) {
    let line = '';
    for (const word of para.split(/(\s+)/)) {
      const next = line + word;
      if (line && g.measureText(next).width > maxW) {
        lines.push(line.trimEnd());
        line = word.trimStart();
        while (g.measureText(line).width > maxW && line.length > 1) {
          let cut = line.length - 1;
          while (cut > 1 && g.measureText(line.slice(0, cut)).width > maxW) cut--;
          lines.push(line.slice(0, cut));
          line = line.slice(cut);
        }
      } else line = next;
    }
    lines.push(line);
  }
  return lines;
}

export function drawItem(g, it) {
  if (it.k === 's') return drawStroke(g, it);
  g.save();
  if (it.k === 't') {
    g.fillStyle = it.c;
    g.font = `500 ${it.sz}px ${FONT}`;
    g.textBaseline = 'top';
    g.fillText(it.text, it.x, it.y + it.sz * 0.1);
  } else if (it.k === 'n') {
    g.shadowColor = 'rgba(20,22,35,.16)';
    g.shadowBlur = 10;
    g.shadowOffsetY = 3;
    g.fillStyle = it.c;
    g.beginPath();
    g.roundRect(it.x, it.y, STICKY_W, STICKY_H, 6);
    g.fill();
    g.shadowColor = 'transparent';
    g.fillStyle = '#1d2030';
    g.font = `17px ${FONT}`;
    g.textBaseline = 'top';
    const lines = wrap(g, it.text, STICKY_W - 24).slice(0, 6);
    lines.forEach((l, i) => g.fillText(l, it.x + 12, it.y + 12 + i * 20));
  } else {
    g.strokeStyle = it.c;
    g.fillStyle = it.c;
    g.lineWidth = it.w;
    g.lineCap = 'round';
    g.lineJoin = 'round';
    g.beginPath();
    if (it.k === 'r') g.rect(Math.min(it.x, it.x2), Math.min(it.y, it.y2), Math.abs(it.x2 - it.x), Math.abs(it.y2 - it.y));
    else if (it.k === 'e') g.ellipse((it.x + it.x2) / 2, (it.y + it.y2) / 2, Math.abs(it.x2 - it.x) / 2, Math.abs(it.y2 - it.y) / 2, 0, 0, Math.PI * 2);
    else {
      g.moveTo(it.x, it.y);
      g.lineTo(it.x2, it.y2);
    }
    g.stroke();
    if (it.k === 'a') {
      const a = Math.atan2(it.y2 - it.y, it.x2 - it.x), h = Math.max(12, it.w * 3.2);
      g.beginPath();
      g.moveTo(it.x2, it.y2);
      g.lineTo(it.x2 - h * Math.cos(a - 0.45), it.y2 - h * Math.sin(a - 0.45));
      g.lineTo(it.x2 - h * Math.cos(a + 0.45), it.y2 - h * Math.sin(a + 0.45));
      g.closePath();
      g.fill();
    }
  }
  g.restore();
}

export function drawThumb(g, th, w, h) {
  const sx = w / 63, sy = h / 63;
  g.lineCap = 'round';
  g.lineJoin = 'round';
  for (const [k, c, v, hl] of th) {
    g.strokeStyle = c;
    g.fillStyle = c;
    if (k === 's') {
      g.globalAlpha = hl ? 0.4 : 1;
      g.lineWidth = hl ? 2.2 : 0.9;
      g.beginPath();
      g.moveTo(v[0] * sx, v[1] * sy);
      for (let i = 2; i < v.length; i += 2) g.lineTo(v[i] * sx, v[i + 1] * sy);
      if (v.length === 4 && v[0] === v[2] && v[1] === v[3]) g.lineTo(v[0] * sx + 0.5, v[1] * sy);
      g.stroke();
      g.globalAlpha = 1;
    } else if (k === 't') {
      g.globalAlpha = 0.7;
      g.fillRect(v[0] * sx, v[1] * sy, Math.max(1, (v[2] - v[0]) * sx), Math.max(1, (v[3] - v[1]) * sy * 0.5));
      g.globalAlpha = 1;
    } else if (k === 'n') {
      g.fillRect(v[0] * sx, v[1] * sy, Math.max(1, (v[2] - v[0]) * sx), Math.max(1, (v[3] - v[1]) * sy));
    } else {
      g.lineWidth = 0.9;
      g.beginPath();
      if (k === 'r') g.rect(Math.min(v[0], v[2]) * sx, Math.min(v[1], v[3]) * sy, Math.abs(v[2] - v[0]) * sx, Math.abs(v[3] - v[1]) * sy);
      else if (k === 'e') g.ellipse(((v[0] + v[2]) / 2) * sx, ((v[1] + v[3]) / 2) * sy, (Math.abs(v[2] - v[0]) / 2) * sx, (Math.abs(v[3] - v[1]) / 2) * sy, 0, 0, Math.PI * 2);
      else {
        g.moveTo(v[0] * sx, v[1] * sy);
        g.lineTo(v[2] * sx, v[3] * sy);
      }
      g.stroke();
    }
  }
}

export function renderPng(items) {
  const c = document.createElement('canvas');
  c.width = BW;
  c.height = BH;
  const g = c.getContext('2d');
  g.fillStyle = '#ffffff';
  g.fillRect(0, 0, BW, BH);
  for (const it of items) drawItem(g, it);
  return c;
}
