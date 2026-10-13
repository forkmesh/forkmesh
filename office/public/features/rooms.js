import { html, setHtml } from '../html.js';
import { focusRoom, roomRect, fade, ease, occupants, linkedRoom, roomLink, DIM, TAG_DIM, ARM_MS } from './rooms/logic.js';

const CARD_EVERY_MS = 250;

export default function setup(api) {
  const { $, icon, tint, initial, ZONES, ROOM_ANCHORS, zoneAt } = api;

  let zone = null, amount = 0, lastDraw = 0, lastCard = 0, sig = '', hidden = null;
  let entered = null, enteredAt = 0;

  const card = document.createElement('div');
  card.className = 'room-card';
  card.hidden = true;
  card.setAttribute('role', 'region');
  card.setAttribute('aria-label', 'Room');
  const cardBody = document.createElement('div');
  cardBody.className = 'room-body';
  const cardNote = document.createElement('p');
  cardNote.hidden = true;
  card.append(cardBody, cardNote);
  $('#stage')?.append(card);

  const here = () => (api.me ? focusRoom(zoneAt(api.me.x, api.me.y)) : null);

  function settled(now) {
    const z = here();
    if (z !== entered) {
      entered = z;
      enteredAt = now;
    }
    return z && now - enteredAt >= ARM_MS ? z : null;
  }

  function renderCard(now) {
    const me = api.me;
    const z = settled(now);
    if (!z) hidden = null;
    if (!z || hidden === z) {
      card.hidden = true;
      if (sig) api.setNote(cardNote, null);
      sig = '';
      return;
    }
    const people = occupants(me, api.others.values(), z, zoneAt);
    const next = z + '|' + people.map((o) => `${o.id}:${o.name}:${o.av}:${o.away ? 1 : 0}`).join(',');
    if (next === sig) return;
    if (!sig.startsWith(z + '|')) api.setNote(cardNote, null);
    sig = next;
    setHtml(cardBody, html`<div class="room-head"><span class="room-ic">${icon('leave', 18)}</span><span class="room-name"><b>${ZONES[z].name}</b><small>Private room</small></span><button class="icon-btn" data-room="hide" title="Hide" aria-label="Hide room details">${icon('x', 14)}</button></div>
      <div class="room-acts"><button class="btn btn-sm" data-room="link">${icon('link', 14)}Room link</button><button class="btn btn-sm room-sq" data-room="seat" title="Take a seat" aria-label="Take a seat">${icon('seat', 14)}</button></div>
      <div class="room-count">${icon('users', 14)}${people.length === 1 ? '1 participant' : `${people.length} participants`}</div>
      <ul class="room-people">${people.map((o) => html`<li><span class="av" style="background:${tint(o.av)}">${initial(o)}<b class="dot${o.away ? ' away' : ''}"></b></span><span class="room-who">${o.name}${o === me ? html` <span class="muted">(you)</span>` : ''}</span></li>`)}</ul>`);
    card.hidden = false;
  }

  card.addEventListener('click', (e) => {
    const act = e.target.closest('[data-room]')?.dataset.room;
    const z = here();
    if (!act || !z) return;
    if (act === 'hide') {
      hidden = z;
      renderCard(performance.now());
    } else if (act === 'seat') api.takeSeat();
    else if (act === 'link') {
      const link = roomLink(location.href, z);
      navigator.clipboard?.writeText(link).catch(() => {});
      api.setNote(cardNote, { title: 'Room link copied', body: `Teammates who open it walk to ${ZONES[z].name}.`, tone: 'ok' });
    }
  });

  api.onInit((m, again) => {
    sig = '';
    if (again) return;
    const z = linkedRoom(location.search);
    if (!z) return;
    const u = new URL(location.href);
    u.searchParams.delete('room');
    history.replaceState(null, '', u);
    api.walkToTile(ROOM_ANCHORS[z].x, ROOM_ANCHORS[z].y);
  });

  api.onTick((now) => {
    if (!api.me || now - lastCard < CARD_EVERY_MS) return;
    lastCard = now;
    renderCard(now);
  });

  api.onDrawWorld((ctx, now) => {
    const z = settled(now);
    if (z) zone = z;
    amount = fade(amount, !!z, lastDraw ? now - lastDraw : 0);
    lastDraw = now;
    if (!zone || amount <= 0) return;
    const k = ease(amount);
    const r = roomRect(zone);
    ctx.save();
    ctx.beginPath();
    ctx.rect(-1e5, -1e5, 2e5, 2e5);
    ctx.roundRect(r.x, r.y, r.w, r.h, 6);
    ctx.fillStyle = `rgba(13,15,28,${DIM * k})`;
    ctx.fill('evenodd');
    ctx.beginPath();
    ctx.roundRect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 7);
    ctx.strokeStyle = `rgba(255,255,255,${0.8 * k})`;
    ctx.lineWidth = 2;
    ctx.stroke();
    ctx.restore();
  });

  api.tagAlpha((o) => (amount > 0 && zone && zoneAt(o.x, o.y) !== zone ? 1 - TAG_DIM * ease(amount) : undefined));
}
