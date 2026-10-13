import { html, setHtml } from '../html.js';
import { W, H, obj, COUCHES, isCouch } from '../map.js';
import { playChime } from '../sound.js';
import { overlay, makeArrival } from './play/ui.js';
import { MODES, MODE_LABEL, MINUTES, ROUNDS_MAX, TOPIC_MAX, phase, fmtClock, describe, shortLeft, roundsMinutes, nearestCouch, RECENT_KEY, FOCUS_EMOJI, pushRecent, readRecent, recentLabel, startMessage, focusStatusOf } from './focus/logic.js';

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const TARGET = html`<circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="6"/><circle cx="12" cy="12" r="2"/>`;
const BREAK = { s: 'break', emoji: '☕', text: 'Pomodoro break' };
const STATUS_KEY = 'office-focus-status';
const LOUNGE = 'f';
const ERRORS = {
  seat: ['Sit on a couch first', 'Focus sessions run from a couch or bean bag.'],
  gone: ['That session has ended', 'Start your own from the couch.'],
  bad: ['Could not start the session', 'Pick a mode and a duration.'],
};

export default function setup(api) {
  const arrive = makeArrival(api);
  const couches = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (COUCHES.has(obj[y][x])) couches.push({ x, y });
  let skew = 0;
  let dialog = null;
  let saved;
  let lastKind = null;
  let completed = false;
  let lastTick = 0;
  let hudKey = '';
  let statusOn = true;
  let pending = null;
  let loungeKey = '';
  let recent = readRecent(api.store.get(RECENT_KEY));
  const statusDefault = () => api.store.get(STATUS_KEY) !== '0';

  const now = () => Date.now() + skew;
  const mine = () => api.me?.ex?.focus || null;
  const seated = () => !!api.me && isCouch(api.me.x, api.me.y);
  const focusStatus = focusStatusOf;
  const same = (a, b) => !!a && !!b && a.s === b.s && a.emoji === b.emoji && a.text === b.text;
  const taken = () => new Set([...api.others.values()].map((o) => o.y * 1000 + o.x));

  const hud = document.createElement('div');
  hud.className = 'fo-hud';
  hud.hidden = true;
  if (api.$('#notes')) api.$('#notes').before(hud);
  else (api.$('#stage') || document.body).append(hud);
  hud.addEventListener('click', (e) => {
    const act = e.target.closest('[data-fo]')?.dataset.fo;
    if (act === 'end') api.send({ t: 'focus-end' });
    else if (act === 'open') openDialog();
  });

  const lounge = document.createElement('div');
  lounge.className = 'fo-lounge';
  lounge.hidden = true;
  (api.$('#stage') || document.body).append(lounge);
  lounge.addEventListener('click', (e) => {
    const b = e.target.closest('[data-recent]');
    if (b) startRecent(recent[+b.dataset.recent]);
    else if (e.target.closest('[data-fo="open"]')) startNearest();
  });

  function paintLounge() {
    const show = !!api.me && !mine() && !dialog && recent.length > 0 && api.zoneAt(api.me.x, api.me.y) === LOUNGE;
    const key = show ? recent.map(recentLabel).join('|') : '';
    if (key === loungeKey) return;
    loungeKey = key;
    lounge.hidden = !show;
    if (!show) return;
    setHtml(lounge, html`<header><b>Your recent focus sessions</b><span class="muted">One click to start again</span></header>
      ${recent.map((r, i) => html`<button type="button" class="btn" data-recent="${i}">${svg(TARGET, 14)}<span>${recentLabel(r)}</span>${r.status ? '' : html`<small class="muted">no status</small>`}</button>`)}
      <button type="button" class="btn btn-sm" data-fo="open">New session</button>`);
  }

  function send(m, opts) {
    pending = opts;
    api.send(m);
  }

  function startRecent(r) {
    if (!r || !api.me) return;
    if (mine()) return api.notice('You are in a focus session', 'End it from the timer above the dock.');
    const go = () => send(startMessage(r), { status: r.status, notify: r.notify });
    if (seated()) return go();
    goSit(nearestCouch(couches, taken(), api.me), go);
  }

  function idleHud() {
    if (!seated() || dialog) {
      if (hudKey) {
        hud.hidden = true;
        hudKey = '';
      }
      return;
    }
    hud.hidden = false;
    if (hudKey === 'idle') return;
    hudKey = 'idle';
    setHtml(hud, html`<span class="muted">On the couch</span><button type="button" class="btn btn-sm btn-primary" data-fo="open">Start focus session</button>`);
  }

  function paintHud(f, p) {
    const label = p.kind === 'break' ? 'Break' : 'Focus';
    const time = fmtClock(p.left === null ? p.elapsed : p.left);
    const key = `${label}|${time}|${f.topic || ''}|${f.emoji || ''}`;
    hud.hidden = false;
    if (key === hudKey) return;
    hudKey = key;
    setHtml(hud, html`<i class="fo-dot${p.kind === 'break' ? ' brk' : ''}"></i>${f.emoji ? html`<span>${f.emoji}</span>` : ''}<b>${label}</b><span class="fo-time">${time}</span>${f.topic ? html`<span class="fo-topic">${f.topic}</span>` : ''}<button type="button" class="btn btn-sm" data-fo="end">End</button>`);
  }

  function begin(f) {
    const st = api.me.ex.status;
    saved = st ? { ...st } : null;
    lastKind = 'work';
    completed = false;
    statusOn = pending ? pending.status !== false : statusDefault();
    if (!f.host && pending) {
      recent = pushRecent(recent, f, pending);
      api.store.set(RECENT_KEY, JSON.stringify(recent));
    }
    pending = null;
    api.hudNote({ key: 'focus-open' });
    if (statusOn) api.setExtra('status', focusStatus(f));
    if (api.local.audio) api.setMedia('audio', false);
    dialog?.close();
  }

  function finish(f) {
    const st = api.me?.ex?.status;
    const ours = !st || same(st, BREAK) || (f && same(st, focusStatus(f))) || st.s === 'focus';
    if (api.me && ours && statusOn) api.setExtra('status', saved || null);
    saved = undefined;
    lastKind = null;
    hud.hidden = true;
    hudKey = '';
    if (!completed) return;
    completed = false;
    playChime([523, 659, 784]);
    api.hudNote({ key: 'focus', title: 'Focus session complete', body: f ? `${describe(f)}${f.topic ? ' · ' + f.topic : ''}. Nice work.` : 'Nice work.', tone: 'ok' });
  }

  let ended = null;
  api.onExtra((o, k, v) => {
    if (o !== api.me || k !== 'focus') return;
    if (v && saved === undefined) begin(v);
    else if (!v && saved !== undefined) finish(ended);
    if (v) ended = v;
  });

  api.onInit((m, again) => {
    if (Number.isFinite(m.now)) skew = m.now - Date.now();
    if (saved !== undefined && !mine()) finish(null);
    if (!again) hud.hidden = true;
  });

  api.onTick((t) => {
    if (t - lastTick < 250) return;
    lastTick = t;
    paintLounge();
    const f = mine();
    if (!f || saved === undefined) return idleHud();
    const p = phase(f, now());
    if (p.kind === 'done') {
      if (!completed) {
        completed = true;
        api.send({ t: 'focus-end' });
      }
      return;
    }
    if (p.kind !== lastKind) {
      lastKind = p.kind;
      if (statusOn) api.setExtra('status', p.kind === 'break' ? { ...BREAK } : focusStatus(f));
      playChime(p.kind === 'break' ? [784, 659] : [659, 784]);
      api.hudNote({ key: 'focus', title: p.kind === 'break' ? 'Break time' : 'Back to focus', body: p.kind === 'break' ? 'Five minutes. Stretch, then the next round starts.' : `Round ${p.round} of ${f.rounds}.` });
    }
    paintHud(f, p);
  });

  api.onDrawScreen((ctx, o, sx, above) => {
    const f = o.ex?.focus;
    if (!f) return above;
    const left = shortLeft(f, now());
    if (!left) return above;
    const text = f.emoji ? `${f.emoji} ${left}` : left;
    const brk = left.startsWith('Break');
    ctx.save();
    ctx.font = '600 10.5px -apple-system, system-ui, sans-serif';
    const w = Math.round(ctx.measureText(text).width + 12), h = 17;
    const bx = Math.round(sx - w / 2), by = Math.round(above - h);
    ctx.fillStyle = brk ? '#fff3d6' : '#fde8e8';
    ctx.beginPath();
    ctx.roundRect(bx, by, w, h, 6);
    ctx.fill();
    ctx.fillStyle = brk ? '#92400e' : '#b42318';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    ctx.fillText(text, bx + 6, by + h / 2 + 0.5);
    ctx.restore();
    ctx.textAlign = 'center';
    return by - 4;
  });

  function sessions() {
    const seen = new Set();
    const out = [];
    for (const o of api.others.values()) {
      const f = o.ex?.focus;
      const key = f?.host || o.id;
      if (!f || seen.has(key) || !shortLeft(f, now())) continue;
      seen.add(key);
      out.push(o);
    }
    return out.slice(0, 3);
  }

  function lengths(mode) {
    if (mode === 'focused') return MINUTES.map((m) => ({ v: m, label: `${m} minutes`, on: m === 60 }));
    if (mode === 'pomodoro') return Array.from({ length: ROUNDS_MAX }, (_, i) => ({ v: i + 1, label: `${i + 1} ${i ? 'rounds' : 'round'} · ${roundsMinutes(i + 1)} minutes`, on: i === 1 }));
    return [];
  }

  function openDialog() {
    if (dialog || !api.me) return;
    if (mine()) return api.notice('You are in a focus session', 'End it from the timer above the dock.');
    let mode = MODES[0];
    let emoji = '';
    const open = sessions();
    dialog = overlay(api, {
      title: 'Focused coworking',
      body: html`<form class="fo-form">
        <p class="muted">Built for deep work. Your status turns to Focus, calls are blocked and your mic is muted until the session ends or you get up.</p>
        ${recent.length ? html`<div class="fo-recent"><span class="fo-lbl">Recent</span>${recent.map((r, i) => html`<button type="button" class="btn" data-recent="${i}">${svg(TARGET, 14)}${recentLabel(r)}</button>`)}</div>` : ''}
        ${open.length ? html`<div class="fo-join">${open.map((o) => html`<button type="button" class="btn" data-join="${o.id}">${svg(TARGET, 14)}Join ${o.name} · ${shortLeft(o.ex.focus, now())}${o.ex.focus.topic ? ` · ${o.ex.focus.topic}` : ''}</button>`)}</div>` : ''}
        <div class="fo-seg">${MODES.map((m) => html`<button type="button" data-mode="${m}" class="${m === mode ? 'on' : ''}">${MODE_LABEL[m]}</button>`)}</div>
        <label class="pf-field"><span>Topic (optional)</span><input class="input" name="topic" maxlength="${TOPIC_MAX}" placeholder="Enter topic" autocomplete="off"></label>
        <div class="pf-field"><span>Emoji (optional)</span><div class="fo-emoji" role="group" aria-label="Focus emoji">${FOCUS_EMOJI.map((e) => html`<button type="button" data-emoji="${e}" aria-pressed="false" title="${e}">${e}</button>`)}</div></div>
        <label class="fo-check"><input type="checkbox" name="status"${statusDefault() ? html` checked` : ''}><span>Set my status to this focus<small class="muted">Shows your emoji and topic as your status until the session ends.</small></span></label>
        <label class="pf-field" data-fo="len"><span>Duration</span><select class="input" name="len"></select></label>
        <label class="pf-field"><span>Let others know?</span><select class="input" name="notify"><option value="1">Yes — everyone available</option><option value="0">No</option></select></label>
        <p class="fo-err" hidden></p>
        <button type="submit" class="btn btn-primary btn-block">Start session</button>
      </form>`,
      onClose: () => { dialog = null; },
    });
    dialog.el.classList.add('fo-ov');
    const form = dialog.q('.fo-form');
    const paintLen = () => {
      const list = lengths(mode);
      form.querySelector('[data-fo="len"]').hidden = !list.length;
      setHtml(form.elements.len, html`${list.map((l) => html`<option value="${l.v}"${l.on ? html` selected` : ''}>${l.label}</option>`)}`);
    };
    paintLen();
    form.addEventListener('click', (e) => {
      const b = e.target.closest('button');
      if (b?.dataset.mode) {
        mode = b.dataset.mode;
        for (const s of form.querySelectorAll('[data-mode]')) s.classList.toggle('on', s === b);
        paintLen();
      } else if (b?.dataset.emoji) {
        emoji = emoji === b.dataset.emoji ? '' : b.dataset.emoji;
        for (const s of form.querySelectorAll('[data-emoji]')) s.setAttribute('aria-pressed', String(s.dataset.emoji === emoji));
      } else if (b?.dataset.recent) {
        const r = recent[+b.dataset.recent];
        if (r) send(startMessage(r), { status: r.status, notify: r.notify });
      } else if (b?.dataset.join) send({ t: 'focus-start', join: b.dataset.join }, { status: form.elements.status.checked });
    });
    form.onsubmit = (e) => {
      e.preventDefault();
      const status = form.elements.status.checked;
      api.store.set(STATUS_KEY, status ? '1' : '0');
      const m = { t: 'focus-start', mode, topic: form.elements.topic.value, notify: form.elements.notify.value === '1' };
      if (emoji) m.emoji = emoji;
      if (mode === 'focused') m.mins = +form.elements.len.value;
      if (mode === 'pomodoro') m.rounds = +form.elements.len.value;
      send(m, { status, notify: m.notify });
    };
  }

  function goSit(spot, then) {
    if (!spot) return api.noticeError('No free couch', 'Every couch and bean bag is taken.');
    if (api.me.x === spot.x && api.me.y === spot.y) return then();
    api.walkToTile(spot.x, spot.y);
    arrive((me) => me.x === spot.x && me.y === spot.y, then);
  }

  for (const ch of COUCHES) api.onObject(ch, (tx, ty) => goSit({ x: tx, y: ty }, openDialog));

  function join(id) {
    const host = api.others.get(id);
    if (!host?.ex?.focus) return api.noticeError(...ERRORS.gone);
    const go = () => api.send({ t: 'focus-start', join: id });
    if (seated()) return go();
    goSit(nearestCouch(couches, taken(), host), go);
  }

  const startNearest = () => (seated() ? openDialog() : goSit(nearestCouch(couches, taken(), api.me), openDialog));

  api.onMessage('focus-err', (m) => {
    pending = null;
    const [title, body] = ERRORS[m.reason] || ERRORS.bad;
    const inDialog = dialog?.q('.fo-err');
    if (inDialog) api.setNote(inDialog, { title, body, tone: 'error' });
    else api.noticeError(title, body);
  });
  api.onMessage('focus-open', (m) => {
    const st = api.me?.ex?.status?.s;
    if (!m.focus || mine() || api.me?.away || st === 'focus' || st === 'away' || st === 'meeting') return;
    api.hudNote({ key: 'focus-open', title: `${m.name} started a focus session`, body: `${describe(m.focus)}${m.focus.topic ? ' · ' + m.focus.topic : ''}`, actions: [{ label: 'Join', primary: true, run: () => join(m.id) }] });
  });

  api.personActions((o) => (o.ex?.focus && shortLeft(o.ex.focus, now()) && !mine()
    ? [{ icon: svg(TARGET, 14), tip: 'Join focus session', run: () => join(o.id) }]
    : []));

  api.paletteItems(() => (mine()
    ? [{ ic: svg(TARGET, 14), label: 'End focus session', run: () => api.send({ t: 'focus-end' }) }]
    : [{ ic: svg(TARGET, 14), label: 'Start a focus session', sub: 'Focus Lounge', run: startNearest }]));
}
