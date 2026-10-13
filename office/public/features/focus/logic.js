import { cleanText } from '../work/logic.js';
import { cleanEmoji } from '../presence/logic.js';

export const MODES = ['focused', 'pomodoro', 'freeform'];
export const MODE_LABEL = { focused: 'Focused', pomodoro: 'Pomodoro', freeform: 'Freeform' };
export const MINUTES = [15, 25, 30, 45, 60, 90, 120];
export const ROUNDS_MAX = 6;
export const WORK_MS = 25 * 60e3;
export const BREAK_MS = 5 * 60e3;
export const TOPIC_MAX = 60;

export function cleanFocus(v, seated, now) {
  if (!v || typeof v !== 'object' || Array.isArray(v) || !seated) return undefined;
  const mode = String(v.mode ?? '');
  if (!MODES.includes(mode)) return undefined;
  const out = { mode, at: now };
  const topic = cleanText(v.topic, TOPIC_MAX);
  if (topic) out.topic = topic;
  const emoji = v.emoji == null ? '' : cleanEmoji(v.emoji);
  if (emoji) out.emoji = emoji;
  if (mode === 'focused') {
    if (!MINUTES.includes(v.mins)) return undefined;
    out.mins = v.mins;
    out.end = now + v.mins * 60e3;
  } else if (mode === 'pomodoro') {
    if (!Number.isInteger(v.rounds) || v.rounds < 1 || v.rounds > ROUNDS_MAX) return undefined;
    out.rounds = v.rounds;
    out.end = now + v.rounds * (WORK_MS + BREAK_MS) - BREAK_MS;
  }
  return out;
}

export function joinFocus(session, hostId, seated, now) {
  if (!session || typeof session !== 'object' || !seated || !hostId || !MODES.includes(session.mode)) return undefined;
  if (session.end !== undefined && !(session.end > now)) return undefined;
  const out = { mode: session.mode, at: session.at, host: session.host || hostId };
  for (const k of ['topic', 'emoji', 'mins', 'rounds', 'end']) if (session[k] !== undefined) out[k] = session[k];
  return out;
}

export function roundsMinutes(rounds) {
  return (rounds * (WORK_MS + BREAK_MS) - BREAK_MS) / 60e3;
}

export function phase(f, now) {
  if (!f || !MODES.includes(f.mode)) return null;
  if (f.mode === 'freeform') return { kind: 'work', left: null, elapsed: Math.max(0, now - f.at) };
  if (!(f.end > now)) return { kind: 'done', left: 0 };
  if (f.mode === 'pomodoro') {
    const cycle = WORK_MS + BREAK_MS;
    const t = Math.max(0, now - f.at);
    const pos = t % cycle;
    const round = Math.floor(t / cycle) + 1;
    return pos < WORK_MS ? { kind: 'work', left: WORK_MS - pos, round } : { kind: 'break', left: cycle - pos, round };
  }
  return { kind: 'work', left: f.end - now };
}

export function fmtClock(ms) {
  const s = Math.max(0, Math.ceil(ms / 1000));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  return h ? `${h}:${pad(m)}:${pad(r)}` : `${m}:${pad(r)}`;
}

export function describe(f) {
  if (f?.mode === 'focused') return `${f.mins} min`;
  if (f?.mode === 'pomodoro') return `${f.rounds} × 25 min`;
  return 'No timer';
}

export function shortLeft(f, now) {
  const p = phase(f, now);
  if (!p || p.kind === 'done') return '';
  if (p.left === null) return 'Focus';
  return `${p.kind === 'break' ? 'Break ' : ''}${Math.max(1, Math.ceil(p.left / 60e3))}m`;
}

export function nearestCouch(couches, taken, from) {
  let best = null, bd = Infinity;
  for (const c of couches) {
    if (taken.has(c.y * 1000 + c.x)) continue;
    const d = Math.abs(c.x - from.x) + Math.abs(c.y - from.y);
    if (d < bd) { bd = d; best = c; }
  }
  return best;
}

export const RECENT_KEY = 'office-focus-recent';
export const RECENT_MAX = 5;
export const FOCUS_EMOJI = ['🎧', '💻', '📚', '✍️', '🎨', '🧠', '🚀', '🐛', '📈', '☕'];

export function recentOf(f, opts = {}) {
  const c = cleanFocus(f, true, 0);
  if (!c) return undefined;
  const out = { mode: c.mode };
  for (const k of ['topic', 'emoji', 'mins', 'rounds']) if (c[k] !== undefined) out[k] = c[k];
  out.status = opts.status !== false;
  out.notify = opts.notify === true;
  return out;
}

export const recentKey = (r) => [r.mode, r.mins || '', r.rounds || '', r.emoji || '', (r.topic || '').toLowerCase()].join('|');

export function cleanRecent(list) {
  const out = [];
  const seen = new Set();
  for (const item of Array.isArray(list) ? list : []) {
    const r = recentOf(item, { status: item?.status, notify: item?.notify });
    if (!r || seen.has(recentKey(r))) continue;
    seen.add(recentKey(r));
    out.push(r);
    if (out.length >= RECENT_MAX) break;
  }
  return out;
}

export function pushRecent(list, f, opts) {
  const r = recentOf(f, opts);
  if (!r) return cleanRecent(list);
  return cleanRecent([r, ...cleanRecent(list).filter((x) => recentKey(x) !== recentKey(r))]);
}

export function readRecent(text) {
  try {
    return cleanRecent(JSON.parse(text || '[]'));
  } catch {
    return [];
  }
}

export function recentLabel(r) {
  return `${r.emoji ? r.emoji + ' ' : ''}${r.topic || MODE_LABEL[r.mode]} · ${describe(r)}`;
}

export function startMessage(r) {
  const m = { t: 'focus-start', mode: r.mode, notify: r.notify === true };
  if (r.topic) m.topic = r.topic;
  if (r.emoji) m.emoji = r.emoji;
  if (r.mode === 'focused') m.mins = r.mins;
  if (r.mode === 'pomodoro') m.rounds = r.rounds;
  return m;
}

export function focusStatusOf(f) {
  return { s: 'focus', emoji: f?.emoji || '🎧', text: f?.topic || 'Focus session' };
}
