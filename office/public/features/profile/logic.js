import { cleanText } from '../work/logic.js';

export const NAME_MAX = 24;
export const TZ_MAX = 64;
export const PIC_SIZES = [256, 192, 128, 96];
export const PIC_MAX_BYTES = 256 * 1024;
const TZ_RE = /^[A-Za-z][A-Za-z0-9_+-]*(?:\/[A-Za-z0-9_+-]+){0,2}$/;
const PIC_RE = /^[0-9a-f]{16}$/;

export const cleanName = (v) => cleanText(v, NAME_MAX);

export function cleanTz(v) {
  const s = String(v ?? '').trim();
  return s.length <= TZ_MAX && TZ_RE.test(s) ? s : '';
}

export function cleanPic(v) {
  const s = String(v ?? '');
  return PIC_RE.test(s) ? s : '';
}

export function picUrl(account, pic) {
  const a = String(account ?? '').trim();
  return a && cleanPic(pic) ? `/api/accounts/${encodeURIComponent(a)}/avatar?v=${pic}` : '';
}

export function localTime(tz, now = Date.now()) {
  if (!cleanTz(tz)) return '';
  try {
    return new Intl.DateTimeFormat('en-GB', { hour: '2-digit', minute: '2-digit', hourCycle: 'h23', timeZone: tz }).format(now);
  } catch {
    return '';
  }
}

export function tzOffset(tz, now = Date.now()) {
  try {
    const part = new Intl.DateTimeFormat('en-US', { timeZone: tz, timeZoneName: 'longOffset' }).formatToParts(now).find((p) => p.type === 'timeZoneName');
    const m = /GMT(?:([+-])(\d{1,2})(?::?(\d{2}))?)?/.exec(part?.value || '');
    if (!m) return null;
    return (m[1] === '-' ? -1 : 1) * ((+m[2] || 0) * 60 + (+m[3] || 0));
  } catch {
    return null;
  }
}

export function fmtOffset(min) {
  const a = Math.abs(min);
  return `UTC${min < 0 ? '-' : '+'}${String((a / 60) | 0).padStart(2, '0')}:${String(a % 60).padStart(2, '0')}`;
}

export function browserTz() {
  try {
    return cleanTz(new Intl.DateTimeFormat().resolvedOptions().timeZone);
  } catch {
    return '';
  }
}

export function tzList(extra = [], now = Date.now()) {
  let names = [];
  try {
    names = Intl.supportedValuesOf('timeZone');
  } catch {}
  const seen = new Set();
  const out = [];
  for (const raw of ['UTC', ...extra, ...names]) {
    const tz = cleanTz(raw);
    if (!tz || seen.has(tz)) continue;
    seen.add(tz);
    const off = tzOffset(tz, now);
    if (off === null) continue;
    out.push({ tz, off, label: `(${fmtOffset(off)}) ${tz.replace(/_/g, ' ')}` });
  }
  return out.sort((a, b) => a.off - b.off || a.tz.localeCompare(b.tz));
}

export function fitSquare(w, h) {
  const side = Math.min(w, h);
  return { sx: (w - side) / 2, sy: (h - side) / 2, side };
}

export function b64Bytes(dataUrl) {
  const b64 = String(dataUrl || '').split(',')[1] || '';
  const pad = b64.endsWith('==') ? 2 : b64.endsWith('=') ? 1 : 0;
  return Math.max(0, (b64.length / 4) * 3 - pad);
}

export function handleOf(o) {
  return String(o?.ex?.fm?.a || '').trim();
}
