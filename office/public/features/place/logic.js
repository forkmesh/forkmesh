import { isWalkable } from '../../map.js';

export const PLACE_PREFIX = 'office-place:';
export const MAX_AGE_MS = 7 * 24 * 60 * 60 * 1000;
const ORG_RE = /^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/;

export function placeKey(org) {
  const o = String(org || '').toLowerCase();
  return PLACE_PREFIX + (ORG_RE.test(o) ? o : '_');
}

export function encodePlace(me, now) {
  return JSON.stringify({ x: me.x, y: me.y, d: me.d & 3, at: now });
}

export function readPlace(text, now) {
  let v;
  try {
    v = JSON.parse(text || 'null');
  } catch {
    return null;
  }
  if (!v || typeof v !== 'object' || !Number.isInteger(v.x) || !Number.isInteger(v.y)) return null;
  if (!Number.isFinite(v.at) || now - v.at > MAX_AGE_MS || v.at > now + 60e3) return null;
  if (!isWalkable(v.x, v.y)) return null;
  return { x: v.x, y: v.y, d: Number.isInteger(v.d) ? v.d & 3 : 0 };
}
