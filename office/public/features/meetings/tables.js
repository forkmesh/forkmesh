import { W, H, ZONES, obj, floor, chairDir } from '../../map.js';

export const NEAR_TILES = 1;
export const DROP_TILES = 5;
export const MIN_VOLUME = 0.15;
export const KNOCK_GAP_MS = 15000;
export const KNOCK_TTL_MS = 120000;
export const SEAT_SETTLE_MS = 450;

function findTables() {
  const seen = new Uint8Array(W * H);
  const out = [];
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      if (obj[y][x] !== 'T' || seen[y * W + x]) continue;
      const tiles = [];
      const q = [[x, y]];
      seen[y * W + x] = 1;
      while (q.length) {
        const [cx, cy] = q.pop();
        tiles.push([cx, cy]);
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const nx = cx + dx, ny = cy + dy;
          if (nx < 0 || ny < 0 || nx >= W || ny >= H || seen[ny * W + nx] || obj[ny][nx] !== 'T') continue;
          seen[ny * W + nx] = 1;
          q.push([nx, ny]);
        }
      }
      const xs = tiles.map((t) => t[0]), ys = tiles.map((t) => t[1]);
      const x0 = Math.min(...xs), y0 = Math.min(...ys), x1 = Math.max(...xs), y1 = Math.max(...ys);
      const chairs = [];
      const key = new Set();
      for (const [tx, ty] of tiles) {
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const nx = tx + dx, ny = ty + dy, k = ny * W + nx;
          if (chairDir(nx, ny) === undefined || key.has(k)) continue;
          key.add(k);
          chairs.push([nx, ny]);
        }
      }
      if (!chairs.length) continue;
      const zone = floor[y0][x0];
      const cxs = chairs.map((c) => c[0]), cys = chairs.map((c) => c[1]);
      out.push({
        id: `${zone}-${x0}-${y0}`,
        zone,
        name: `${ZONES[zone]?.name || 'Meeting'} table`,
        tiles,
        chairs,
        box: { x0: Math.min(x0, ...cxs), y0: Math.min(y0, ...cys), x1: Math.max(x1, ...cxs), y1: Math.max(y1, ...cys) },
      });
    }
  }
  return out;
}

export const TABLES = findTables();
export const TABLE_BY_ID = new Map(TABLES.map((t) => [t.id, t]));
const SEAT_TABLE = new Map();
for (const t of TABLES) for (const [x, y] of t.chairs) SEAT_TABLE.set(y * W + x, t.id);

export function tableAtSeat(x, y) {
  if (!Number.isInteger(x) || !Number.isInteger(y) || x < 0 || y < 0 || x >= W || y >= H) return null;
  return SEAT_TABLE.get(y * W + x) || null;
}

export function cleanMeeting(v, x, y, now = Date.now()) {
  if (!v || typeof v !== 'object') return undefined;
  const id = String(v.table || '');
  if (!TABLE_BY_ID.has(id) || tableAtSeat(x, y) !== id) return undefined;
  return { table: id, at: now };
}

export function volumeFor(dist, near = NEAR_TILES, drop = DROP_TILES, min = MIN_VOLUME) {
  if (!(dist > near)) return 1;
  if (dist >= drop) return min;
  const k = (dist - near) / (drop - near);
  return 1 - (1 - min) * (k * k * (3 - 2 * k));
}

export function smooth(current, target, dtMs, tauMs = 250) {
  if (!Number.isFinite(current)) return target;
  const a = 1 - Math.exp(-Math.max(0, dtMs) / tauMs);
  const v = current + (target - current) * a;
  return Math.abs(v - target) < 0.005 ? target : v;
}

export function fullVolume(me, o, zoneAt) {
  if (!me || !o) return true;
  const mt = me.ex?.meeting?.table;
  if (mt && mt === o.ex?.meeting?.table) return true;
  return !!(ZONES[zoneAt(me.x, me.y)]?.private || ZONES[zoneAt(o.x, o.y)]?.private);
}

export function meetingVerdict(me, o) {
  const mine = me?.ex?.meeting?.table || null;
  const theirs = o?.ex?.meeting?.table || null;
  if (mine) return theirs === mine;
  if (theirs) return false;
  return undefined;
}

export function knockAllowed(last, now, gap = KNOCK_GAP_MS) {
  return !last || now - last >= gap;
}

export function meetingSince(people, table) {
  let at = Infinity;
  for (const o of people) if (o?.ex?.meeting?.table === table && Number.isFinite(o.ex.meeting.at)) at = Math.min(at, o.ex.meeting.at);
  return Number.isFinite(at) ? at : null;
}

export function fmtDuration(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return h ? `${h}:${pad(m)}:${pad(r)}` : `${m}:${pad(r)}`;
}
