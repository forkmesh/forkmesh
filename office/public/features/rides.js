import { html } from '../html.js';
import { TILE as T, GATE, RIDE_GATE, CAR, COASTER_PLATFORM, RIDES, PARK_X0, isWalkable } from '../map.js';
import { SPRITE_FOOT } from '../sprite.js';
import { CLOCK_SLACK_MS, buildTrack } from './coaster/track.js';
import { drawTrack, cart } from './coaster/art.js';
import { timeline, INFO } from './rides/defs.js';
import { drawVehicle } from './rides/vehicles.js';
import { goUse, makeArrival, overlay } from './play/ui.js';

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const LOOP_ICON = html`<path d="M3 20h18"/><path d="M7 20c0-6 2-12 5-12s5 6 5 12"/><circle cx="12" cy="11" r="3"/>`;
const CAR_ICON = html`<path d="M5 17h14v-5l-2-5H7l-2 5z"/><circle cx="8" cy="17" r="2"/><circle cx="16" cy="17" r="2"/>`;
const WAVE_ICON = html`<path d="M2 12c2 0 2-2 4-2s2 2 4 2 2-2 4-2 2 2 4 2 2-2 4-2"/><path d="M2 18c2 0 2-2 4-2s2 2 4 2 2-2 4-2 2 2 4 2 2-2 4-2"/>`;
const SUMMIT = ['drop', 'funnel', 'blaster'];
const RACES = ['race1', 'race2', 'race3', 'race4'];

export const ATTRACTIONS = [
  { key: 'drive-park', ride: 'drive-park', cta: '🚗 Drive to the water park' },
  { key: 'drive-office', ride: 'drive-office', cta: '🚗 Drive back to the office' },
  { key: 'flush', ride: 'flush', cta: '▶ Ride the Royal Flush' },
  { key: 'summit', ride: 'drop', menu: true, cta: '▶ Pick a volcano slide' },
  { key: 'lazy', ride: 'lazy', cta: '▶ Float the Jungle River' },
  { key: 'race', ride: 'race1', race: true, cta: '▶ Race Riptide Racers' },
  { key: 'cliff', ride: 'cliff', cta: '▶ Dive off Skull Rock' },
  { key: 'surf', ride: 'surf', cta: '▶ Surf the Endless Wave' },
];
const GUIDE = ['flush', 'drop', 'funnel', 'blaster', 'lazy', 'race1', 'cliff', 'surf'];

export function nearestAttraction(x, y, reach = 3) {
  let best = null, bd = Infinity;
  for (const a of ATTRACTIONS) {
    const r = RIDES[a.ride];
    const [gx, gy] = r.gate;
    const d = inBoard(a.ride, x, y) ? 0 : Math.max(Math.abs(gx - x), Math.abs(gy - y));
    if (d <= reach && d < bd) { bd = d; best = a; }
  }
  return best;
}

export const rideId = (ride) => (ride && typeof ride.id === 'string' ? ride.id : ride ? 'coaster' : null);

export function rideAt(o, now) {
  const ride = o?.ex?.ride;
  const id = rideId(ride);
  if (!id || !Number.isFinite(ride.at)) return null;
  const tl = timeline(id, ride.at);
  if (!tl) return null;
  const t = now - ride.at;
  if (t < -CLOCK_SLACK_MS || t > tl.duration) return null;
  const tt = Math.max(0, t);
  const p = tl.sample(tt);
  return { id, tl, t: tt, p, prev: tl.sample(Math.max(0, tt - 150)) };
}

export function inBoard(id, x, y) {
  if (id === 'coaster') return y === COASTER_PLATFORM.y && x >= COASTER_PLATFORM.x0 && x <= COASTER_PLATFORM.x1;
  const r = RIDES[id];
  if (!r) return false;
  const [x0, y0, x1, y1] = r.board;
  return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

export function phaseLeft(tl, t, phase) {
  let i = Math.floor(t / 50);
  let n = 0;
  while (i < tl.s.length && tl.s[i].phase === phase) { i++; n++; }
  return n * 50;
}

export default function setup(api) {
  const arrive = makeArrival(api);
  const track = buildTrack();
  let trackCv = null;
  let skew = 0;
  let from = null;
  let askedAt = 0;
  let lastAt = null;
  let menu = null;
  let racedAt = null;
  const serverNow = () => Date.now() + skew;
  const riding = (o) => rideAt(o, serverNow());

  api.onInit((m, again) => {
    if (Number.isFinite(m.now)) skew = m.now - Date.now();
    if (again || !api.store || api.store.get('fm.office.parkHint') || (api.me && api.me.x >= PARK_X0)) return;
    api.store.set('fm.office.parkHint', '1');
    setTimeout(() => api.hudNote({ key: 'park', title: 'New: the ForkMesh Water Park 🌴', body: 'Drive there from the car park by the hallway\'s east door. Slides, a wave pool, the Royal Flush and more.', actions: [{ label: 'Show me', icon: 'walk', primary: true, run: openGuide }] }), 2500);
  });

  function request(id) {
    const me = api.me;
    if (!me || riding(me)) return;
    from = null;
    askedAt = Date.now();
    lastAt = null;
    api.setExtra('ride', id === 'coaster' ? { go: 1 } : { id });
    const info = INFO[id];
    if (id.startsWith('drive')) api.hudNote({ key: 'ride', title: id === 'drive-park' ? 'Off to the water park 🚗' : 'Heading back to the office 🚗', body: 'Buckle up, it is a short drive.' });
    else if (id === 'coaster') api.hudNote({ key: 'ride', title: 'Hold on tight! 🎢', body: 'One lap of the office with three loops.' });
    else api.hudNote({ key: 'ride', title: `${info.name} 💦`, body: info.sub });
  }

  function board(id, then = () => request(id)) {
    const me = api.me;
    if (!me) return;
    if (inBoard(id, me.x, me.y)) return then();
    if (id === 'coaster') return api.hudNote({ key: 'ride', title: 'Step onto the platform', body: 'The ride boards from the platform north of the Garden.' });
    const [x0, y0, x1, y1] = RIDES[id].board;
    let best = null, bd = Infinity;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      if (!isWalkable(x, y)) continue;
      const d = Math.abs(x - me.x) + Math.abs(y - me.y);
      if (d < bd) { bd = d; best = [x, y]; }
    }
    if (!best) return;
    api.walkToTile(best[0], best[1]);
    arrive((m) => inBoard(id, m.x, m.y), then);
  }

  function start(a) {
    if (a.menu) return board(a.ride, chooseSummit);
    return board(a.race ? freeLane() : a.ride);
  }

  let guide = null;
  function openGuide() {
    if (guide) return;
    const me = api.me;
    const inPark = me && me.x >= PARK_X0;
    const rows = inPark ? GUIDE : ['drive-park'];
    const body = html`<div class="ride-menu">
      ${inPark ? '' : html`<p class="ride-note">The water park is a short drive away. Walk out of the hallway's east door to the car park, or press Go.</p>`}
      ${rows.map((id) => html`<button type="button" class="btn ride-pick" data-id="${id}"><b>${INFO[id].name}</b><small>${INFO[id].sub}</small><span class="ride-go">Go ▶</span></button>`)}
      ${inPark ? html`<p class="ride-note">Look for the green ▶ START signs, or walk up to any ride and press the big button at the bottom.</p>` : ''}
    </div>`;
    guide = overlay(api, { title: inPark ? 'Water park guide' : 'Water park', body, onClose: () => { guide = null; } });
    guide.el.addEventListener('click', (e) => {
      const b = e.target.closest('.ride-pick');
      if (!b) return;
      guide.close();
      const id = b.dataset.id;
      board(id.startsWith('race') ? freeLane() : id);
    });
  }

  const cta = typeof document === 'undefined' ? null : document.createElement('button');
  let ctaFor = null;
  if (cta) {
    cta.type = 'button';
    cta.className = 'ride-cta';
    cta.hidden = true;
    cta.addEventListener('click', () => { if (ctaFor) start(ctaFor); });
    (document.querySelector('#stage') || document.body).append(cta);
  }
  function updateCta(me) {
    if (!cta) return;
    const busy = !me || me.ex?.ride;
    const a = busy ? null : nearestAttraction(me.x, me.y);
    if (a === ctaFor) return;
    ctaFor = a;
    cta.hidden = !a;
    if (a) cta.textContent = a.cta;
  }

  function freeLane() {
    const now = serverNow();
    const busy = new Set();
    for (const o of [api.me, ...api.others.values()]) {
      const r = o?.ex?.ride;
      if (r && RACES.includes(r.id) && now - r.at < 4000) busy.add(r.id);
    }
    return RACES.find((id) => !busy.has(id)) || RACES[(Math.random() * 4) | 0];
  }

  function chooseSummit() {
    if (menu) return;
    const body = html`<div class="ride-menu">${SUMMIT.map((id) => html`<button type="button" class="btn ride-pick" data-id="${id}"><b>${INFO[id].name}</b><small>${INFO[id].sub}</small></button>`)}</div>`;
    menu = overlay(api, { title: 'Volcano summit', body, onClose: () => { menu = null; } });
    menu.el.addEventListener('click', (e) => {
      const b = e.target.closest('.ride-pick');
      if (!b) return;
      menu.close();
      board(b.dataset.id);
    });
  }

  function gateAt(tx, ty) {
    if (tx === RIDES.drop.gate[0] && ty === RIDES.drop.gate[1]) return chooseSummit();
    if (tx === RIDES.race1.gate[0] && ty === RIDES.race1.gate[1]) return board(freeLane());
    for (const [id, r] of Object.entries(RIDES)) if (r.gate[0] === tx && r.gate[1] === ty) return board(id);
  }

  api.onObject(GATE, () => board('coaster'));
  api.onObject(RIDE_GATE, (tx, ty) => gateAt(tx, ty));
  api.onObject(CAR, (tx, ty) => gateAt(tx, ty));
  const byKey = (k) => ATTRACTIONS.find((a) => a.key === k);
  api.onObject('l', () => start(byKey('flush')));
  api.onObject('b', () => board('funnel'));
  api.onObject('z', () => start(byKey('race')));
  api.onObject('=', () => start(byKey('race')));
  api.onObject('f', () => start(byKey('surf')));
  api.onObject('q', (tx) => start(byKey(tx < PARK_X0 + 62 ? 'summit' : 'cliff')));

  api.onPlace((o) => {
    const r = riding(o);
    if (!r) return;
    o.rx = r.p.x - 0.5;
    o.ry = r.p.y - 0.5 - r.p.lift;
  });

  api.onPose((o) => {
    const r = riding(o);
    if (!r) return null;
    const dx = r.p.x - r.prev.x;
    return { sit: true, step: 0, dir: r.tl.kind === 'car' ? (dx < 0 ? 2 : 3) : 0, pose: r.p.wild ? 3 : 1 };
  });

  function raceResult(mine) {
    const field = [];
    for (const o of [api.me, ...api.others.values()]) {
      const r = o?.ex?.ride;
      if (!r || !RACES.includes(r.id) || Math.abs(r.at - mine.at) > 3000) continue;
      const tl = timeline(r.id, r.at);
      field.push({ me: o === api.me, name: o.name || 'Someone', end: r.at + tl.duration });
    }
    field.sort((a, b) => a.end - b.end);
    const place = field.findIndex((f) => f.me) + 1;
    if (!place) return;
    const ord = ['1st 🥇', '2nd 🥈', '3rd 🥉', '4th'][place - 1] || `${place}th`;
    api.hudNote({ key: 'ride', title: field.length > 1 ? `Riptide Racers: ${ord} of ${field.length}` : 'Riptide Racers: solo run 🏁', body: field.length > 1 ? `Winner: ${field[0].me ? 'you!' : field[0].name}` : 'Bring a friend and race all four lanes.' });
  }

  api.onTick(() => {
    const me = api.me;
    updateCta(me);
    const ride = me?.ex?.ride;
    if (!ride) return;
    if (Number.isFinite(ride.at) && ride.at !== lastAt) {
      lastAt = ride.at;
      from = { x: me.x, y: me.y };
    }
    const moved = from && (me.x !== from.x || me.y !== from.y);
    const pending = !Number.isFinite(ride.at) && Date.now() - askedAt < 3000;
    if (moved || (!pending && !riding(me))) {
      if (!moved && RACES.includes(ride.id) && racedAt !== ride.at) { racedAt = ride.at; raceResult(ride); }
      if (!moved && ride.id === 'drive-park') {
        api.hudNote({ key: 'park', title: 'Welcome to the ForkMesh Water Park 🌴', body: 'Every ride starts at a green ▶ START sign. Walk up to one and press the big button, or pick from the guide.', actions: [{ label: 'Show attractions', icon: 'walk', primary: true, run: openGuide }] });
      }
      from = null;
      api.setExtra('ride', null);
    }
  });

  api.onDrawUnder((ctx) => {
    if (!trackCv) trackCv = drawTrack(track);
    ctx.drawImage(trackCv, 0, 0);
  });

  api.onDrawWorld((ctx, now, vis) => {
    let parked = true;
    const sn = serverNow();
    for (const o of vis) {
      const r = riding(o);
      if (!r) continue;
      if (r.id === 'coaster') parked = false;
      const x = Math.round((o.rx + 0.5) * T), y = Math.round((o.ry + 1) * T - SPRITE_FOOT + 34);
      drawVehicle(ctx, r.tl.kind, x, y, {
        dir: r.p.x - r.prev.x, id: r.id, phase: r.p.phase,
        phaseLeft: r.p.phase ? phaseLeft(r.tl, r.t, r.p.phase) : 0,
      });
      if (r.p.phase === 'gate' && Math.floor(sn / 400) % 2) {
        ctx.fillStyle = '#e5484d';
        ctx.fillRect(x - 2, y - 52, 4, 4);
      }
    }
    if (parked) cart(ctx, Math.round(track.start.x * T), Math.round(track.start.y * T + 4), '#c2403c');
    const pulse = 0.75 + 0.25 * Math.sin(now / 260);
    ctx.font = '800 11px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (const a of ATTRACTIONS) {
      const [gx, gy] = RIDES[a.ride].gate;
      const x = (gx + 0.5) * T, y = gy * T - 34 - pulse * 4;
      ctx.globalAlpha = pulse;
      ctx.fillStyle = '#16a34a';
      ctx.beginPath();
      ctx.roundRect(x - 30, y - 9, 60, 18, 9);
      ctx.fill();
      ctx.fillStyle = '#ffffff';
      ctx.fillText(a.key.startsWith('drive') ? '🚗 DRIVE' : '▶ START', x, y + 1);
      ctx.globalAlpha = 1;
    }
  });

  api.paletteItems(() => {
    const me = api.me;
    const inPark = me && me.x >= PARK_X0;
    const items = [];
    items.push({ ic: svg(WAVE_ICON, 14), label: 'Water park guide', sub: 'Every attraction and how to start it', run: openGuide });
    if (!inPark) {
      items.push({ ic: svg(LOOP_ICON, 14), label: 'Ride the rollercoaster', sub: 'Garden platform', run: () => goUse(api, arrive, GATE, () => board('coaster')) });
      items.push({ ic: svg(CAR_ICON, 14), label: 'Drive to the water park', sub: 'Car park by the east door', run: () => board('drive-park') });
      return items;
    }
    items.push({ ic: svg(CAR_ICON, 14), label: 'Drive back to the office', sub: 'Park entrance', run: () => board('drive-office') });
    for (const id of GUIDE) {
      items.push({ ic: svg(WAVE_ICON, 14), label: INFO[id].name, sub: INFO[id].sub, run: () => board(id === 'race1' ? freeLane() : id) });
    }
    return items;
  });
}
