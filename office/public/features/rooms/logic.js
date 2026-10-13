import { TILE, ZONES, ROOMS, ROOM_ANCHORS } from '../../map.js';

export const FADE_MS = 240;
export const ARM_MS = 180;
export const DIM = 0.52;
export const TAG_DIM = 0.55;

export function focusRoom(zone) {
  return ZONES[zone]?.private && ROOMS[zone] ? zone : null;
}

export function roomRect(zone) {
  const r = ROOMS[zone];
  return { x: r.x0 * TILE, y: r.y0 * TILE, w: (r.x1 - r.x0 + 1) * TILE, h: (r.y1 - r.y0 + 1) * TILE };
}

export function fade(amount, on, dtMs, ms = FADE_MS) {
  const step = Math.max(0, dtMs) / ms;
  return on ? Math.min(1, amount + step) : Math.max(0, amount - step);
}

export function ease(a) {
  return a * a * (3 - 2 * a);
}

export function occupants(me, others, zone, zoneAt) {
  const list = [...others].filter((o) => zoneAt(o.x, o.y) === zone).sort((a, b) => a.name.localeCompare(b.name));
  return me ? [me, ...list] : list;
}

export function linkedRoom(search) {
  const z = new URLSearchParams(search).get('room') || '';
  return Object.hasOwn(ROOM_ANCHORS, z) && ZONES[z] ? z : null;
}

export function roomLink(href, zone) {
  const u = new URL(href);
  u.searchParams.set('room', zone);
  u.hash = '';
  return u.toString();
}
