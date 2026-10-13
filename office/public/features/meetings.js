import { html, setHtml, trusted } from '../html.js';
import { TABLES, TABLE_BY_ID, tableAtSeat, volumeFor, smooth, fullVolume, meetingVerdict, meetingSince, fmtDuration, SEAT_SETTLE_MS } from './meetings/tables.js';

const PATHS = {
  users: html`<path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/><path d="M22 21v-2a4 4 0 0 0-3-3.87"/><path d="M16 3.13a4 4 0 0 1 0 7.75"/>`,
  share: html`<path d="m9 10 3-3 3 3"/><path d="M12 13V7"/><rect width="20" height="14" x="2" y="3" rx="2"/><path d="M12 17v4"/><path d="M8 21h8"/>`,
  unshare: html`<path d="m14.5 12.5-5-5"/><path d="m9.5 12.5 5-5"/><rect width="20" height="14" x="2" y="3" rx="2"/><path d="M12 17v4"/><path d="M8 21h8"/>`,
  monitor: html`<rect width="20" height="14" x="2" y="3" rx="2"/><path d="M8 21h8"/><path d="M12 17v4"/>`,
  stand: html`<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5"/><path d="M21 12H9"/>`,
  knock: html`<path d="M18 11V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2"/><path d="M14 10V4a2 2 0 0 0-2-2a2 2 0 0 0-2 2v2"/><path d="M10 10.5V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2v8"/><path d="M18 8a2 2 0 1 1 4 0v6a8 8 0 0 1-8 8h-2c-2.8 0-4.5-.86-5.99-2.34l-3.6-3.6a2 2 0 0 1 2.83-2.82L7 15"/>`,
  door: html`<path d="M13 4h3a2 2 0 0 1 2 2v14"/><path d="M2 20h3"/><path d="M13 20h9"/><path d="M10 12v.01"/><path d="M13 4.56v16.16a1 1 0 0 1-1.24.97L5 20V5.56a2 2 0 0 1 1.52-1.94l4-1A2 2 0 0 1 13 4.56Z"/>`,
  clock: html`<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>`,
  table: html`<rect width="18" height="8" x="3" y="8" rx="2"/><path d="M7 16v4"/><path d="M17 16v4"/><path d="M7 4v4"/><path d="M17 4v4"/>`,
};
const svg = (name, size = 16) =>
  html`<svg class="i mt-i" width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${PATHS[name] || ''}</svg>`;

const VOLUME_EVERY_MS = 80;
const SNAP_WINDOW_MS = 3000;
const OFFER_AGAIN_MS = 8000;
const RESEND_MS = 2000;
const KNOCK_PROMPT_MS = 30000;

export default function setup(api) {
  const { $, esc, tint, initial, ZONES, zoneAt, T } = api;

  let seatId = null, seatSince = 0, sentWant, sentAt = 0;
  let share = null;
  const screens = new Map();
  const screenTiles = new Map();
  let pinned = null, pinnedAuto = false;
  const dismissed = new Set();
  const vols = new Map();
  let lastVolAt = 0, lastSlow = 0, barSig = '';
  let attempt = null, lastZone = null;
  const offered = new Map();
  const admittedHere = new Set();
  const prompts = new Set();

  const bar = document.createElement('div');
  bar.id = 'mt-bar';
  bar.className = 'mt-bar';
  bar.hidden = true;
  bar.setAttribute('role', 'region');
  bar.setAttribute('aria-label', 'Meeting');
  $('#stage')?.append(bar);

  const canShare = !!navigator.mediaDevices?.getDisplayMedia;
  const dockBtn = canShare ? api.addDockButton({ id: 'b-share', iconHtml: svg('share', 18), tip: 'Share screen', onClick: () => toggleShare() }) : null;

  function say({ key = 'meet', title, body, tone, actions = [], ms }) {
    return api.hudNote({ key, title, body, tone, ms, actions: actions.map((a) => ({ label: a.label, primary: a.primary, run: a.run, iconHtml: a.icon ? svg(a.icon, 14) : '' })) });
  }

  const zoneLabel = (z) => ZONES[z]?.name || 'that room';

  function standUp() {
    const me = api.me;
    if (!me) return;
    const taken = new Set([...api.others.values()].map((o) => `${o.x},${o.y}`));
    for (let r = 1; r <= 3; r++) {
      for (let dy = -r; dy <= r; dy++) {
        for (let dx = -r; dx <= r; dx++) {
          if (Math.max(Math.abs(dx), Math.abs(dy)) !== r) continue;
          const x = me.x + dx, y = me.y + dy;
          if (api.isWalkable(x, y) && api.chairDir(x, y) === undefined && !taken.has(`${x},${y}`)) return api.walkToTile(x, y);
        }
      }
    }
  }

  function syncMeeting(now) {
    const me = api.me;
    const seat = tableAtSeat(me.x, me.y);
    if (seat !== seatId) { seatId = seat; seatSince = now; }
    const current = me.ex?.meeting?.table || null;
    let want;
    if (!seat) want = null;
    else if (now - seatSince >= SEAT_SETTLE_MS) want = seat;
    else want = current === seat ? seat : null;
    if (want === current) { sentWant = want; return; }
    if (want === sentWant && now - sentAt < RESEND_MS) return;
    sentWant = want;
    sentAt = now;
    api.send({ t: 'ex', k: 'meeting', v: want ? { table: want } : null });
  }

  function members(t) {
    const me = api.me;
    return [me, ...[...api.others.values()].filter((o) => o.ex?.meeting?.table === t).sort((a, b) => a.name.localeCompare(b.name))];
  }

  function renderBar() {
    const me = api.me;
    const t = me?.ex?.meeting?.table;
    const table = t && TABLE_BY_ID.get(t);
    if (!table) {
      bar.hidden = true;
      barSig = '';
      return;
    }
    const people = members(t);
    const sig = t + '|' + (share ? 1 : 0) + '|' + people.map((o) => `${o.id}:${o.ex?.sharing ? 1 : 0}`).join(',');
    if (sig !== barSig) {
      barSig = sig;
      const shown = people.slice(0, 6);
      setHtml(bar, html`<div class="mt-head">${svg('table', 15)}<b>${(table.name)}</b><span class="mt-time">${svg('clock', 12)}<span>0:00</span></span></div>
        <div class="mt-people">${shown.map((o) => html`<span class="mt-av" style="background:${tint(o.av)}" title="${(o === me ? 'You' : o.name)}">${(initial(o))}${o.ex?.sharing ? html`<i>${svg('monitor', 9)}</i>` : ''}</span>`)}${people.length > shown.length ? html`<span class="mt-more">+${people.length - shown.length}</span>` : ''}<span class="mt-count">${people.length === 1 ? 'Just you' : `${people.length} here`}</span></div>
        <div class="mt-acts">${canShare ? html`<button class="btn btn-sm${share ? ' mt-on' : ''}" data-mt="share">${svg(share ? 'unshare' : 'share', 14)}${share ? 'Stop sharing' : 'Share'}</button>` : ''}<button class="btn btn-sm" data-mt="leave">${svg('stand', 14)}Leave</button></div>`);
      bar.hidden = false;
    }
    const since = meetingSince(people, t);
    const el = bar.querySelector('.mt-time span');
    if (el) el.textContent = since ? fmtDuration(Date.now() - since) : '0:00';
  }
  bar.addEventListener('click', (e) => {
    const b = e.target.closest('[data-mt]');
    if (!b) return;
    if (b.dataset.mt === 'share') toggleShare();
    else if (b.dataset.mt === 'leave') standUp();
  });

  function updateShareUi() {
    if (dockBtn) {
      setHtml(dockBtn, svg(share ? 'unshare' : 'share', 18));
      dockBtn.dataset.tip = share ? 'Stop sharing' : 'Share screen';
      dockBtn.classList.toggle('mt-on', !!share);
    }
    barSig = '';
    if (api.me) renderBar();
  }

  async function startShare() {
    if (!canShare) return say({ title: "Screen sharing isn't available here", body: 'Try a desktop browser.', tone: 'error' });
    let stream;
    try {
      stream = await navigator.mediaDevices.getDisplayMedia({ video: { frameRate: { ideal: 15, max: 30 } }, audio: false });
    } catch (err) {
      if (err?.name !== 'NotAllowedError' && err?.name !== 'AbortError') say({ title: "Couldn't share your screen", body: 'Check your browser permissions.', tone: 'error' });
      return;
    }
    const track = stream.getVideoTracks()[0];
    if (!track) return;
    try { track.contentHint = 'detail'; } catch {}
    if (share) stopShare();
    share = { stream, track };
    track.addEventListener('ended', () => { if (share?.track === track) stopShare(); });
    await api.mesh?.setTrack('screen', track);
    api.send({ t: 'ex', k: 'sharing', v: true });
    ensureTile('me', stream, 'Your screen');
    updateShareUi();
    if (!api.mesh?.peers.size) say({ title: "You're sharing your screen", body: 'People you are connected to will see it.' });
  }

  function stopShare() {
    if (!share) return;
    const s = share;
    share = null;
    for (const t of s.stream.getTracks()) t.stop();
    api.mesh?.setTrack('screen', null);
    api.send({ t: 'ex', k: 'sharing', v: null });
    dropTile('me');
    updateShareUi();
  }

  const toggleShare = () => (share ? stopShare() : startShare());

  function videosEl() {
    return $('#videos');
  }

  function ensureTile(id, stream, label) {
    let t = screenTiles.get(id);
    if (!t) {
      const el = document.createElement('div');
      el.className = 'tile cam mt-screen';
      el.dataset.mtKey = 'screen:' + id;
      setHtml(el, html`<video autoplay playsinline muted></video><label>${svg('monitor', 12)}<span></span></label>`);
      const video = el.querySelector('video');
      video.muted = true;
      t = { el, video };
      screenTiles.set(id, t);
      videosEl()?.prepend(el);
      if (id !== 'me' && !pinned && !dismissed.has(el.dataset.mtKey)) pin(el, true);
    }
    if (t.video.srcObject !== stream) t.video.srcObject = stream;
    t.video.play().catch(() => {});
    t.el.querySelector('label span').textContent = label;
    return t;
  }

  function dropTile(id) {
    const t = screenTiles.get(id);
    if (!t) return;
    screenTiles.delete(id);
    dismissed.delete(t.el.dataset.mtKey);
    if (pinned === t.el) unpin(false);
    t.video.srcObject = null;
    t.el.remove();
  }

  function syncScreens() {
    const mesh = api.mesh;
    for (const id of [...screenTiles.keys()]) {
      if (id === 'me') continue;
      const o = api.others.get(id);
      if (!o || !o.ex?.sharing || !screens.has(id) || !mesh?.has(id)) dropTile(id);
    }
    for (const [id, stream] of screens) {
      const o = api.others.get(id);
      if (o?.ex?.sharing && mesh?.has(id)) ensureTile(id, stream, `${o.name}'s screen`);
    }
    if (share && !screenTiles.has('me')) ensureTile('me', share.stream, 'Your screen');
  }

  function pin(el, auto) {
    if (pinned && pinned !== el) pinned.classList.remove('mt-pinned');
    pinned = el;
    pinnedAuto = auto;
    el.classList.add('mt-pinned');
    videosEl()?.classList.add('mt-spot');
    el.querySelector('video')?.play().catch(() => {});
  }

  function unpin(byUser) {
    if (!pinned) return;
    if (byUser && pinned.dataset.mtKey) dismissed.add(pinned.dataset.mtKey);
    pinned.classList.remove('mt-pinned');
    pinned = null;
    pinnedAuto = false;
    videosEl()?.classList.remove('mt-spot');
  }

  videosEl()?.addEventListener('click', (e) => {
    const el = e.target.closest('.tile');
    if (!el) return;
    if (!el.dataset.mtKey) el.dataset.mtKey = 'tile:' + ([...api.tiles].find(([, t]) => t.el === el)?.[0] || Math.random().toString(36).slice(2));
    if (pinned === el) unpin(true);
    else pin(el, false);
  });
  addEventListener('keydown', (e) => {
    if (e.key !== 'Escape' || !pinned) return;
    if (e.target?.tagName === 'INPUT' || e.target?.tagName === 'TEXTAREA') return;
    unpin(true);
  });

  function applyVolumes(now) {
    const me = api.me;
    const dt = lastVolAt ? now - lastVolAt : VOLUME_EVERY_MS;
    lastVolAt = now;
    for (const [id, t] of api.tiles) {
      if (id === 'me' || !t?.video) continue;
      const o = api.others.get(id);
      if (!o) continue;
      const target = fullVolume(me, o, zoneAt) ? 1 : volumeFor(Math.hypot(o.rx - me.rx, o.ry - me.ry));
      const v = smooth(vols.get(id), target, dt);
      vols.set(id, v);
      if (Math.abs(t.video.volume - v) > 0.004) {
        try { t.video.volume = v; } catch {}
      }
    }
    for (const id of vols.keys()) if (!api.tiles.has(id)) vols.delete(id);
  }

  function trackAttempt(now) {
    const me = api.me;
    const z = zoneAt(me.x, me.y);
    if (z === lastZone) return;
    lastZone = z;
    if (ZONES[z]?.private) attempt = { z, t: now };
  }

  function onSnap(m) {
    const a = attempt;
    if (!a || performance.now() - a.t > SNAP_WINDOW_MS) return;
    if (zoneAt(m.x, m.y) === a.z) return;
    attempt = null;
    lastZone = zoneAt(m.x, m.y);
    offerKnock(a.z);
  }

  function offerKnock(z) {
    if (!ZONES[z]?.private || admittedHere.has(z)) return;
    if ((offered.get(z) || 0) > Date.now() - OFFER_AGAIN_MS) return;
    offered.set(z, Date.now());
    say({
      key: 'knock-offer',
      title: `${zoneLabel(z)} is private`,
      body: 'Knock and someone inside can let you in.',
      actions: [{ label: 'Knock', icon: 'knock', primary: true, run: () => knock(z) }],
      ms: OFFER_AGAIN_MS,
    });
  }

  function knock(z) {
    api.send({ t: 'meet-knock', z });
  }

  api.onMessage('you', (m) => onSnap(m));

  document.addEventListener('office:locked', (e) => {
    const z = e.detail?.z;
    if (!ZONES[z]?.private || admittedHere.has(z)) return;
    e.preventDefault();
    offerKnock(z);
  });

  api.onMessage('meet-knock-r', (m) => {
    const name = zoneLabel(m.z);
    if (m.ok && m.already) {
      admittedHere.add(m.z);
      return say({ key: 'knock-offer', title: `You can already enter ${name}`, body: 'Someone let you in earlier.' });
    }
    if (m.ok) return say({ key: 'knock-offer', title: `You knocked on ${name}`, body: `Waiting for ${m.n === 1 ? 'the person' : `one of the ${m.n} people`} inside to answer.`, ms: KNOCK_PROMPT_MS });
    if (m.why === 'wait') return say({ key: 'knock-offer', title: 'You just knocked', body: 'Give them a moment before knocking again.' });
    if (m.why === 'empty') return say({ key: 'knock-offer', title: `No one is in ${name}`, body: 'Knocking needs someone inside to answer.' });
  });

  api.onMessage('meet-knock', (m) => {
    const key = `knock:${m.id}:${m.z}`;
    const el = say({
      key,
      title: `${m.name} is knocking`,
      body: `They'd like to come into ${zoneLabel(m.z)}.`,
      actions: [
        { label: 'Admit', icon: 'door', primary: true, run: () => { prompts.delete(key); api.send({ t: 'meet-admit', id: m.id, z: m.z }); } },
        { label: 'Not now', run: () => { prompts.delete(key); api.send({ t: 'meet-deny', id: m.id, z: m.z }); } },
      ],
      ms: KNOCK_PROMPT_MS,
    });
    if (el) prompts.add(key);
  });

  api.onMessage('meet-knock-end', (m) => {
    const key = `knock:${m.id}:${m.z}`;
    if (!prompts.has(key)) return;
    prompts.delete(key);
    if (m.gone) return api.hudNote({ key });
    say({ key, title: m.ok ? `${m.by} let ${m.name} in` : `${m.by} answered ${m.name}`, body: m.ok ? `${m.name} can now enter ${zoneLabel(m.z)}.` : 'They were asked to come back later.' });
  });

  api.onMessage('meet-admitted', (m) => {
    admittedHere.add(m.z);
    const a = api.ROOM_ANCHORS?.[m.z];
    say({ key: 'knock-offer', title: `${m.by} let you in`, body: `You can enter ${zoneLabel(m.z)} for the rest of this visit.`, tone: 'ok', actions: a ? [{ label: 'Walk in', primary: true, run: () => api.walkToTile(a.x, a.y) }] : [] });
  });

  api.onMessage('meet-knock-denied', (m) => {
    say({ key: 'knock-offer', title: `${zoneLabel(m.z)} is busy`, body: `${m.by} can't let you in right now.` });
  });

  api.onInit((m, reconnect) => {
    sentWant = undefined;
    seatId = null;
    lastZone = null;
    attempt = null;
    admittedHere.clear();
    for (const key of prompts) api.hudNote({ key });
    prompts.clear();
    screens.clear();
    for (const id of [...screenTiles.keys()]) if (id !== 'me') dropTile(id);
    if (reconnect && share) api.send({ t: 'ex', k: 'sharing', v: true });
    barSig = '';
  });

  api.onMesh((mesh) => {
    mesh.local.screen = share?.track || null;
    mesh.onScreen = (id, stream) => {
      screens.set(id, stream);
      syncScreens();
    };
    const closed = mesh.onClose;
    mesh.onClose = (id) => {
      closed?.(id);
      screens.delete(id);
      dropTile(id);
    };
  });

  api.onExtra((o, k) => {
    if (k === 'sharing') syncScreens();
    if (k === 'meeting' || k === 'sharing') barSig = '';
  });

  api.proximity((o) => meetingVerdict(api.me, o));

  api.personBadge((o) => (o.ex?.sharing ? html`<span class="mt-badge" title="Sharing their screen">${svg('monitor', 11)}</span>` : ''));

  api.paletteItems(() => {
    const items = [];
    if (canShare) items.push({ ic: svg(share ? 'unshare' : 'share', 14), label: share ? 'Stop sharing your screen' : 'Share your screen', run: toggleShare });
    for (const t of TABLES) {
      const n = [api.me, ...api.others.values()].filter((o) => o?.ex?.meeting?.table === t.id).length;
      items.push({
        ic: svg('table', 14),
        label: `Join ${t.name}`,
        sub: n ? `${n} in a meeting` : 'Take a seat',
        run: () => {
          const taken = new Set([...api.others.values()].map((o) => `${o.x},${o.y}`));
          const c = t.chairs.find(([x, y]) => !taken.has(`${x},${y}`));
          if (c) api.walkToTile(c[0], c[1]);
          else api.noticeError('That table is full');
        },
      });
    }
    if (api.me?.ex?.meeting) items.push({ ic: svg('stand', 14), label: 'Leave the meeting', run: standUp });
    return items;
  });

  api.onDrawWorld((ctx) => {
    const everyone = [api.me, ...api.others.values()];
    for (const t of TABLES) {
      if (!everyone.some((o) => o?.ex?.meeting?.table === t.id)) continue;
      const b = t.box, pad = 0.2;
      const x = (b.x0 - pad) * T, y = (b.y0 - pad) * T, w = (b.x1 - b.x0 + 1 + pad * 2) * T, h = (b.y1 - b.y0 + 1 + pad * 2) * T;
      ctx.save();
      ctx.fillStyle = 'rgba(59,69,224,.07)';
      ctx.strokeStyle = 'rgba(59,69,224,.55)';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.roundRect(x, y, w, h, 10);
      ctx.fill();
      ctx.stroke();
      ctx.restore();
    }
  });

  api.onTick((now) => {
    if (!api.me) return;
    trackAttempt(now);
    syncMeeting(now);
    if (now - lastVolAt >= VOLUME_EVERY_MS) applyVolumes(now);
    if (now - lastSlow >= 500) {
      lastSlow = now;
      syncScreens();
      renderBar();
      if (pinned && !pinned.isConnected) unpin(false);
    }
  });

  api.onLeave(() => stopShare());
}
