import { html, trusted, setHtml } from '../html.js';

const svg = (d, s = 16) => trusted(`<svg width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`);
const I = {
  login: '<path d="M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4"/><path d="m10 17 5-5-5-5"/><path d="M15 12H3"/>',
  logout: '<path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><path d="m16 17 5-5-5-5"/><path d="M21 12H9"/>',
  building: '<rect width="16" height="20" x="4" y="2" rx="2"/><path d="M9 22v-4h6v4"/><path d="M8 6h.01"/><path d="M16 6h.01"/><path d="M12 6h.01"/><path d="M12 10h.01"/><path d="M12 14h.01"/><path d="M16 10h.01"/><path d="M16 14h.01"/><path d="M8 10h.01"/><path d="M8 14h.01"/>',
  lock: '<rect width="18" height="11" x="3" y="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>',
  info: '<circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/>',
};
const SESSION = '/api/office/session';
const ENTRY = '/api/office/entry';
const ICE_REFRESH_MS = 8 * 60 * 1000;
const ICE_RETRY_MS = 60 * 1000;
const TASKS_RE = /^\/api\/tasks(?:[/?]|$)/;
const CALORIES = '/api/calories';
const CALORIES_ENTRIES = '/api/calories/entries';
const ORG_RE = /^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/;

export default function setup(api) {
  const { $, tint } = api;
  const params = new URLSearchParams(location.search);
  const wanted = (params.get('org') || '').trim().toLowerCase();
  let state = null;
  let denied = '';
  let entering = false;
  let orgErr = '';
  const REMEMBER = 'office-org';
  const remembered = () => { const o = (api.store.get(REMEMBER) || '').trim().toLowerCase(); return ORG_RE.test(o) ? o : ''; };

  const form = $('#join-form');
  const nameLabel = form?.querySelector('label[for="name"]');
  const nameInput = $('#name');
  const submit = form?.querySelector('button[type="submit"]');
  const lead = form?.querySelector('p');
  const title = form?.querySelector('h1');
  const box = document.createElement('div');
  box.className = 'id-box';
  if (form && lead) lead.after(box);
  const loginUrl = () => '/login?next=' + encodeURIComponent(location.pathname + location.search);
  for (const el of [nameLabel, nameInput, submit]) if (el) el.hidden = true;
  if (nameInput) nameInput.required = false;

  function setState(s) {
    state = s && s.signedIn ? s : null;
    const a = state?.account;
    api.account = a && state.org ? { id: a.name, name: a.name, org: state.org, teams: (state.teams || []).slice(), role: state.role } : null;
    api.tasksEnabled = !!(state && state.tasks);
    api.officeChat = state && state.chat && typeof state.chat === 'object' ? { ...state.chat } : {};
    api.forkmesh = async (path, init = {}) => {
      const p = '/' + String(path || '').replace(/^\/+/, '');
      if (!api.account || !api.tasksEnabled || !TASKS_RE.test(p) || p.length > 512) throw new Error('not available');
      return fetch(p, { ...init, credentials: 'same-origin' });
    };
    api.calories = {
      overview: () => (api.account ? fetch(CALORIES, { credentials: 'same-origin', cache: 'no-store' }) : Promise.reject(new Error('not available'))),
      log: (entry) => (api.account
        ? fetch(CALORIES_ENTRIES, { method: 'POST', credentials: 'same-origin', headers: { 'content-type': 'application/json' }, body: JSON.stringify(entry) })
        : Promise.reject(new Error('not available'))),
    };
    document.dispatchEvent(new CustomEvent('office:account', { detail: api.account }));
  }

  async function loadSession(org) {
    const q = org && ORG_RE.test(org) ? '?org=' + encodeURIComponent(org) : '';
    const r = await fetch(SESSION + q, { credentials: 'same-origin', cache: 'no-store' });
    if (!r.ok) throw new Error('session ' + r.status);
    return r.json();
  }

  function heading(markup, text) {
    if (title) setHtml(title, markup);
    if (lead) lead.textContent = text;
  }

  function renderJoin(fail) {
    if (!form) return;
    const a = state?.account;
    const lines = [];
    if (entering && state?.org && !denied) {
      heading(html`Entering the ${state.org} office…`, 'Your microphone and camera stay off until you turn them on.');
      setHtml(box, html``);
      return;
    }
    if (denied) lines.push(html`<div class="id-note id-warn">${svg(I.info, 14)}<span>${denied}</span></div>`);
    if (orgErr) lines.push(html`<div class="id-note id-warn" role="alert">${svg(I.info, 14)}<span>${orgErr}</span></div>`);
    if (fail) {
      heading(html`Could not reach ForkMesh`, 'Check your connection and reload the page.');
    } else if (!a) {
      heading(html`Let's get you in <span aria-hidden="true">👋</span>`, "Sign in with ForkMesh to enter your organization's office.");
      lines.push(html`<a class="btn btn-primary btn-block id-signin" href="${loginUrl()}">${svg(I.login, 15)}Sign in with ForkMesh</a>`);
      lines.push(html`<div class="id-note">${svg(I.lock, 14)}<span>The office is for members of your ForkMesh organization.</span></div>`);
    } else if (!state.org) {
      heading(html`No office yet`, 'Each organization has its own office.');
    } else {
      heading(html`You are not in the ${state.org} office`, 'You are signed in with ForkMesh. Your teammates will see your account name.');
    }
    if (a) {
      const orgs = state.orgs || [];
      lines.push(html`<div class="id-card">
        <span class="av" style="background:${tint(a.av)}">${(a.name[0] || '?').toUpperCase()}</span>
        <span class="id-who"><b>${a.name}</b><small>${state.org ? `${state.org}${state.role ? ' · ' + state.role : ''}` : 'No organization'}</small></span>
        <button type="button" class="icon-btn id-out" data-tip="Sign out" aria-label="Sign out">${svg(I.logout, 15)}</button>
      </div>`);
      if (orgs.length > 1) lines.push(html`<select class="input id-org" aria-label="Organization">${orgs.map((o) => html`<option value="${o.name}"${o.name === state.org ? html` selected` : ''}>${o.name}</option>`)}</select>`);
      if (!state.org) lines.push(html`<div class="id-note">${svg(I.info, 14)}<span>Create or join an organization on ForkMesh to open yours.</span></div><a class="btn btn-primary btn-block" href="/dashboard">${svg(I.building, 15)}Open ForkMesh</a>`);
      else lines.push(html`<button type="button" class="btn btn-primary btn-block id-enter">${svg(I.building, 15)}Enter the ${state.org} office</button>`);
    }
    setHtml(box, html`${lines}`);
    box.querySelector('.id-enter')?.addEventListener('click', enter);
    box.querySelector('.id-out')?.addEventListener('click', signOut);
    box.querySelector('.id-org')?.addEventListener('change', (e) => switchOrg(e.target.value));
  }

  function enter() {
    if (!state?.org) return;
    denied = '';
    entering = true;
    api.store.set(REMEMBER, state.org);
    renderJoin();
    api.connect();
  }

  async function switchOrg(org) {
    orgErr = '';
    try {
      setState(await loadSession(org));
      if (state?.org) {
        api.store.set(REMEMBER, state.org);
        const u = new URL(location.href);
        u.searchParams.set('org', state.org);
        history.replaceState(null, '', u);
      }
    } catch {
      orgErr = "Couldn't switch organization. Try again in a moment.";
    }
    renderJoin();
  }

  async function signOut() {
    try {
      await fetch('/api/accounts/logout', { method: 'POST', credentials: 'same-origin', headers: { 'content-type': 'application/json' }, body: '{}' });
    } catch {}
    location.reload();
  }

  function showJoin(reason) {
    denied = reason || '';
    entering = false;
    $('#join') && ($('#join').hidden = false);
    $('#app') && ($('#app').hidden = true);
    renderJoin();
  }

  const postEntry = () => fetch(ENTRY, {
    method: 'POST',
    credentials: 'same-origin',
    cache: 'no-store',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ org: state.org }),
  });
  let iceAt = 0;
  let iceBusy = false;
  const setIce = (list) => {
    api.iceServers = Array.isArray(list) ? list : [];
    iceAt = Date.now();
    if (api.mesh) api.mesh.iceServers = api.iceServers;
  };

  api.refreshIce = () => {
    if (iceBusy || !state?.org || !iceAt || Date.now() - iceAt < ICE_REFRESH_MS) return;
    iceBusy = true;
    postEntry()
      .then((r) => (r.ok ? r.json() : null))
      .then((j) => { if (j && Array.isArray(j.iceServers)) setIce(j.iceServers); else iceAt = Date.now() - ICE_REFRESH_MS + ICE_RETRY_MS; })
      .catch(() => { iceAt = Date.now() - ICE_REFRESH_MS + ICE_RETRY_MS; })
      .finally(() => { iceBusy = false; });
  };

  api.socketSource = async () => {
    if (!state?.org) return null;
    const r = await postEntry();
    if (r.status === 401) {
      setState(null);
      showJoin('Your ForkMesh sign-in has ended. Sign in again to enter.');
      location.replace(loginUrl());
      return null;
    }
    if (r.status === 403) {
      showJoin(`You are no longer a member of ${state.org}.`);
      return null;
    }
    if (!r.ok) throw new Error('entry ' + r.status);
    const j = await r.json();
    if (!j || typeof j.ws !== 'string' || !j.ws.startsWith('/api/office/ws?')) throw new Error('entry');
    setIce(j.iceServers);
    return `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}${j.ws}`;
  };

  api.onDenied((reason) => showJoin(reason || 'You could not enter the office.'));

  api.onInit((m) => {
    const org = m.identity?.org || state?.org;
    const h = $('.side-head h2');
    if (h && org) h.textContent = `${org} office`;
    const inv = $('#panel-people .invite strong');
    if (inv && org) inv.textContent = `Experience the ${org} office together`;
    if (org) {
      const u = new URL(location.href);
      if (u.searchParams.get('org') !== org) {
        u.searchParams.set('org', org);
        history.replaceState(null, '', u);
      }
    }
    renderAccountStrip(m.identity);
  });

  for (const b of document.querySelectorAll('.copy-link')) {
    b.onclick = () => {
      const u = new URL(location.pathname, location.origin);
      if (state?.org) u.searchParams.set('org', state.org);
      const link = u.toString();
      navigator.clipboard?.writeText(link).catch(() => {});
      api.setNote($('#invite-note'), { title: 'Invite link copied', body: `${link} · teammates sign in with ForkMesh to enter.`, tone: 'ok' });
    };
  }

  function renderAccountStrip(info) {
    const panel = $('#panel-people');
    if (!panel) return;
    let el = panel.querySelector('.id-strip');
    if (!el) {
      el = document.createElement('div');
      el.className = 'id-strip';
      const inv = panel.querySelector('.invite');
      if (inv) inv.after(el);
      else panel.prepend(el);
    }
    const a = state?.account;
    const orgs = state?.orgs || [];
    const rooms = info?.rooms || {};
    const unlocked = new Set(info?.unlocked || []);
    const chips = Object.entries(rooms).map(([z, n]) => html`<span class="id-room${unlocked.has(z) ? ' on' : ''}" data-tip="${unlocked.has(z) ? 'You can enter' : 'Team members only'}">${unlocked.has(z) ? '' : svg(I.lock, 11)}${n}</span>`);
    setHtml(el, html`<span class="id-who"><b>${a?.name || ''}</b><small>${state?.org || ''}${(state?.teams || []).length ? ' · ' + state.teams.join(', ') : ''}</small></span>
      <button type="button" class="icon-btn id-out" data-tip="Sign out" aria-label="Sign out">${svg(I.logout, 14)}</button>
      ${orgs.length > 1 ? html`<select class="input id-org" aria-label="Switch office">${orgs.map((o) => html`<option value="${o.name}"${o.name === state.org ? html` selected` : ''}>${o.name}</option>`)}</select>` : ''}
      ${chips.length ? html`<div class="id-rooms">${chips}</div>` : ''}`);
    el.querySelector('.id-out')?.addEventListener('click', signOut);
    el.querySelector('.id-org')?.addEventListener('change', (e) => moveTo(e.target.value));
  }

  function moveTo(org) {
    if (!ORG_RE.test(org) || org === state?.org) return;
    api.store.set(REMEMBER, org);
    const u = new URL(location.href);
    u.searchParams.set('org', org);
    location.assign(u.toString());
  }

  api.onMessage('identity:locked', (m) => {
    const room = m.room || 'This room';
    const ev = new CustomEvent('office:locked', { detail: { z: String(m.z || ''), room, guest: false }, cancelable: true });
    document.dispatchEvent(ev);
    if (!ev.defaultPrevented) api.notice(`${room} is locked`, `Only the ${room} team can enter. Knock to ask someone inside.`);
  });

  api.personBadge((o) => {
    const t = o.ex?.teams;
    if (!Array.isArray(t) || !t.length) return '';
    return html` <span class="id-team">${t[0]}</span>`;
  });

  const first = wanted || remembered();
  loadSession(first)
    .then((s) => {
      if (!s || !s.signedIn) {
        setState(null);
        renderJoin();
        location.replace(loginUrl());
        return;
      }
      setState(s);
      if (!state.org) return renderJoin();
      if (wanted && wanted !== state.org) api.notice(`You are not a member of ${wanted}`, `You entered the ${state.org} office instead.`);
      enter();
    })
    .catch(() => {
      setState(null);
      renderJoin(true);
    });
}
