import { TILE as T, PARK_X0 as K, W, H, obj } from '../map.js';
import { timeline } from './rides/defs.js';

export const BIG_WAVE_MS = 60000;
export const ERUPT_MS = 90000;
export const BUCKET_MS = 45000;
const POOL = { x0: K + 12, x1: K + 37, y0: 2, y1: 12 };
const CRATER = { x: (K + 51.5) * T, y: 11.5 * T };
const BUCKET = { x: (K + 22) * T, y: 29 * T };
const TUBES = [['drop', '#22c55e'], ['funnel', '#f97316'], ['blaster', '#8b5cf6']];

export const cycle = (now, period) => ((now % period) + period) % period;

export function inPool(o) {
  return o && o.x >= POOL.x0 && o.x < POOL.x1 && o.y >= POOL.y0 && o.y < POOL.y1 && obj[o.y]?.[o.x] === 'W';
}

export function tubesCanvas() {
  const cv = document.createElement('canvas');
  cv.width = (W - K) * T;
  cv.height = H * T;
  const c = cv.getContext('2d');
  c.translate(-K * T, 0);
  c.lineCap = 'round';
  c.lineJoin = 'round';
  for (const [id, color] of TUBES) {
    const s = timeline(id).s;
    let top = 0;
    for (let i = 0; i < s.length; i++) if (s[i].lift >= s[top].lift) top = i;
    const pts = s.slice(top).filter((p, i) => i % 2 === 0 && p.lift > 0.15);
    const line = (off, width, col) => {
      c.strokeStyle = col;
      c.lineWidth = width;
      c.beginPath();
      pts.forEach((p, i) => {
        const x = p.x * T, y = (p.y - (off ? p.lift : 0)) * T + 4;
        i ? c.lineTo(x, y) : c.moveTo(x, y);
      });
      c.stroke();
    };
    line(false, 14, 'rgba(0,0,0,.12)');
    line(true, 16, color);
    line(true, 8, 'rgba(255,255,255,.35)');
    c.fillStyle = '#9aa1b0';
    for (let i = 0; i < pts.length; i += 12) {
      const p = pts[i];
      c.fillRect(p.x * T - 2, (p.y - p.lift) * T + 10, 4, p.lift * T - 6);
    }
  }
  return cv;
}

export default function setup(api) {
  let skew = 0;
  let tubes = null;
  let waveTold = -1;
  let eruptTold = -1;
  const sharks = [0, 1, 2].map((i) => ({ row: i === 1 ? 7.6 : 2.6, speed: 0.9 + i * 0.35, phase: i * 2.1 }));
  const serverNow = () => Date.now() + skew;
  api.onInit((m) => { if (Number.isFinite(m.now)) skew = m.now - Date.now(); });

  const visible = () => {
    const cam = api.camera;
    if (!cam) return false;
    const half = (innerWidth / 2) / cam.zoom;
    return cam.x + half > K * T;
  };

  api.onDrawUnder((ctx) => {
    if (!visible()) return;
    if (!tubes) tubes = tubesCanvas();
    ctx.drawImage(tubes, K * T, 0);
  });

  api.onTick(() => {
    const me = api.me;
    if (!me || me.x < K) return;
    const sn = serverNow();
    const wave = Math.floor(sn / BIG_WAVE_MS);
    if (cycle(sn, BIG_WAVE_MS) < 1500 && waveTold !== wave && inPool(me)) {
      waveTold = wave;
      api.hudNote({ key: 'park-event', title: '🌊 Big wave!', body: 'The Surf Lagoon sends a big one every minute.' });
    }
    const erupt = Math.floor(sn / ERUPT_MS);
    if (cycle(sn, ERUPT_MS) < 1500 && eruptTold !== erupt && eruptTold === -1) {
      eruptTold = erupt;
      api.hudNote({ key: 'park-event', title: '🌋 The volcano is erupting!', body: 'It erupts every 90 seconds. Ride the slides from the summit stairs.' });
    }
  });

  api.onDrawWorld((ctx, now) => {
    if (!visible()) return;
    const sn = serverNow();

    for (let i = 0; i < 4; i++) {
      const y = POOL.y0 + (cycle(sn / 6000 + i / 4, 1)) * (POOL.y1 - POOL.y0 - 0.5);
      ctx.fillStyle = 'rgba(255,255,255,.28)';
      for (let x = POOL.x0; x < POOL.x1; x += 1.4) ctx.fillRect(x * T, y * T + Math.sin(x + sn / 500) * 3, T * 0.8, 3);
    }
    const bw = cycle(sn, BIG_WAVE_MS);
    if (bw < 8000) {
      const y = POOL.y0 + (bw / 8000) * (POOL.y1 - POOL.y0);
      ctx.fillStyle = 'rgba(255,255,255,.75)';
      ctx.fillRect(POOL.x0 * T, y * T - 6, (POOL.x1 - POOL.x0) * T, 7);
      ctx.fillStyle = 'rgba(160,225,250,.6)';
      ctx.fillRect(POOL.x0 * T, y * T + 1, (POOL.x1 - POOL.x0) * T, 10);
    }

    ctx.fillStyle = 'rgba(255,255,255,.55)';
    const ring = [[41, 10], [62, 10], [62, 29], [41, 29]];
    const per = 84;
    for (let i = 0; i < 28; i++) {
      let d = cycle(i * 3 + sn / 600, per);
      let k = 0;
      while (k < 4) {
        const [ax, ay] = ring[k], [bx, by] = ring[(k + 1) % 4];
        const len = Math.abs(bx - ax) + Math.abs(by - ay);
        if (d <= len) {
          const t = d / len;
          ctx.fillRect((K + ax + (bx - ax) * t) * T - 3, (ay + (by - ay) * t) * T, 7, 2);
          break;
        }
        d -= len;
        k++;
      }
    }

    const er = cycle(sn, ERUPT_MS);
    const erupting = er < 6000;
    for (let i = 0; i < 6; i++) {
      const t = cycle(now / 2600 + i / 6, 1);
      const r = 8 + t * 22;
      ctx.fillStyle = `rgba(${erupting ? '90,80,80' : '200,200,205'},${0.45 * (1 - t)})`;
      ctx.beginPath();
      ctx.arc(CRATER.x + Math.sin(i * 2 + t * 3) * 14, CRATER.y - 10 - t * 110, r, 0, Math.PI * 2);
      ctx.fill();
    }
    if (erupting) {
      ctx.fillStyle = 'rgba(255,140,40,.35)';
      ctx.beginPath();
      ctx.arc(CRATER.x, CRATER.y, 60 + Math.sin(now / 80) * 8, 0, Math.PI * 2);
      ctx.fill();
      for (let i = 0; i < 26; i++) {
        const t = cycle(now / 1400 + i / 26, 1);
        const a = -Math.PI / 2 + (((i * 37) % 26) / 26 - 0.5) * 1.6;
        const v = 150 + (i % 5) * 30;
        const x = CRATER.x + Math.cos(a) * v * t;
        const y = CRATER.y + Math.sin(a) * v * t + 260 * t * t;
        ctx.fillStyle = i % 3 ? '#ff7a1a' : '#ffd84d';
        ctx.fillRect(x - 3, y - 3, 6, 6);
      }
    }

    const bk = cycle(sn, BUCKET_MS);
    const tipping = bk > BUCKET_MS - 2200;
    const fill = Math.min(1, bk / (BUCKET_MS - 2200));
    ctx.save();
    ctx.translate(BUCKET.x, BUCKET.y - 30);
    ctx.rotate(tipping ? Math.min(1.9, (bk - (BUCKET_MS - 2200)) / 300) : Math.sin(now / 400) * 0.03);
    ctx.fillStyle = '#f59e0b';
    ctx.fillRect(-22, -20, 44, 30);
    ctx.fillStyle = '#4fb0d6';
    ctx.fillRect(-19, 7 - 25 * (tipping ? 0.3 : fill), 38, 25 * (tipping ? 0.3 : fill));
    ctx.restore();
    if (tipping) {
      const p = (bk - (BUCKET_MS - 2200)) / 2200;
      ctx.fillStyle = `rgba(150,215,240,${0.85 * (1 - p)})`;
      ctx.fillRect(BUCKET.x - 40 - p * 30, BUCKET.y - 10, 80 + p * 60, 4 * T * Math.min(1, p * 3));
      for (let i = 0; i < 18; i++) {
        const a = (i / 18) * Math.PI * 2;
        ctx.fillStyle = '#d6f0fa';
        ctx.fillRect(BUCKET.x + Math.cos(a) * p * 110, BUCKET.y + 3.4 * T + Math.sin(a) * p * 40, 4, 4);
      }
    } else {
      const left = Math.ceil((BUCKET_MS - 2200 - bk) / 1000);
      if (left <= 10) {
        ctx.fillStyle = '#1c5f91';
        ctx.font = '700 14px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(`💦 ${left}`, BUCKET.x, BUCKET.y - 60);
      }
    }

    for (const s of sharks) {
      const t = cycle(now / 1000 * s.speed + s.phase, 20);
      const x = K + 64.5 + (t < 10 ? t : 20 - t) * 2;
      const dir = t < 10 ? 1 : -1;
      const y = s.row * T + Math.sin(now / 500 + s.phase) * 3;
      ctx.fillStyle = 'rgba(60,80,100,.85)';
      ctx.beginPath();
      ctx.ellipse(x * T, y, 20, 6, 0, 0, Math.PI * 2);
      ctx.fill();
      ctx.beginPath();
      ctx.moveTo(x * T - dir * 4, y - 4);
      ctx.lineTo(x * T - dir * 10, y - 14);
      ctx.lineTo(x * T + dir * 4, y - 4);
      ctx.fill();
      ctx.beginPath();
      ctx.moveTo(x * T - dir * 18, y);
      ctx.lineTo(x * T - dir * 28, y - 8);
      ctx.lineTo(x * T - dir * 28, y + 8);
      ctx.fill();
    }

    for (let i = 0; i < 7; i++) {
      const jx = (K + 13 + i * 3) * T + 16, jy = (i % 2 ? 37 : 39) * T;
      const h = 20 + 22 * Math.abs(Math.sin(now / 600 + i));
      ctx.fillStyle = 'rgba(150,215,240,.75)';
      ctx.fillRect(jx - 2, jy - h, 4, h);
      ctx.fillStyle = 'rgba(214,240,250,.9)';
      ctx.fillRect(jx - 5, jy - h, 10, 3);
    }

    ctx.fillStyle = 'rgba(255,255,255,.55)';
    for (let i = 0; i < 9; i++) {
      const x = (K + 66) * T + cycle(i * 40 - now / 6, 9 * T);
      ctx.fillRect(x, 35 * T + 8 + (i % 4) * 36, 22, 3);
    }

    const fx = (K + 5) * T, fy = 33 * T;
    ctx.strokeStyle = 'rgba(255,255,255,.6)';
    ctx.lineWidth = 2;
    for (let i = 0; i < 5; i++) {
      const r = (0.5 + i * 0.45) * T;
      const a = now / (300 + i * 120) + i;
      ctx.beginPath();
      ctx.ellipse(fx, fy, r, r * 0.9, 0, a, a + 1.6);
      ctx.stroke();
    }

    ctx.fillStyle = 'rgba(220,245,255,.7)';
    for (let i = 0; i < 4; i++) {
      const y = 9 * T - 30 + cycle(now / 8 + i * 9, 30);
      ctx.fillRect((K + 50) * T + 6 + i * 14, y, 6, 4);
    }
  });
}
