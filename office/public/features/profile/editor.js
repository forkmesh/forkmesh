import { html, setHtml } from '../../html.js';
import { overlay } from '../play/ui.js';
import { avatarSprite, SPRITE_W, SPRITE_H } from '../../sprite.js';
import { lookOf, packAvatar, randomLook, SKIN, HAIR_COLOR, TOP, BOTTOM, SHOES, HAIR, FACE, EXTRA } from '../../avatar.js';

const CATS = [
  { id: 'skin', label: 'Skin', colors: ['skin', SKIN] },
  { id: 'hair', label: 'Hair', tiles: ['hair', HAIR], colors: ['hairColor', HAIR_COLOR] },
  { id: 'face', label: 'Facial hair', tiles: ['face', FACE], colors: ['hairColor', HAIR_COLOR] },
  { id: 'top', label: 'Top', colors: ['top', TOP] },
  { id: 'bottom', label: 'Bottom', colors: ['bottom', BOTTOM] },
  { id: 'shoes', label: 'Shoes', colors: ['shoes', SHOES] },
  { id: 'extra', label: 'Extras', tiles: ['extra', EXTRA] },
];
const WALK = [1, 0, 2, 0];
const TURN = [0, 3, 1, 2];
const SCALE = 4;
const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const DICE = html`<rect width="18" height="18" x="3" y="3" rx="3"/><path d="M8 8h.01"/><path d="M16 8h.01"/><path d="M12 12h.01"/><path d="M8 16h.01"/><path d="M16 16h.01"/>`;
const TURN_IC = html`<path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/>`;

export function openAvatarEditor(api, { av, name, save }) {
  let look = { ...lookOf(av) };
  let cat = CATS[1];
  let facing = 0;
  let frame = 0;
  let busy = false;
  let timer = null;

  const ov = overlay(api, {
    title: 'Edit avatar',
    body: html`<div class="av-ed">
        <nav class="av-cats"></nav>
        <div class="av-opts"></div>
        <div class="av-prev">
          <span class="av-name">${name}</span>
          <canvas width="${SPRITE_W * SCALE}" height="${SPRITE_H * SCALE}"></canvas>
          <div class="av-tools">
            <button type="button" class="icon-btn" data-ed="turn" data-tip="Turn around" aria-label="Turn around">${svg(TURN_IC)}</button>
            <button type="button" class="icon-btn" data-ed="dice" data-tip="Surprise me" aria-label="Surprise me">${svg(DICE)}</button>
          </div>
        </div>
      </div>
      <p class="pf-err" hidden></p>
      <div class="av-foot"><button type="button" class="btn" data-ed="cancel">Cancel</button><button type="button" class="btn btn-primary" data-ed="done">Done</button></div>`,
    onClose: () => clearInterval(timer),
  });
  ov.el.classList.add('av-ov');
  const err = ov.q('.pf-err');

  function draw(cv, l, dir, step, crop) {
    const c = cv.getContext('2d');
    c.imageSmoothingEnabled = false;
    c.clearRect(0, 0, cv.width, cv.height);
    const spr = avatarSprite(packAvatar(l), dir, step, false);
    if (crop) c.drawImage(spr, 0, 0, SPRITE_W, SPRITE_W, 0, 0, cv.width, cv.height);
    else c.drawImage(spr, 0, 0, cv.width, cv.height);
  }

  function paintCats() {
    setHtml(ov.q('.av-cats'), html`${CATS.map((c) => html`<button type="button" data-cat="${c.id}" class="${c === cat ? 'on' : ''}">${c.label}</button>`)}`);
  }

  function paintOpts() {
    const box = ov.q('.av-opts');
    const tiles = cat.tiles
      ? html`<div class="av-grid">${cat.tiles[1].map((label, i) => html`<button type="button" class="av-tile${look[cat.tiles[0]] === i ? ' on' : ''}" data-k="${cat.tiles[0]}" data-v="${i}" data-tip="${label}" aria-label="${label}"><canvas width="64" height="64"></canvas></button>`)}</div>`
      : '';
    const colors = cat.colors
      ? html`<div class="av-colors">${cat.colors[1].map((col, i) => html`<button type="button" class="av-sw${look[cat.colors[0]] === i ? ' on' : ''}" data-k="${cat.colors[0]}" data-v="${i}" style="background:${col}" aria-label="Colour ${i + 1}"></button>`)}</div>`
      : '';
    setHtml(box, html`${tiles}${colors}`);
    if (cat.tiles) box.querySelectorAll('.av-tile canvas').forEach((cv, i) => draw(cv, { ...look, [cat.tiles[0]]: i }, 0, 0, true));
  }

  const paintPreview = () => draw(ov.q('.av-prev canvas'), look, TURN[facing], WALK[frame], false);

  ov.el.addEventListener('click', async (e) => {
    const b = e.target.closest('button');
    if (!b || busy) return;
    if (b.dataset.cat) {
      cat = CATS.find((c) => c.id === b.dataset.cat) || cat;
      paintCats();
      return paintOpts();
    }
    if (b.dataset.k) {
      look = { ...look, [b.dataset.k]: +b.dataset.v };
      paintOpts();
      return paintPreview();
    }
    const act = b.dataset.ed;
    if (act === 'turn') {
      facing = (facing + 1) % TURN.length;
      return paintPreview();
    }
    if (act === 'dice') {
      look = randomLook();
      paintOpts();
      return paintPreview();
    }
    if (act === 'cancel') return ov.close();
    if (act !== 'done') return;
    const next = packAvatar(look);
    if (next === av) return ov.close();
    busy = true;
    b.disabled = true;
    err.hidden = true;
    const error = await save(next);
    busy = false;
    b.disabled = false;
    if (!error) return ov.close();
    err.textContent = error;
    err.hidden = false;
  });

  paintCats();
  paintOpts();
  paintPreview();
  timer = setInterval(() => {
    frame = (frame + 1) % WALK.length;
    paintPreview();
  }, 170);
  return ov;
}
