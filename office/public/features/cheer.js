import { html } from '../html.js';
import { overlay } from './play/ui.js';
import { soundSettings, setSound, playClaps, playApplause } from '../sound.js';
import { CLAP_MS, DANCE_MS, CHEER_MS, MAX_PARTS, poseFor, burst, stepParticles, canHear } from './cheer/logic.js';

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
const SPEAKER = html`<path d="M11 4.7a.7.7 0 0 0-1.2-.5L6.4 7.6A1.4 1.4 0 0 1 5.4 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.4a1.4 1.4 0 0 1 1 .4l3.4 3.4a.7.7 0 0 0 1.2-.5z"/>`;
const IC = {
  on: html`${SPEAKER}<path d="M16 9a5 5 0 0 1 0 6"/><path d="M19.4 18.4a9 9 0 0 0 0-12.8"/>`,
  off: html`${SPEAKER}<path d="m22 9-6 6"/><path d="m16 9 6 6"/>`,
};

const NEWS_KEY = 'office-news';
const NEWS_ID = 'applause-1';

export default function setup(api) {
  const parts = [];
  let last = 0;
  let modal = null;

  const person = (id) => (id === api.me?.id ? api.me : api.others.get(id));
  const hears = (o) => canHear(api.me, o, !!api.me && api.zoneAt(o.x, o.y) === api.zoneAt(api.me.x, api.me.y));

  function confetti(o, n) {
    const room = MAX_PARTS - parts.length;
    if (room > 0) parts.push(...burst((o.rx + 0.5) * api.T, o.ry * api.T - 6, Math.min(n, room)));
  }

  api.onEmote((o, e) => {
    const now = performance.now();
    if (e === '👏') {
      o.clapUntil = now + CLAP_MS;
      if (hears(o)) playClaps(4, o === api.me ? 0.6 : 0.4);
    } else if (e === '🎉') confetti(o, 36);
    else if (e === '💃') o.danceUntil = now + DANCE_MS;
  });

  api.onPose((o, now, moving) => (moving ? null : poseFor(o, now)));

  api.onDrawWorld((ctx, now) => {
    const dt = Math.min(50, now - last);
    last = now;
    if (!parts.length) return;
    stepParticles(parts, dt);
    for (const p of parts) {
      ctx.globalAlpha = Math.min(1, p.life / 300);
      ctx.fillStyle = p.c;
      ctx.fillRect(Math.round(p.x), Math.round(p.y), p.s, p.s);
    }
    ctx.globalAlpha = 1;
  });

  api.onMessage('cheer', (m) => {
    const now = performance.now();
    for (const id of Array.isArray(m.ids) ? m.ids.slice(0, 40) : []) {
      const o = person(id);
      if (!o) continue;
      if (!(o.danceUntil > now)) o.clapUntil = Math.max(o.clapUntil || 0, now + CHEER_MS);
      confetti(o, 18);
    }
    playApplause(CHEER_MS / 1000, m.n);
    api.hudNote({ key: 'cheer', title: 'Applause! 👏', body: `${m.n} people are celebrating in ${api.ZONES[m.z]?.name || 'the room'}.` });
  });

  function openSettings() {
    if (modal) return;
    const s = soundSettings();
    modal = overlay(api, {
      title: 'Sound settings',
      body: html`<form class="ch-form">
        <label class="ch-row"><input type="checkbox" name="on"${s.on ? html` checked` : ''}><span><b>Sound effects</b><small>Claps, and applause when three or more people in your room celebrate at once.</small></span></label>
        <label class="ch-row ch-vol"><span><b>Volume</b></span><input type="range" name="vol" min="0" max="100" step="5" value="${Math.round(s.vol * 100)}"></label>
        <p class="ch-note" hidden></p>
        <div class="ch-foot"><button type="button" class="btn" data-ch="test">Play applause</button><button type="button" class="btn btn-primary" data-ch="done">Done</button></div>
      </form>`,
      onClose: () => { modal = null; },
    });
    modal.el.classList.add('ch-ov');
    const form = modal.q('.ch-form');
    const save = () => setSound({ on: form.elements.on.checked, vol: form.elements.vol.value / 100 });
    form.addEventListener('input', save);
    form.addEventListener('click', (e) => {
      const act = e.target.closest('[data-ch]')?.dataset.ch;
      if (act === 'test') {
        save();
        api.setNote(form.querySelector('.ch-note'), playApplause(CHEER_MS / 1000, 6) ? null : { title: 'Sound is off', body: 'Turn sound effects on to hear it.' });
      } else if (act === 'done') modal.close();
    });
  }

  function showNews() {
    if (api.store.get(NEWS_KEY) === NEWS_ID) return;
    const seen = () => api.store.set(NEWS_KEY, NEWS_ID);
    api.hudNote({
      key: 'news',
      title: "🎁 What's new: Applause! 👏",
      body: 'When three of you in a room clap, dance or throw confetti at once, applause breaks out. Also new: profile and avatar editing, desk decoration, and focus sessions from any couch.',
      dismiss: false,
      actions: [
        { label: 'Sound settings', iconHtml: svg(IC.on, 14), run: () => { seen(); openSettings(); } },
        { label: 'Got it!', primary: true, run: seen },
      ],
    });
  }

  api.onInit((m, again) => { if (!again) setTimeout(showNews, 1200); });

  function toggle() {
    const s = setSound({ on: !soundSettings().on });
    api.hudNote({ key: 'sound', title: s.on ? 'Sound effects on' : 'Sound effects off', tone: 'ok' });
  }

  api.meMenu(() => [
    { icon: svg(soundSettings().on ? IC.on : IC.off, 14), label: 'Sound settings', run: openSettings },
  ]);
  api.paletteItems(() => [
    { ic: '👏', label: 'Clap', sub: 'Applause', run: () => api.emote('👏') },
    { ic: '🎉', label: 'Throw confetti', sub: 'Applause', run: () => api.emote('🎉') },
    { ic: '💃', label: 'Dance', sub: 'Applause', run: () => api.emote('💃') },
    { ic: svg(soundSettings().on ? IC.on : IC.off, 14), label: soundSettings().on ? 'Mute sound effects' : 'Unmute sound effects', run: toggle },
    { ic: svg(IC.on, 14), label: 'Sound settings', sub: 'Volume', run: openSettings },
  ]);
}
