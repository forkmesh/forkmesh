import { html, setHtml } from '../html.js';
import { avatarSprite, SPRITE_W, SPRITE_H } from '../sprite.js';
import { overlay } from './play/ui.js';
import { openAvatarEditor } from './profile/editor.js';
import { STATUS_COLOR, effective, statusLine } from './presence/logic.js';
import { NAME_MAX, PIC_SIZES, PIC_MAX_BYTES, cleanName, cleanTz, picUrl, localTime, browserTz, tzList, fitSquare, b64Bytes, handleOf } from './profile/logic.js';

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const IC = {
  user: html`<circle cx="12" cy="8" r="4"/><path d="M20 21a8 8 0 0 0-16 0"/>`,
  shirt: html`<path d="M20.38 3.46 16 2a4 4 0 0 1-8 0L3.62 3.46a2 2 0 0 0-1.34 2.23l.58 3.47a1 1 0 0 0 .99.84H6v10c0 1.1.9 2 2 2h8a2 2 0 0 0 2-2V10h2.15a1 1 0 0 0 .99-.84l.58-3.47a2 2 0 0 0-1.34-2.23z"/>`,
  pencil: html`<path d="M21.17 6.81a1 1 0 0 0-3.98-3.98L3.84 16.17a2 2 0 0 0-.5.83l-1.32 4.35a.5.5 0 0 0 .62.62l4.35-1.32a2 2 0 0 0 .83-.5z"/><path d="m15 5 4 4"/>`,
  clock: html`<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>`,
  x: html`<path d="M18 6 6 18"/><path d="m6 6 12 12"/>`,
  chat: html`<path d="M7.9 20A9 9 0 1 0 4 16.1L2 22Z"/>`,
};
const SAVE_WAIT_MS = 5000;
const FILE_MAX = 12 * 1024 * 1024;

export default function setup(api) {
  const { $, tint, initial } = api;
  let card = null;
  let localPic;
  let modal = null;
  let editor = null;
  let pending = null;
  let lastMinute = '';

  const person = (id) => (id === api.me?.id ? api.me : api.others.get(id));
  const picOf = (o) => (o === api.me && localPic !== undefined ? localPic : picUrl(handleOf(o), o?.ex?.pic));
  const face = (o) => {
    const u = picOf(o);
    return u ? html`<img class="pf-img" src="${u}" alt="">` : '';
  };
  api.face(face);

  function paintDock() {
    const b = $('#b-me');
    if (!b || !api.me) return;
    b.querySelector('.pf-img')?.remove();
    const u = picOf(api.me);
    if (!u) return;
    const img = document.createElement('img');
    img.className = 'pf-img';
    img.alt = '';
    img.src = u;
    b.prepend(img);
  }

  const cardEl = document.createElement('div');
  cardEl.className = 'pf-card';
  cardEl.hidden = true;
  cardEl.setAttribute('role', 'dialog');
  ($('#stage') || document.body).append(cardEl);

  function openCard(id, note) {
    card = { id, note: note || null };
    renderCard();
  }

  function closeCard() {
    card = null;
    cardEl.hidden = true;
  }

  function renderCard() {
    if (!card) return;
    const o = person(card.id);
    if (!o) return closeCard();
    const mine = o === api.me;
    const handle = handleOf(o);
    const time = localTime(o.ex?.tz);
    const teams = Array.isArray(o.ex?.teams) ? o.ex.teams.slice(0, 3) : [];
    const extra = mine ? [] : api.personActs(o);
    const acts = mine
      ? html`<button type="button" class="btn btn-primary" data-pf="edit">${svg(IC.user, 14)}Edit profile</button><button type="button" class="icon-btn" data-pf="look" data-tip="Edit avatar">${svg(IC.shirt, 15)}</button>`
      : html`<button type="button" class="btn btn-primary" data-pf="dm">${svg(IC.chat, 14)}Message</button><button type="button" class="btn" data-pf="walk">Walk over</button><button type="button" class="btn" data-pf="wave">Wave</button>${
        extra.map((a, i) => html`<button type="button" class="icon-btn" data-pf="x${i}" data-tip="${a.tip}">${a.icon}</button>`)}`;
    setHtml(cardEl, html`<div class="pf-top">
        <span class="pf-pic av" style="background:${tint(o.av)}">${initial(o)}${face(o)}<b class="dot" style="background:${STATUS_COLOR[effective(o)]}"></b></span>
        <span class="pf-who"><b>${o.name}</b>${handle ? html`<small>@${handle}</small>` : ''}${time ? html`<small class="pf-time">${svg(IC.clock, 12)}${time} local time</small>` : ''}</span>
        <button type="button" class="icon-btn" data-pf="close" aria-label="Close">${svg(IC.x, 14)}</button>
      </div>
      <div class="pf-meta"><span>${statusLine(o)}</span><span>${api.zoneName(o)}</span>${teams.map((t) => html`<span class="pf-team">${t}</span>`)}</div>
      <div class="pf-acts">${acts}</div><p class="pf-note" hidden></p>`);
    api.setNote(cardEl.querySelector('.pf-note'), card.note);
    cardEl.hidden = false;
    lastMinute = time;
  }

  cardEl.addEventListener('click', (e) => {
    const b = e.target.closest('[data-pf]');
    if (!b || !card) return;
    const act = b.dataset.pf;
    const id = card.id;
    if (act === 'close') return closeCard();
    if (act === 'edit') return openEdit();
    if (act === 'look') return openLook();
    const o = person(id);
    if (!o) return closeCard();
    if (act === 'dm') {
      document.dispatchEvent(new CustomEvent('office:dm', { detail: { id } }));
      return closeCard();
    }
    if (act === 'walk') return api.walkTo(id);
    if (act === 'wave') {
      api.send({ t: 'wave', to: id });
      card.note = { title: `You waved at ${o.name} 👋`, tone: 'ok' };
      return renderCard();
    }
    if (act[0] === 'x') api.personActs(o)[+act.slice(1)]?.run();
  });

  api.onPerson((o) => openCard(o.id));
  $('#people')?.addEventListener('click', (e) => {
    if (e.target.closest('button')) return;
    const id = e.target.closest('li')?.dataset.id;
    if (id) openCard(id);
  });
  addEventListener('keydown', (e) => { if (e.key === 'Escape' && !modal && !editor) closeCard(); });

  let lastCheck = 0;
  api.onTick((now) => {
    if (!card || now - lastCheck < 1000) return;
    lastCheck = now;
    const o = person(card.id);
    if (!o) return closeCard();
    if (localTime(o.ex?.tz) !== lastMinute) renderCard();
  });

  api.onExtra((o, k) => {
    if (o === api.me && k === 'pic') {
      localPic = undefined;
      paintDock();
    }
    if (card && o.id === card.id) renderCard();
  });

  api.onInit(() => {
    paintDock();
    if (card) renderCard();
  });

  api.onMessage('prof', (m) => {
    const o = person(m.id);
    if (!o) return;
    if (typeof m.name === 'string') o.name = m.name;
    if (Number.isInteger(m.av)) o.av = m.av;
    api.refreshPeople();
    if (o === api.me) {
      api.renderMe();
      paintDock();
      modal?.paintSprite?.();
    }
    if (card?.id === m.id) renderCard();
  });

  const ERRORS = {
    taken: 'Someone in this office already uses that name.',
    avatar: 'That avatar could not be saved. Pick the options again.',
  };
  function settle(error) {
    if (!pending) return;
    clearTimeout(pending.timer);
    const done = pending;
    pending = null;
    done.finish(error);
  }
  function request(payload) {
    settle('Another change replaced this one.');
    return new Promise((resolve) => {
      pending = {
        timer: setTimeout(() => settle('The office did not answer. Check your connection and try again.'), SAVE_WAIT_MS),
        finish: resolve,
      };
      api.send({ t: 'prof-set', ...payload });
    });
  }
  api.onMessage('prof-ok', () => settle(''));
  api.onMessage('prof-err', (m) => settle(ERRORS[m.reason] || 'Could not save your profile. Try again.'));

  function openLook() {
    const me = api.me;
    if (editor || !me) return;
    editor = openAvatarEditor(api, {
      av: me.av,
      name: me.name,
      save: async (av) => {
        const error = await request({ av });
        if (!error && api.me) openCard(api.me.id, { title: 'Avatar saved', tone: 'ok' });
        return error;
      },
    });
    const close = editor.close;
    editor.close = () => {
      close();
      editor = null;
    };
  }

  async function toPng(file) {
    const bmp = await createImageBitmap(file);
    const { sx, sy, side } = fitSquare(bmp.width, bmp.height);
    for (const size of PIC_SIZES) {
      const c = document.createElement('canvas');
      c.width = c.height = size;
      c.getContext('2d').drawImage(bmp, sx, sy, side, side, 0, 0, size, size);
      const url = c.toDataURL('image/png');
      if (b64Bytes(url) <= PIC_MAX_BYTES) return url;
    }
    throw new Error('too large');
  }

  function openEdit() {
    const me = api.me;
    if (modal || !me) return;
    const current = cleanTz(me.ex?.tz) || browserTz() || 'UTC';
    const zones = tzList([current]);
    let pic;
    modal = overlay(api, {
      title: 'Edit profile',
      body: html`<form class="pf-form" novalidate>
        <div class="pf-slots">
          <div class="pf-slot"><small>Profile picture</small>
            <span class="pf-big av" data-pf="picbox"></span>
            <button type="button" class="pf-pencil" data-pf="pick" aria-label="Change picture">${svg(IC.pencil, 13)}</button>
            <input type="file" accept="image/png,image/jpeg,image/webp" hidden>
          </div>
          <div class="pf-slot"><small>Avatar</small>
            <span class="pf-big pf-stage"><canvas width="${SPRITE_W * 2}" height="${SPRITE_H * 2}"></canvas></span>
            <button type="button" class="pf-pencil" data-pf="look" aria-label="Edit avatar">${svg(IC.pencil, 13)}</button>
          </div>
        </div>
        <button type="button" class="pf-link" data-pf="unpic" hidden>Remove picture</button>
        <label class="pf-field"><span>Full name<i>*</i></span><input class="input" name="name" maxlength="${NAME_MAX}" autocomplete="off" required value="${me.name}"></label>
        <label class="pf-field"><span>Time zone</span><select class="input" name="tz">${zones.map((z) => html`<option value="${z.tz}"${z.tz === current ? html` selected` : ''}>${z.label}</option>`)}</select></label>
        <p class="pf-err" hidden></p>
        <div class="pf-foot"><span class="muted">Teammates always see @${handleOf(me) || api.account?.name || ''} next to your name.</span><button type="submit" class="btn btn-primary">Save</button></div>
      </form>`,
      onClose: () => {
        modal = null;
      },
    });
    const form = modal.q('.pf-form');
    const file = form.querySelector('input[type="file"]');
    const err = form.querySelector('.pf-err');
    const save = form.querySelector('[type="submit"]');
    const showErr = (text) => {
      err.textContent = text || '';
      err.hidden = !text;
    };

    function paintPic() {
      const box = form.querySelector('[data-pf="picbox"]');
      const u = pic !== undefined ? pic : picOf(me);
      box.style.background = tint(me.av);
      setHtml(box, html`${initial(me)}${u ? html`<img class="pf-img" src="${u}" alt="">` : ''}`);
      form.querySelector('[data-pf="unpic"]').hidden = !u;
    }
    function paintSprite() {
      const cv = form.querySelector('.pf-stage canvas');
      const c = cv.getContext('2d');
      c.imageSmoothingEnabled = false;
      c.clearRect(0, 0, cv.width, cv.height);
      c.drawImage(avatarSprite(api.me.av, 0, 0, false), 0, 0, cv.width, cv.height);
    }
    modal.paintSprite = paintSprite;
    paintPic();
    paintSprite();

    form.addEventListener('click', (e) => {
      const b = e.target.closest('[data-pf]');
      if (!b) return;
      if (b.dataset.pf === 'pick') file.click();
      if (b.dataset.pf === 'look') openLook();
      if (b.dataset.pf === 'unpic') {
        pic = '';
        paintPic();
      }
    });
    file.onchange = async () => {
      const f = file.files?.[0];
      file.value = '';
      if (!f) return;
      showErr('');
      if (f.size > FILE_MAX) return showErr('That picture is too large. Pick one under 12 MB.');
      try {
        pic = await toPng(f);
        paintPic();
      } catch {
        showErr('That file could not be read as a picture.');
      }
    };

    form.onsubmit = async (e) => {
      e.preventDefault();
      if (save.disabled) return;
      const name = cleanName(form.elements.name.value);
      if (!name) return showErr('Enter the name teammates should see.');
      showErr('');
      save.disabled = true;
      if (pic !== undefined) {
        try {
          const r = await fetch('/api/accounts/profile', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'content-type': 'application/json' },
            body: JSON.stringify({ avatarPng: pic }),
          });
          if (!r.ok) throw new Error(r.status === 413 ? 'That picture is too large.' : 'Could not save the picture. Try again.');
          localPic = pic;
          pic = undefined;
          paintDock();
          api.refreshPeople();
        } catch (error) {
          save.disabled = false;
          return showErr(error.message || 'Could not save the picture. Try again.');
        }
      }
      const error = await request({ name, tz: form.elements.tz.value });
      if (!modal) return;
      if (error) {
        save.disabled = false;
        return showErr(error);
      }
      modal.close();
      if (api.me) openCard(api.me.id, { title: 'Profile saved', tone: 'ok' });
    };
  }

  api.openProfile = (id) => openCard(id || api.me?.id);
  api.editProfile = openEdit;
  api.editAvatar = openLook;

  api.meMenu(() => [
    { icon: svg(IC.user, 14), label: 'View profile', run: () => openCard(api.me.id) },
    { icon: svg(IC.pencil, 14), label: 'Edit profile', run: openEdit },
    { icon: svg(IC.shirt, 14), label: 'Edit avatar', run: openLook },
  ]);
  api.paletteItems(() => [
    { ic: svg(IC.pencil, 14), label: 'Edit profile', sub: 'Name, time zone, picture', run: openEdit },
    { ic: svg(IC.shirt, 14), label: 'Edit avatar', sub: 'Hair, clothes, extras', run: openLook },
  ]);
}
