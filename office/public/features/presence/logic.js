export const STATUSES = ['available', 'focus', 'meeting', 'away', 'break'];
export const STATUS_LABEL = { available: 'Available', focus: 'Focus', meeting: 'In a meeting', away: 'Away', break: 'On a break' };
export const STATUS_COLOR = { available: '#22c55e', focus: '#dc2626', meeting: '#3b45e0', away: '#f59e0b', break: '#92400e' };
export const STATUS_EMOJI = { focus: '🎧', away: '💤', break: '☕' };
export const TEXT_MAX = 60;
export const IDLE_MS = 10 * 60e3;

const seg = typeof Intl !== 'undefined' && Intl.Segmenter ? new Intl.Segmenter(undefined, { granularity: 'grapheme' }) : null;
const graphemes = (s) => (seg ? [...seg.segment(s)].map((x) => x.segment) : [...s]);

export function cleanEmoji(v) {
  const emoji = String(v ?? '').replace(/[\u0000-\u001f\u007f]/g, '').trim();
  if (!emoji) return '';
  const g = graphemes(emoji)[0];
  if (g.length > 16 || /[\p{L}\p{N}<>&"'`]/u.test(g)) return undefined;
  return g;
}

export function cleanStatus(v) {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return undefined;
  const s = String(v.s || '');
  if (!STATUSES.includes(s)) return undefined;
  const out = { s };
  const emoji = cleanEmoji(v.emoji);
  if (emoji === undefined) return undefined;
  if (emoji) out.emoji = emoji;
  const text = String(v.text ?? '').replace(/[\u0000-\u001f\u007f]/g, ' ').replace(/\s+/g, ' ').trim();
  if (text) out.text = graphemes(text).slice(0, TEXT_MAX).join('');
  if (v.auto === true && s === 'away') out.auto = true;
  return out;
}

export function effective(o) {
  const st = o?.ex?.status;
  if (o?.ex?.meeting && st?.s !== 'away') return 'meeting';
  if (st && STATUSES.includes(st.s)) return st.s;
  return o?.away ? 'away' : 'available';
}

export function statusLine(o) {
  const st = o?.ex?.status;
  const label = STATUS_LABEL[effective(o)];
  return st?.text ? `${label}: ${st.text}` : label;
}

export function badgeEmoji(o) {
  const st = o?.ex?.status;
  if (st?.emoji) return st.emoji;
  return STATUS_EMOJI[effective(o)] || '';
}

export function sameTable(a, b) {
  const x = a?.ex?.meeting?.table, y = b?.ex?.meeting?.table;
  return x != null && x === y;
}

export function focusBlocks(me, o) {
  if (effective(me) !== 'focus' && effective(o) !== 'focus') return false;
  return !sameTable(me, o);
}

export function shouldAutoAway(now, lastInput, hiddenSince, idle = IDLE_MS) {
  if (now - lastInput >= idle) return true;
  return hiddenSince != null && now - hiddenSince >= idle;
}

export function fmtDur(ms) {
  const m = Math.max(0, Math.round(ms / 60e3));
  if (m < 1) return '<1m';
  const h = Math.floor(m / 60);
  return h ? `${h}h ${String(m % 60).padStart(2, '0')}m` : `${m}m`;
}

export function followStep(meXY, target, last, pathLen, now) {
  if (!target) return 'stop';
  const moved = !last || last.x !== target.x || last.y !== target.y;
  const dist = Math.abs(meXY.x - target.x) + Math.abs(meXY.y - target.y);
  if (dist <= 1) return 'wait';
  if (moved) return 'walk';
  if (!pathLen && dist > 1 && now - (last.at || 0) > 2000) return 'walk';
  return 'wait';
}
