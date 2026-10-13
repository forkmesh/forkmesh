import { html, setHtml } from '../../html.js';
import { T, HEAD, SPRITE_W, SPRITE_H, SLOT_LABEL, DESK_COLORS, DESK_NAMES, CHAIR_COLORS, CHAIR_NAMES, ITEMS, CROP, cleanDeco, itemsFor, drawDeco, sampleFor } from './deco.js';

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const IC = {
  x: html`<path d="M18 6 6 18"/><path d="m6 6 12 12"/>`,
  undo: html`<path d="M9 14 4 9l5-5"/><path d="M4 9h10.5a5.5 5.5 0 0 1 0 11H11"/>`,
  redo: html`<path d="m15 14 5-5-5-5"/><path d="M20 9H9.5a5.5 5.5 0 0 0 0 11H13"/>`,
};
const SCALE = 3;

export function decoSprite(deco) {
  const cv = document.createElement('canvas');
  cv.width = SPRITE_W;
  cv.height = SPRITE_H;
  drawDeco(cv.getContext('2d'), deco);
  return cv;
}

export function openDecorator(api, { desk, deco, onDraft, onSave, onClose }) {
  const history = [cleanDeco(deco) || { desk: 0, chair: 0, slots: [0, 0, 0, 0] }];
  let at = 0;
  let slot = 0;
  let query = '';
  let busy = false;
  const cur = () => history[at];

  const el = document.createElement('div');
  el.className = 'dk-panel';
  el.setAttribute('role', 'dialog');
  el.setAttribute('aria-label', 'Decorator');
  setHtml(el, html`<header class="dk-h"><b>Decorator</b><button type="button" class="icon-btn" data-dk="close" aria-label="Close">${svg(IC.x, 14)}</button></header>
    <div class="dk-stage"><canvas width="${SPRITE_W * SCALE}" height="${SPRITE_H * SCALE}"></canvas><span class="muted">Change your desk and chair, and add decorations.</span></div>
    <div class="dk-slots"></div>
    <label class="search"><input data-dk="q" placeholder="Search for a decoration" autocomplete="off"></label>
    <div class="dk-grid"></div>
    <div class="dk-row" data-dk="desk"></div>
    <div class="dk-row" data-dk="chair"></div>
    <p class="dk-note" hidden></p>
    <footer class="dk-foot"><button type="button" class="icon-btn" data-dk="undo" data-tip="Undo" aria-label="Undo">${svg(IC.undo, 15)}</button><button type="button" class="icon-btn" data-dk="redo" data-tip="Redo" aria-label="Redo">${svg(IC.redo, 15)}</button><span></span><button type="button" class="btn btn-sm" data-dk="close">Cancel</button><button type="button" class="btn btn-sm btn-primary" data-dk="save">Save</button></footer>`);
  (api.$('#stage') || document.body).append(el);

  function paintStage() {
    const cv = el.querySelector('.dk-stage canvas');
    const c = cv.getContext('2d');
    c.imageSmoothingEnabled = false;
    c.clearRect(0, 0, cv.width, cv.height);
    if (api.mapCanvas) c.drawImage(api.mapCanvas, desk.mx * T, desk.my * T - HEAD, SPRITE_W, SPRITE_H, 0, 0, cv.width, cv.height);
    c.drawImage(decoSprite(cur()), 0, 0, cv.width, cv.height);
  }

  function paintSlots() {
    setHtml(el.querySelector('.dk-slots'), html`${SLOT_LABEL.map((label, i) => html`<button type="button" data-slot="${i}" class="${i === slot ? 'on' : ''}"><small>${label}</small><b>${ITEMS[cur().slots[i]]?.name || 'Empty'}</b></button>`)}`);
  }

  function paintGrid() {
    const q = query.trim().toLowerCase();
    const list = itemsFor(slot).filter((it) => !q || it.name.toLowerCase().includes(q));
    const grid = el.querySelector('.dk-grid');
    setHtml(grid, html`<button type="button" data-item="0" class="${cur().slots[slot] === 0 ? 'on' : ''}"><span class="dk-none"></span><small>None</small></button>${
      list.map((it) => html`<button type="button" data-item="${it.id}" class="${cur().slots[slot] === it.id ? 'on' : ''}"><canvas width="44" height="44"></canvas><small>${it.name}</small></button>`)}${
      list.length ? '' : html`<p class="muted">Nothing matches.</p>`}`);
    grid.querySelectorAll('canvas').forEach((cv, i) => {
      const it = list[i];
      const [sx, sy, sw, sh] = CROP[it.kind];
      const k = Math.min(44 / sw, 44 / sh);
      const c = cv.getContext('2d');
      c.imageSmoothingEnabled = false;
      c.drawImage(decoSprite(sampleFor(it.id)), sx, sy, sw, sh, Math.round((44 - sw * k) / 2), Math.round((44 - sh * k) / 2), Math.round(sw * k), Math.round(sh * k));
    });
  }

  function paintColors() {
    const row = (key, names, colors, value) => html`<span>${key === 'desk' ? 'Desk' : 'Chair'}</span>${colors.map((col, i) => html`<button type="button" data-${key}="${i}" class="dk-sw${value === i ? ' on' : ''}${col ? '' : ' std'}" style="background:${Array.isArray(col) ? col[0] : col || '#e2e6ed'}" data-tip="${names[i]}" aria-label="${names[i]}"></button>`)}`;
    setHtml(el.querySelector('[data-dk="desk"]'), row('desk', DESK_NAMES, DESK_COLORS, cur().desk));
    setHtml(el.querySelector('[data-dk="chair"]'), row('chair', CHAIR_NAMES, CHAIR_COLORS, cur().chair));
  }

  function paint() {
    paintStage();
    paintSlots();
    paintGrid();
    paintColors();
    el.querySelector('[data-dk="undo"]').disabled = at === 0;
    el.querySelector('[data-dk="redo"]').disabled = at === history.length - 1;
    onDraft(cur());
  }

  function push(next) {
    const clean = cleanDeco(next);
    if (!clean || JSON.stringify(clean) === JSON.stringify(cur())) return;
    history.splice(at + 1);
    history.push(clean);
    if (history.length > 60) history.shift();
    at = history.length - 1;
    paint();
  }

  function close() {
    el.remove();
    onClose();
  }

  el.addEventListener('click', async (e) => {
    const b = e.target.closest('button');
    if (!b || busy) return;
    const d = b.dataset;
    if (d.slot !== undefined) {
      slot = +d.slot;
      paintSlots();
      return paintGrid();
    }
    if (d.item !== undefined) return push({ ...cur(), slots: cur().slots.map((s, i) => (i === slot ? +d.item : s)) });
    if (d.desk !== undefined) return push({ ...cur(), desk: +d.desk });
    if (d.chair !== undefined) return push({ ...cur(), chair: +d.chair });
    if (d.dk === 'undo' && at > 0) { at--; return paint(); }
    if (d.dk === 'redo' && at < history.length - 1) { at++; return paint(); }
    if (d.dk === 'close') return close();
    if (d.dk !== 'save') return;
    busy = true;
    b.disabled = true;
    api.setNote(el.querySelector('.dk-note'), null);
    const error = await onSave(cur());
    busy = false;
    b.disabled = false;
    if (!error) return close();
    api.setNote(el.querySelector('.dk-note'), { title: 'Could not save your desk', body: error, tone: 'error' });
  });
  el.querySelector('[data-dk="q"]').oninput = (e) => {
    query = e.target.value;
    paintGrid();
  };
  el.querySelector('[data-dk="q"]').onkeydown = (e) => { if (e.key === 'Escape') e.target.blur(); };

  paint();
  return { el, close };
}
