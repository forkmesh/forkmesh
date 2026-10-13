import { OFFICE_W as W, H, COASTER_PLATFORM } from '../../map.js';

export const SPEED = 8;
export const STEP = 0.08;
export const LOOP_R = 2.2;
export const CLOCK_SLACK_MS = 5000;
const LEFT = 0.5, TOP = 0.5, RIGHT = W - 0.5, BOTTOM = H - 0.5, CORNER = 1;

function line(out, x0, y0, x1, y1, loops = []) {
  const len = Math.hypot(x1 - x0, y1 - y0);
  const dx = (x1 - x0) / len, dy = (y1 - y0) / len;
  const marks = loops.map((t) => t * len).sort((a, b) => a - b);
  let s = 0;
  for (const m of marks) {
    for (; s < m; s += STEP) out.push({ x: x0 + dx * s, y: y0 + dy * s, lift: 0, loop: false });
    const cx = x0 + dx * m, cy = y0 + dy * m;
    const n = Math.ceil((2 * Math.PI * LOOP_R) / STEP);
    for (let i = 0; i < n; i++) {
      const a = (i / n) * 2 * Math.PI;
      const fwd = LOOP_R * Math.sin(a), up = LOOP_R * (1 - Math.cos(a));
      if (dy === 0) out.push({ x: cx + dx * fwd, y: cy, lift: up, loop: true });
      else out.push({ x: cx + (dy > 0 ? -1 : 1) * up, y: cy + dy * fwd, lift: 0, loop: true });
    }
  }
  for (; s < len; s += STEP) out.push({ x: x0 + dx * s, y: y0 + dy * s, lift: 0, loop: false });
}

function arc(out, cx, cy, a0, a1) {
  const n = Math.ceil((Math.abs(a1 - a0) * CORNER) / STEP);
  for (let i = 0; i < n; i++) {
    const a = a0 + ((a1 - a0) * i) / n;
    out.push({ x: cx + Math.cos(a) * CORNER, y: cy + Math.sin(a) * CORNER, lift: 0, loop: false });
  }
}

export function buildTrack() {
  const pts = [];
  const sx = COASTER_PLATFORM.x1 + 0.5;
  const h = Math.PI / 2;
  line(pts, sx, TOP, RIGHT - CORNER, TOP);
  arc(pts, RIGHT - CORNER, TOP + CORNER, -h, 0);
  line(pts, RIGHT, TOP + CORNER, RIGHT, BOTTOM - CORNER, [0.5]);
  arc(pts, RIGHT - CORNER, BOTTOM - CORNER, 0, h);
  line(pts, RIGHT - CORNER, BOTTOM, LEFT + CORNER, BOTTOM, [0.17]);
  arc(pts, LEFT + CORNER, BOTTOM - CORNER, h, 2 * h);
  line(pts, LEFT, BOTTOM - CORNER, LEFT, TOP + CORNER, [0.5]);
  arc(pts, LEFT + CORNER, TOP + CORNER, 2 * h, 3 * h);
  line(pts, LEFT + CORNER, TOP, sx, TOP);
  return { pts, length: pts.length * STEP, start: { x: sx, y: TOP } };
}

export function pointAt(track, s) {
  const i = Math.max(0, Math.min(track.pts.length - 1, Math.floor(s / STEP)));
  return track.pts[i];
}

export function rideDuration(track) {
  return (track.length / SPEED) * 1000;
}

export function rideState(at, now, track) {
  if (!Number.isFinite(at)) return null;
  const t = now - at;
  if (t < -CLOCK_SLACK_MS || t > rideDuration(track)) return null;
  return pointAt(track, (Math.max(0, t) / 1000) * SPEED);
}

export function loopCenters(track) {
  const out = [];
  let run = null;
  for (const p of track.pts) {
    if (p.loop && !run) run = [];
    if (p.loop) run.push(p);
    else if (run) { out.push(run); run = null; }
  }
  return out.map((r) => {
    const xs = r.map((p) => p.x), ys = r.map((p) => p.y - p.lift);
    return { x: (Math.min(...xs) + Math.max(...xs)) / 2, y: (Math.min(...ys) + Math.max(...ys)) / 2, r: LOOP_R };
  });
}
