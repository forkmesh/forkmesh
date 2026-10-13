import { html, setHtml, trusted } from '../../html.js';
const PATHS = {
  x: html`<path d="M18 6 6 18"/><path d="m6 6 12 12"/>`,
  gamepad: html`<line x1="6" x2="10" y1="11" y2="11"/><line x1="8" x2="8" y1="9" y2="13"/><line x1="15" x2="15.01" y1="12" y2="12"/><line x1="18" x2="18.01" y1="10" y2="10"/><path d="M17.32 5H6.68a4 4 0 0 0-3.978 3.59c-.006.052-.01.101-.017.152C2.604 9.416 2 14.456 2 16a3 3 0 0 0 3 3c1 0 1.5-.5 2-1l1.414-1.414A2 2 0 0 1 9.828 16h4.344a2 2 0 0 1 1.414.586L17 18c.5.5 1 1 2 1a3 3 0 0 0 3-3c0-1.545-.604-6.584-.685-7.258-.007-.05-.011-.1-.017-.151A4 4 0 0 0 17.32 5z"/>`,
  coffee: html`<path d="M10 2v2"/><path d="M14 2v2"/><path d="M16 8a1 1 0 0 1 1 1v8a4 4 0 0 1-4 4H7a4 4 0 0 1-4-4V9a1 1 0 0 1 1-1h14a4 4 0 1 1 0 8h-1"/><path d="M6 2v2"/>`,
  droplet: html`<path d="M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 5 13 5 15a7 7 0 0 0 7 7z"/>`,
  trophy: html`<path d="M6 9H4.5a2.5 2.5 0 0 1 0-5H6"/><path d="M18 9h1.5a2.5 2.5 0 0 0 0-5H18"/><path d="M4 22h16"/><path d="M10 14.66V17c0 .55-.47.98-.97 1.21C7.85 18.75 7 20.24 7 22"/><path d="M14 14.66V17c0 .55.47.98.97 1.21C16.15 18.75 17 20.24 17 22"/><path d="M18 2H6v7a6 6 0 0 0 12 0V2Z"/>`,
  table: html`<rect x="2" y="7" width="20" height="10" rx="1"/><path d="M12 7v10"/><circle cx="17" cy="4" r="1.5"/>`,
  up: html`<path d="m18 15-6-6-6 6"/>`,
  down: html`<path d="m6 9 6 6 6-6"/>`,
  left: html`<path d="m15 18-6-6 6-6"/>`,
  right: html`<path d="m9 18 6-6-6-6"/>`,
};

export function svg(name, size = 16) {
  return html`<svg class="i" width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${PATHS[name] || ''}</svg>`;
}

const open = new Set();
const GAME_KEYS = new Set(['arrowup', 'arrowdown', 'arrowleft', 'arrowright', 'w', 'a', 's', 'd', ' ', 'enter', 'escape']);

addEventListener('keydown', (e) => {
  if (!open.size || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target?.tagName || '')) return;
  const k = e.key.toLowerCase();
  if (!GAME_KEYS.has(k)) return;
  const top = [...open].pop();
  if (!top.onKey && (k === 'enter' || k === ' ')) return e.stopImmediatePropagation();
  e.preventDefault();
  e.stopImmediatePropagation();
  if (k === 'escape') return top.close();
  top.onKey?.(k, true);
}, true);
addEventListener('keyup', (e) => {
  if (!open.size) return;
  const k = e.key.toLowerCase();
  if (!GAME_KEYS.has(k)) return;
  [...open].pop().onKey?.(k, false);
}, true);

export function anyOpen() {
  return open.size > 0;
}

export function overlay(api, { title, iconName, body, onClose, onKey, modal = true }) {
  const el = document.createElement('div');
  el.className = modal ? 'pl-ov' : 'pl-ov pl-float';
  setHtml(el, html`<div class="pl-card" role="dialog" aria-label="${(title)}">
    <header class="pl-h"><span class="pl-t">${iconName ? svg(iconName, 16) : ''}<b>${(title)}</b></span><button class="icon-btn pl-x" aria-label="Close">${svg('x', 16)}</button></header>
    <div class="pl-body">${body}</div></div>`);
  const h = {
    el,
    onKey,
    q: (s) => el.querySelector(s),
    close() {
      if (!open.has(h)) return;
      open.delete(h);
      el.remove();
      onClose?.();
    },
  };
  el.querySelector('.pl-x').onclick = () => h.close();
  if (modal) el.addEventListener('pointerdown', (e) => { if (e.target === el) h.close(); });
  (document.querySelector('#stage') || document.body).append(el);
  open.add(h);
  return h;
}

export function findObjects(api, char) {
  const out = [];
  for (let y = 0; y < api.H; y++) for (let x = 0; x < api.W; x++) if (api.obj[y][x] === char) out.push({ x, y });
  return out;
}

export function nearestObject(api, char) {
  const me = api.me;
  if (!me) return null;
  let best = null, bd = Infinity;
  for (const t of findObjects(api, char)) {
    const d = Math.abs(t.x - me.x) + Math.abs(t.y - me.y);
    if (d < bd) { bd = d; best = t; }
  }
  return best;
}

export function makeArrival(api) {
  let pending = null;
  api.onTick((now) => {
    if (!pending || !api.me) return;
    const me = api.me;
    if (now > pending.until) { pending = null; return; }
    if (pending.test(me)) {
      const p = pending;
      pending = null;
      p.fn();
    }
  });
  return (test, fn, ms = 20000) => { pending = { test, fn, until: performance.now() + ms }; };
}

export function goUse(api, arrive, char, fn) {
  const t = nearestObject(api, char);
  if (!t) return;
  const near = (me) => Math.abs(me.x - t.x) + Math.abs(me.y - t.y) <= 1;
  if (near(api.me)) return fn(t.x, t.y);
  api.walkToTile(t.x, t.y);
  arrive(near, () => fn(t.x, t.y));
}
