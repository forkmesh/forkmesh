import { html, setHtml } from './html.js';
import { icon } from './icons.js';

export const NOTE_MS = 10000;
export const NOTE_MAX = 3;
const live = new WeakMap();

export function setNote(at, note) {
  if (!at) return null;
  const prev = live.get(at);
  clearTimeout(prev?.timer);
  live.delete(at);
  if (!note || (!note.title && !note.body)) {
    at.hidden = true;
    setHtml(at, html``);
    prev?.onClear?.();
    return null;
  }
  const { title = '', body = '', tone = 'info', actions = [], dismiss = false, ms = 0, onClear } = note;
  at.classList.add('note');
  at.dataset.tone = tone;
  at.setAttribute('role', tone === 'error' ? 'alert' : 'status');
  setHtml(at, html`<span class="note-t">${title ? html`<b>${title}</b>` : ''}${body ? html`<span>${body}</span>` : ''}</span>${actions.length ? html`<span class="note-acts">${actions.map((a, i) => html`<button type="button" class="btn btn-sm${a.primary ? ' btn-primary' : ''}" data-note="${i}">${a.iconHtml || (a.icon ? icon(a.icon, 14) : '')}${a.label}</button>`)}</span>` : ''}${dismiss ? html`<button type="button" class="icon-btn note-x" data-note="x" aria-label="Dismiss">${icon('x', 12)}</button>` : ''}`);
  at.onclick = (e) => {
    const k = e.target.closest('[data-note]')?.dataset.note;
    if (k === undefined) return;
    setNote(at, null);
    if (k !== 'x') actions[+k]?.run?.();
  };
  at.hidden = false;
  const entry = { onClear };
  if (ms && tone !== 'error') entry.timer = setTimeout(() => setNote(at, null), ms);
  live.set(at, entry);
  return at;
}

export function noteLine(box, { key = 'main', ...note } = {}, make = () => document.createElement('div')) {
  if (!box) return null;
  let el = [...box.children].find((c) => c.dataset.key === key);
  if (!note.title && !note.body) return el ? setNote(el, null) : null;
  if (!el) {
    el = make();
    el.dataset.key = key;
    box.append(el);
    while (box.children.length > NOTE_MAX) {
      const old = box.firstElementChild;
      setNote(old, null);
      old.remove();
    }
  }
  const line = el;
  const ms = note.ms ?? (note.actions?.length || note.tone === 'error' ? 0 : NOTE_MS);
  return setNote(line, { ...note, dismiss: note.dismiss ?? true, ms, onClear: () => { line.remove(); note.onClear?.(); } });
}
