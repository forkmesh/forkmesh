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

export const RECEIPT_LABELS = { pending: 'Sending', sent: 'Sent', delivered: 'Delivered', read: 'Read' };
const RECEIPT_FRONT = 'cx="14" cy="7" r="5.2"';
const RECEIPT_FRONT_TICK = 'M11.7 7.2L13.4 8.9L16.4 5.6';
const RECEIPT_BACK_TICK = 'M6.2 7.2L7.9 8.9L8.87 7.86';
const RECEIPT_BACK_TICK_READ = 'M6.2 7.2L7.33 8.33';
const RECEIPT_BACK_RING = 'M11.25 2.59A5.2 5.2 0 1 0 11.25 11.41';
const RECEIPT_BACK_DISC = 'M10.21 1.35A5.9 5.9 0 1 0 10.21 12.65A6.8 6.8 0 0 1 10.21 1.35Z';

export function receiptState(m, mine, direct) {
  if (!m || !mine) return '';
  if (direct && (m.receipt === 'delivered' || m.receipt === 'read')) return m.receipt;
  return m.receipt === 'pending' && !Number.isSafeInteger(m.recipients) ? 'pending' : 'sent';
}

export function receiptShapes(state) {
  const tick = (d, cut = false) => `<path d="${d}"${cut ? ' class="rc-cut"' : ''}/>`;
  let body;
  if (state === 'read') {
    body = `<path d="${RECEIPT_BACK_DISC}" fill="currentColor" stroke="none"/><circle ${RECEIPT_FRONT} fill="currentColor"/>${tick(RECEIPT_BACK_TICK_READ, true)}${tick(RECEIPT_FRONT_TICK, true)}`;
  } else {
    body = (state === 'delivered' ? `<path d="${RECEIPT_BACK_RING}"/>${tick(RECEIPT_BACK_TICK)}` : '')
      + `<circle ${RECEIPT_FRONT}${state === 'pending' ? ' stroke-dasharray="1.6 1.7"' : ''}/>`
      + (state === 'pending' ? '' : tick(RECEIPT_FRONT_TICK));
  }
  return body;
}
