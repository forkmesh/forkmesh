export const ROOMS = [
  { ch: 'general', label: 'general', zone: null },
  { ch: 'engineering', label: 'engineering', zone: 'a' },
  { ch: 'design', label: 'design', zone: 'b' },
  { ch: 'coffee', label: 'coffee-huddle', zone: 'c' },
  { ch: 'lobby', label: 'lobby', zone: 'l' },
  { ch: 'garden', label: 'garden', zone: 'p' },
];
export const ROOM_IDS = new Set(ROOMS.map((r) => r.ch));
export const PAGE = 50;
export const MAX_TEXT = 1000;

export function chanForZone(z) {
  return ROOMS.find((r) => r.zone && r.zone === z)?.ch || null;
}

export function roomLabel(ch) {
  return ROOMS.find((r) => r.ch === ch)?.label || ch;
}

export function mentionQuery(value, caret) {
  const m = /(^|\s)@([^\s@]{0,24})$/.exec(value.slice(0, caret));
  return m ? { start: caret - m[2].length - 1, q: m[2] } : null;
}
