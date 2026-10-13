import { html, setHtml, trusted, join } from '../html.js';
import {
  findDesks, deskAt, deskKey, parseTaskPage, normTask, apiError, snapshot, diffTasks, findMentions, newReplies,
  filterTasks, sortTasks, pollInterval, fmtElapsed, liveElapsed, pingTargets, short, STATUS_LABEL, cleanTask,
} from './work/logic.js';
import { cleanDeco, decoEmpty, HEAD } from './work/deco.js';
import { openDecorator, decoSprite } from './work/decorator.js';

const svg = (p, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${p}</svg>`;
const P = {
  tasks: html`<path d="m3 17 2 2 4-4"/><path d="m3 7 2 2 4-4"/><path d="M13 6h8"/><path d="M13 12h8"/><path d="M13 18h8"/>`,
  monitor: html`<rect width="20" height="14" x="2" y="3" rx="2"/><path d="M8 21h8"/><path d="M12 17v4"/>`,
  play: html`<polygon points="6 3 20 12 6 21 6 3"/>`,
  stop: html`<rect width="14" height="14" x="5" y="5" rx="2"/>`,
  check: html`<path d="M20 6 9 17l-5-5"/>`,
  refresh: html`<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>`,
  x: html`<path d="M18 6 6 18"/><path d="m6 6 12 12"/>`,
  target: html`<circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="6"/><circle cx="12" cy="12" r="2"/>`,
  chat: html`<path d="M7.9 20A9 9 0 1 0 4 16.1L2 22Z"/>`,
  brush: html`<path d="m9.06 11.9 8.07-8.06a2.85 2.85 0 1 1 4.03 4.03l-8.06 8.08"/><path d="M7.07 14.94c-1.66 0-3 1.35-3 3.02 0 1.33-2.5 1.52-2 2.02 1.08 1.1 2.49 2.02 4 2.02 2.2 0 4-1.8 4-4.04a3.01 3.01 0 0 0-3-3.02z"/>`,
  walk: html`<circle cx="13" cy="4" r="2"/><path d="m9 20 3-6 3 3v6"/><path d="m6 8 3-2 4 1 2 4 3 1"/><path d="M9 11.5 7 20"/>`,
  lock: html`<rect width="18" height="11" x="3" y="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/>`,
};

export default function setup(api) {
  const { $, esc, T } = api;

  const DESKS = findDesks(api.obj);
  const owners = new Map();
  const decos = new Map();
  const sprites = new Map();
  let decorator = null;
  let draft = null;
  let decoWait = null;
  let tasks = [];
  let state = 'idle';
  let errorMsg = '';
  let actor = '';
  let canManage = false;
  let lastPoll = 0;
  let loading = null;
  let prevSnap = null;
  let snapFor = '';
  let scope = 'mine';
  let statusFilter = 'open';
  let query = '';
  let completing = null;
  let busy = new Set();
  let panelName = '';
  let card = null;
  let working = null;
  let updates = [];
  const taskKey = () => 'work-task:' + (api.account?.org || '');
  const quiet = new Map();
  const widths = new Map();

  function readJSON(k) {
    try { return JSON.parse(api.store.get(k) || 'null'); } catch { return null; }
  }
  const writeJSON = (k, v) => api.store.set(k, JSON.stringify(v));

  const who = () => actor || String(api.account?.name || '').trim().toLowerCase();
  const people = () => (api.me ? [api.me, ...api.others.values()] : [...api.others.values()]);
  const deskOf = (o) => (o?.ex?.desk ? DESKS.find((d) => d.x === o.ex.desk.x && d.y === o.ex.desk.y) || null : null);
  const occupant = (d) => people().find((o) => o.ex?.desk && o.ex.desk.x === d.x && o.ex.desk.y === d.y) || null;
  const zoneName = (d) => api.ZONES[api.zoneAt(d.x, d.y)]?.name || 'Office';
  const myDesk = () => deskOf(api.me);
  const atDesk = () => { const d = myDesk(); return !!d && api.me && api.me.x === d.x && api.me.y === d.y; };
  const panelOpen = () => panelName === 'tasks' && !$('#app')?.classList.contains('collapsed') && !ui.panel.hidden;

  const ui = api.addPanel({ id: 'tasks', iconHtml: svg(P.tasks, 18), tip: 'Tasks', html: html`
    <div class="work-desk" data-w="desk"></div>
    <div class="work-bar">
      <div class="work-seg" role="tablist">
        <button type="button" data-scope="mine" class="on">Mine</button>
        <button type="button" data-scope="all">All</button>
      </div>
      <select class="input work-status" data-w="status" aria-label="Status">
        <option value="open">Open</option>
        <option value="any">Any status</option>
        <option value="idle">To do</option>
        <option value="active">In progress</option>
        <option value="done">Done</option>
      </select>
      <button type="button" class="icon-btn" data-w="refresh" data-tip="Refresh">${svg(P.refresh, 15)}</button>
    </div>
    <label class="search work-q"><input data-w="q" placeholder="Filter tasks" autocomplete="off"></label>
    <div class="work-now" data-w="now" hidden></div>
    <div class="work-updates" data-w="updates" hidden></div>
    <p class="work-msg" data-w="msg" hidden></p>
    <ul class="work-list" data-w="list"></ul>
    <div class="work-foot" data-w="foot"></div>` });
  const q = (k) => ui.panel.querySelector(`[data-w="${k}"]`);

  ui.panel.addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b) return;
    if (b.dataset.scope) {
      scope = b.dataset.scope;
      for (const s of ui.panel.querySelectorAll('[data-scope]')) s.classList.toggle('on', s === b);
      return renderList();
    }
    if (b.dataset.w === 'refresh') return load(true);
    const act = b.dataset.act;
    const id = b.closest('[data-id]')?.dataset.id;
    if (act === 'sit') return sitAtDesk();
    if (act === 'release') return api.send({ t: 'work-release' });
    if (act === 'find') return claimFree();
    if (act === 'stop-working') return setWorking(null);
    if (!id) return;
    const t = tasks.find((x) => x.id === id);
    if (!t) return;
    if (act === 'work') return setWorking(working?.id === id ? null : t);
    if (act === 'start' || act === 'stop') return mutate(t, act);
    if (act === 'complete') { completing = completing === id ? null : id; return renderList(); }
    if (act === 'cancel') { completing = null; return renderList(); }
    if (act === 'done') return mutate(t, 'complete', { completionNote: String(ui.panel.querySelector(`[data-id="${id}"] .work-note`)?.value || '').slice(0, 4000) });
  });
  q('status').onchange = (e) => { statusFilter = e.target.value; renderList(); };
  q('q').oninput = (e) => { query = e.target.value; renderList(); };
  q('q').onkeydown = (e) => { if (e.key === 'Escape') e.target.blur(); };

  document.addEventListener('office:panel', (e) => {
    panelName = e.detail;
    if (panelName === 'tasks') {
      ui.button.querySelector('.badge')?.remove();
      render();
      if (Date.now() - lastPoll > 30e3) load();
    }
  });

  async function call(path, init) {
    let res;
    try {
      res = await api.forkmesh(path, init);
    } catch {
      const err = new Error('Sign in with ForkMesh to see your org’s tasks.');
      err.signedOut = true;
      throw err;
    }
    if (!res || typeof res.json !== 'function') {
      const err = new Error('Sign in with ForkMesh to see your org’s tasks.');
      err.signedOut = true;
      throw err;
    }
    let json = null;
    try { json = await res.json(); } catch {}
    if (!res.ok) {
      const err = new Error(apiError(res.status, json));
      err.status = res.status;
      err.signedOut = res.status === 401;
      throw err;
    }
    return json;
  }

  function load(manual) {
    if (loading) return loading;
    if (!api.me) return null;
    if (!api.tasksEnabled) { state = 'signedout'; render(); return null; }
    if (manual) { state = state === 'ok' ? 'ok' : 'loading'; render(); }
    loading = (async () => {
      try {
        const all = [];
        let offset = 0, pages = 0, first = null;
        while (offset !== null && pages < 8) {
          const json = await call(`/api/tasks?offset=${offset}`);
          const page = parseTaskPage(json);
          if (!first) { first = page; canManage = !!json?.canManage; }
          all.push(...page.tasks);
          offset = page.nextOffset;
          pages++;
        }
        const now = Date.now();
        for (const t of all) t.fetchedAt = now;
        actor = first?.actor || actor;
        tasks = all;
        state = 'ok';
        errorMsg = '';
        await announce(all);
        if (working && !all.some((t) => t.id === working.id && t.status !== 'done')) setWorking(null);
      } catch (err) {
        state = err.signedOut ? 'signedout' : 'error';
        errorMsg = err.message || 'Could not load tasks.';
      } finally {
        loading = null;
        lastPoll = Date.now();
        render();
      }
    })();
    return loading;
  }

  async function announce(list) {
    const me = who();
    if (!me) return;
    const key = 'work-snap:' + me;
    if (snapFor !== me) { prevSnap = readJSON(key); snapFor = me; }
    const events = diffTasks(prevSnap, list, me);
    prevSnap = snapshot(list);
    writeJSON(key, prevSnap);
    const now = Date.now();
    for (const [id, until] of quiet) if (until < now) quiet.delete(id);
    const notes = [];
    let lookups = 0;
    for (const ev of events) {
      const t = ev.task;
      if (ev.kind === 'assigned') notes.push(['New task for you', t.title, t.id]);
      else if (ev.kind === 'status') {
        if (!quiet.has(t.id)) notes.push([`Task ${STATUS_LABEL[ev.to].toLowerCase()}`, t.title, t.id]);
      } else if (ev.kind === 'reply' && lookups < 3) {
        lookups++;
        try {
          const json = await call(`/api/tasks/${encodeURIComponent(t.id)}/replies`);
          const replies = json?.task?.replies || [];
          const hit = findMentions(replies, me, ev.since);
          if (hit.length) notes.push([`@${hit[hit.length - 1].author} mentioned you`, t.title, t.id]);
          else if (ev.involved) {
            const fresh = newReplies(replies, me, ev.since);
            if (fresh.length) notes.push([`New comment from @${fresh[fresh.length - 1].author}`, t.title, t.id]);
          }
        } catch {}
      }
    }
    addUpdates(notes.map(([title, body, id]) => ({ title, body, id })));
  }

  function addUpdates(list) {
    if (!list.length) return;
    updates = [...list.slice().reverse(), ...updates].slice(0, 3);
    renderUpdates();
    if (!panelOpen() && !ui.button.querySelector('.badge')) {
      const dot = document.createElement('b');
      dot.className = 'badge';
      ui.button.append(dot);
    }
  }

  function renderUpdates() {
    const box = q('updates');
    box.hidden = !updates.length;
    box.replaceChildren();
    for (const u of updates) {
      const el = document.createElement('p');
      box.append(el);
      api.setNote(el, {
        title: u.title,
        body: u.body,
        dismiss: true,
        actions: u.id ? [{ label: 'Open task', iconHtml: svg(P.tasks, 13), run: () => openTask(u.id) }] : [],
        onClear: () => { updates = updates.filter((x) => x !== u); renderUpdates(); },
      });
    }
  }

  function say(note) {
    api.setNote(q('msg'), note && { dismiss: true, ...note });
  }

  async function mutate(t, action, body = {}) {
    if (busy.has(t.id)) return;
    busy.add(t.id);
    renderList();
    try {
      const json = await call(`/api/tasks/${encodeURIComponent(t.id)}/${action}`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(body),
      });
      const next = normTask(json?.task);
      if (next) {
        next.fetchedAt = Date.now();
        tasks = tasks.map((x) => (x.id === next.id ? next : x));
        if (prevSnap) { prevSnap = { ...prevSnap, ...snapshot([next]) }; writeJSON('work-snap:' + who(), prevSnap); }
      }
      quiet.set(t.id, Date.now() + 120e3);
      if (action === 'complete') { completing = null; if (working?.id === t.id) setWorking(null); }
      if (action === 'start' && working?.id !== t.id) setWorking(next || t);
      const what = action === 'start' ? 'started' : action === 'stop' ? 'stopped' : 'completed';
      const to = pingTargets(next || t, who());
      if (to.length) api.send({ t: 'work-ping', to, what, task: { id: t.id, title: t.title } });
      say({ title: action === 'start' ? 'Timer started' : action === 'stop' ? 'Timer stopped' : 'Task done, sent to QA', body: t.title, tone: 'ok' });
    } catch (err) {
      say({ title: 'Could not update the task', body: err.message, tone: 'error' });
      if (err.signedOut) state = 'signedout';
    } finally {
      busy.delete(t.id);
      render();
    }
  }

  function setWorking(t) {
    working = t && api.tasksEnabled ? cleanTask(t) || null : null;
    if (api.account?.org) writeJSON(taskKey(), working);
    if (api.me) api.setExtra('task', working);
    render();
  }

  function openTask(id) {
    api.showPanel('tasks', true);
    let t = tasks.find((x) => x.id === id);
    if (t && !filterTasks([t], { scope, status: statusFilter, q: query }, who()).length) {
      scope = 'all'; statusFilter = 'any'; query = '';
      q('status').value = 'any';
      q('q').value = '';
      for (const s of ui.panel.querySelectorAll('[data-scope]')) s.classList.toggle('on', s.dataset.scope === 'all');
      renderList();
    }
    const li = ui.panel.querySelector(`[data-id="${CSS.escape(id)}"]`);
    if (!li) return;
    li.scrollIntoView({ block: 'nearest' });
    li.classList.remove('flash');
    void li.offsetWidth;
    li.classList.add('flash');
  }

  function sitAtDesk() {
    const d = myDesk();
    if (!d) return say({ title: 'You have no desk yet', body: 'Click a monitor to claim one.' });
    api.walkToTile(d.x, d.y);
  }

  function claimFree() {
    const taken = new Set([...owners.keys()]);
    const d = DESKS.filter((x) => !taken.has(deskKey(x.x, x.y))).sort((a, b) => dist(a) - dist(b))[0];
    if (!d) return say({ title: 'Every desk is taken', body: 'Ask someone to release theirs.', tone: 'error' });
    say(null);
    api.send({ t: 'work-claim', x: d.x, y: d.y });
    api.walkToTile(d.x, d.y);
  }
  const dist = (d) => (api.me ? Math.abs(api.me.x - d.x) + Math.abs(api.me.y - d.y) : 0);

  function render() {
    renderDesk();
    renderNow();
    renderList();
  }

  function renderDesk() {
    const d = myDesk();
    setHtml(q('desk'), d
      ? html`<span class="work-desk-ic">${svg(P.monitor, 15)}</span><span class="work-desk-t"><b>Your desk</b><small>${(zoneName(d))}</small></span>
         <button type="button" class="btn btn-sm" data-act="sit">Sit</button><button type="button" class="icon-btn" data-act="release" data-tip="Release desk">${svg(P.x, 14)}</button>`
      : html`<span class="work-desk-ic">${svg(P.monitor, 15)}</span><span class="work-desk-t"><b>No desk yet</b><small>Click a monitor on the map</small></span>
         <button type="button" class="btn btn-sm" data-act="find">Claim a free desk</button>`);
  }

  function renderNow() {
    const el = q('now');
    el.hidden = !working;
    if (working) setHtml(el, html`${svg(P.target, 14)}<span><small>Working on</small><b>${(working.title)}</b></span><button type="button" class="icon-btn" data-act="stop-working" data-tip="Clear">${svg(P.x, 14)}</button>`);
  }

  function renderList() {
    const list = q('list');
    const foot = q('foot');
    if (state === 'signedout' || (!api.account && state !== 'ok') || (api.account && !api.tasksEnabled)) {
      setHtml(list, api.account && !api.tasksEnabled
        ? html`<li class="work-empty">${svg(P.lock, 18)}<b>The task board is not shared with this office</b><span>Desks still work. Task pings come from the board, so they are off here too.</span></li>`
        : html`<li class="work-empty">${svg(P.lock, 18)}<b>Sign in with ForkMesh to see your org’s tasks</b><span>Desks still work without an account.</span></li>`);
      foot.textContent = '';
      return;
    }
    if (state === 'idle' || state === 'loading') {
      setHtml(list, html`<li class="work-empty"><span>Loading tasks…</span></li>`);
      foot.textContent = '';
      return;
    }
    if (state === 'error') {
      setHtml(list, html`<li class="work-empty"><b>Could not load tasks</b><span>${(errorMsg)}</span></li>`);
      foot.textContent = '';
      return;
    }
    const me = who();
    const shown = sortTasks(filterTasks(tasks, { scope, status: statusFilter, q: query }, me));
    const now = Date.now();
    setHtml(list, shown.length
      ? join(shown.slice(0, 100).map((t) => row(t, me, now)))
      : html`<li class="work-empty"><span>${scope === 'mine' ? 'Nothing assigned to you here.' : 'No tasks match.'}</span></li>`);
    foot.textContent = `${shown.length} of ${tasks.length} tasks · updated ${new Date(lastPoll).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}`;
  }

  function row(t, me, now) {
    const mine = t.assignee === me;
    const isBusy = busy.has(t.id);
    const acts = [];
    if (t.status !== 'done') {
      if (mine) acts.push(t.status === 'active'
        ? html`<button type="button" class="btn btn-sm" data-act="stop"${isBusy ? ' disabled' : ''}>${svg(P.stop, 13)}Stop</button>`
        : html`<button type="button" class="btn btn-sm" data-act="start"${isBusy ? ' disabled' : ''}>${svg(P.play, 13)}Start</button>`);
      if (mine || canManage) acts.push(html`<button type="button" class="btn btn-sm${completing === t.id ? ' on' : ''}" data-act="complete"${isBusy ? ' disabled' : ''}>${svg(P.check, 13)}Done</button>`);
      acts.push(html`<button type="button" class="btn btn-sm${working?.id === t.id ? ' on' : ''}" data-act="work">${svg(P.target, 13)}${working?.id === t.id ? 'Working on' : 'Work on this'}</button>`);
    }
    const who2 = t.assigneeKind === 'agent' ? 'agent' : t.assignee ? '@' + t.assignee : 'unassigned';
    const time = t.elapsedMs || t.status === 'active' ? ` · ${fmtElapsed(liveElapsed(t, now))}` : '';
    const doneForm = completing === t.id
      ? html`<div class="work-done"><input class="input work-note" maxlength="4000" placeholder="What changed? (optional)"><div class="work-acts"><button type="button" class="btn btn-sm btn-primary" data-act="done"${isBusy ? ' disabled' : ''}>Mark done</button><button type="button" class="btn btn-sm" data-act="cancel">Cancel</button></div></div>`
      : '';
    return html`<li data-id="${(t.id)}" class="work-task${working?.id === t.id ? ' is-working' : ''}">
      <div class="work-meta"><span class="work-chip s-${t.status}">${STATUS_LABEL[t.status]}</span><span class="muted">${(who2)}${(time)}${t.replyCount ? ` · ${t.replyCount} ${t.replyCount === 1 ? 'comment' : 'comments'}` : ''}</span></div>
      <b class="work-title">${(t.title)}</b>
      ${acts.length ? html`<div class="work-acts">${acts}</div>` : ''}${doneForm}</li>`;
  }

  setInterval(() => {
    if (!api.me || !api.tasksEnabled) return;
    const due = pollInterval({ panelOpen: panelOpen(), atDesk: atDesk() });
    if (Date.now() - lastPoll >= due) load();
    else if (panelOpen() && state === 'ok' && !completing && document.activeElement?.closest?.('#panel-tasks') == null) renderList();
  }, 5e3);

  function closeCard() {
    card?.el.remove();
    card = null;
  }

  function openCard(d) {
    closeCard();
    const el = document.createElement('div');
    el.className = 'work-card';
    el.setAttribute('role', 'dialog');
    if ($('#dock')) $('#dock').prepend(el);
    else ($('#stage') || document.body).append(el);
    card = { el, d, note: null };
    el.addEventListener('click', onCardClick);
    renderCard();
  }

  function renderCard() {
    if (!card) return;
    const { d, el } = card;
    const owner = owners.get(deskKey(d.x, d.y));
    const o = occupant(d);
    const mineNow = deskOf(api.me) === d;
    const task = o?.ex?.task;
    let body = '';
    let acts = '';
    if (!owner) {
      body = html`<p class="muted">A free desk in ${(zoneName(d))}. Claim it and it stays yours whenever you come back.</p>`;
      acts = html`<button type="button" class="btn btn-sm btn-primary" data-c="claim">${myDesk() ? 'Move here' : 'Claim this desk'}</button>`;
    } else if (mineNow) {
      body = html`<p class="muted">${(zoneName(d))}${working ? '' : ' · not working on a task'}</p>${working ? html`<p class="work-card-task">${svg(P.target, 13)}${(working.title)}</p>` : ''}`;
      acts = html`<button type="button" class="btn btn-sm btn-primary" data-c="sit">Sit down</button><button type="button" class="btn btn-sm" data-c="deco">${svg(P.brush, 13)}Decorate</button><button type="button" class="btn btn-sm" data-c="tasks">Tasks</button><button type="button" class="btn btn-sm" data-c="release">Release</button>`;
    } else {
      body = html`<p class="muted">${(zoneName(d))} · ${o ? 'in the office' : 'not here right now'}</p>${task ? html`<p class="work-card-task">${svg(P.target, 13)}${(task.title)}</p>` : ''}`;
      if (o && o !== api.me) acts = html`<button type="button" class="btn btn-sm" data-c="dm">${svg(P.chat, 13)}Message</button><button type="button" class="btn btn-sm" data-c="walk">${svg(P.walk, 13)}Walk over</button>`;
    }
    setHtml(el, html`<div class="work-card-h"><span class="work-desk-ic">${svg(P.monitor, 15)}</span><b>${owner ? (mineNow ? 'Your desk' : (owner) + '’s desk') : 'Free desk'}</b><button type="button" class="icon-btn" data-c="close" aria-label="Close">${svg(P.x, 14)}</button></div>${body}${acts ? html`<div class="work-acts">${acts}</div>` : ''}<p class="work-card-note" hidden></p>`);
    api.setNote(el.querySelector('.work-card-note'), card.note);
  }

  function deskSay(note) {
    if (!card) return say(note);
    card.note = note;
    renderCard();
  }

  function onCardClick(e) {
    const b = e.target.closest('button[data-c]');
    if (!b || !card) return;
    const { d } = card;
    const c = b.dataset.c;
    if (c === 'close') return closeCard();
    if (c === 'claim') { card.note = null; renderCard(); api.send({ t: 'work-claim', x: d.x, y: d.y }); api.walkToTile(d.x, d.y); return; }
    if (c === 'sit') { api.walkToTile(d.x, d.y); return closeCard(); }
    if (c === 'release') { api.send({ t: 'work-release' }); return; }
    if (c === 'tasks') { api.showPanel('tasks', true); return closeCard(); }
    if (c === 'deco') { closeCard(); return openDeco(); }
    const o = occupant(d);
    if (c === 'dm' && o) { document.dispatchEvent(new CustomEvent('office:dm', { detail: { id: o.id } })); return closeCard(); }
    if (c === 'walk' && o) { api.walkTo(o.id); return closeCard(); }
  }

  addEventListener('keydown', (e) => { if (e.key === 'Escape') closeCard(); });

  api.onObject('M', (tx, ty) => {
    const d = deskAt(DESKS, tx, ty);
    if (!d) return api.notice('Reception desk', 'This monitor is shared. Desks with a chair can be claimed.');
    openCard(d);
  });

  api.onTick(() => {
    if (card && api.me && Math.abs(api.me.x - card.d.mx) + Math.abs(api.me.y - card.d.my) > 3) closeCard();
  });

  function setOwners(list) {
    owners.clear();
    decos.clear();
    for (const d of list || []) {
      if (!d || !Number.isInteger(d.x) || !Number.isInteger(d.y)) continue;
      owners.set(deskKey(d.x, d.y), String(d.name || ''));
      const deco = cleanDeco(d.deco);
      if (deco && !decoEmpty(deco)) decos.set(deskKey(d.x, d.y), deco);
    }
    renderDesk();
    renderCard();
  }

  function spriteOf(deco) {
    const key = JSON.stringify(deco);
    let cv = sprites.get(key);
    if (!cv) {
      if (sprites.size > 200) sprites.clear();
      cv = decoSprite(deco);
      sprites.set(key, cv);
    }
    return cv;
  }

  function settleDeco(error) {
    if (!decoWait) return;
    clearTimeout(decoWait.timer);
    const done = decoWait;
    decoWait = null;
    done.resolve(error);
  }

  function openDeco() {
    const d = myDesk();
    if (!d) return api.notice('You have no desk yet', 'Claim a desk, then decorate it.');
    if (decorator) return;
    api.walkToTile(d.x, d.y);
    const key = deskKey(d.x, d.y);
    decorator = openDecorator(api, {
      desk: d,
      deco: decos.get(key),
      onDraft: (v) => { draft = { key, deco: v }; },
      onSave: (v) => new Promise((resolve) => {
        settleDeco('Another save replaced this one.');
        decoWait = { resolve, timer: setTimeout(() => settleDeco('The office did not answer. Try again.'), 5000) };
        api.send({ t: 'work-deco', deco: v });
      }),
      onClose: () => {
        decorator = null;
        draft = null;
      },
    });
  }

  api.onDrawUnder((ctx) => {
    if (!decos.size && !draft) return;
    for (const d of DESKS) {
      const key = deskKey(d.x, d.y);
      const deco = draft?.key === key ? draft.deco : decos.get(key);
      if (deco && !decoEmpty(deco)) ctx.drawImage(spriteOf(deco), d.mx * T, d.my * T - HEAD);
    }
  });

  api.onInit((m) => {
    setOwners(m.workDesks);
    try { api.store.set('work-task', 'null'); } catch {}
    working = api.tasksEnabled && api.account?.org ? cleanTask(readJSON(taskKey()) || {}) || null : null;
    if (working && !api.me.ex?.task) api.setExtra('task', working);
    else if (!working && api.me.ex?.task) api.setExtra('task', null);
    render();
    if (Date.now() - lastPoll > 30e3) setTimeout(() => load(), 1500);
  });
  api.onMessage('work-desks', (m) => setOwners(m.desks));
  api.onMessage('work-err', (m) => {
    if (m.reason === 'taken') deskSay({ title: 'That desk is taken', body: m.by ? `${m.by} already claimed it.` : 'Pick another one.', tone: 'error' });
    else if (m.reason === 'deco' || m.reason === 'no_desk') settleDeco(m.reason === 'deco' ? 'Those decorations could not be saved.' : 'Claim a desk first.');
    else deskSay({ title: 'Could not claim that desk', tone: 'error' });
  });
  api.onMessage('work-deco', () => {
    if (!decoWait) return;
    settleDeco('');
    api.hudNote({ title: 'Desk saved', tone: 'ok' });
  });
  api.onMessage('work-ping', (m) => {
    if (m.task?.id) quiet.set(m.task.id, Date.now() + 120e3);
    const t = m.task?.title;
    addUpdates([{ title: `${m.name || 'Someone'} ${m.what || 'updated'} a task`, body: t || '', id: t ? m.task.id : '' }]);
    load();
  });
  api.onExtra((o, k) => {
    if (k === 'desk' || k === 'task') renderCard();
    if (o === api.me && k === 'desk') {
      renderDesk();
      const d = myDesk();
      if (d && card && card.d !== d) closeCard();
      if (decorator && (!d || deskKey(d.x, d.y) !== draft?.key)) decorator.close();
    }
  });

  api.paletteItems(() => {
    const items = [{ ic: svg(P.tasks, 14), label: 'Task board', sub: 'Tasks', run: () => api.showPanel('tasks', true) }];
    if (myDesk()) items.unshift({ ic: svg(P.monitor, 14), label: 'Sit at my desk', sub: zoneName(myDesk()), run: sitAtDesk }, { ic: svg(P.brush, 14), label: 'Decorate my desk', sub: 'Desk, chair, decorations', run: openDeco });
    else items.push({ ic: svg(P.monitor, 14), label: 'Claim a free desk', sub: 'Desks', run: claimFree });
    return items;
  });

  api.personBadge((o) => (o.ex?.task ? html` <span class="work-badge" title="Working on: ${(o.ex.task.title)}">${svg(P.target, 11)}${(short(o.ex.task.title, 18))}</span>` : ''));
  api.personActions((o) => {
    const d = deskOf(o);
    return d ? [{ icon: svg(P.monitor, 14), tip: 'Go to their desk', run: () => api.walkToTile(d.x, d.y) }] : [];
  });

  function measure(ctx, font, text) {
    const k = font + '|' + text;
    let w = widths.get(k);
    if (w === undefined) {
      ctx.font = font;
      w = ctx.measureText(text).width;
      if (widths.size > 400) widths.clear();
      widths.set(k, w);
    }
    return w;
  }

  function pill(ctx, x, bottom, text, font, fg, bg, edge, glyph) {
    const tw = measure(ctx, font, text);
    const gw = glyph ? 12 : 0;
    const w = Math.round(tw + gw + 12), h = 17;
    const bx = Math.round(x - w / 2), by = Math.round(bottom - h);
    ctx.fillStyle = bg;
    ctx.beginPath();
    ctx.roundRect(bx, by, w, h, 6);
    ctx.fill();
    if (edge) { ctx.strokeStyle = edge; ctx.lineWidth = 1; ctx.stroke(); }
    if (glyph) {
      ctx.strokeStyle = fg;
      ctx.lineWidth = 1.4;
      ctx.beginPath();
      ctx.arc(bx + 6 + 4, by + h / 2, 3.5, 0, Math.PI * 2);
      ctx.stroke();
      ctx.fillStyle = fg;
      ctx.fillRect(bx + 9, by + h / 2 - 1, 2, 2);
    }
    ctx.font = font;
    ctx.fillStyle = fg;
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    ctx.fillText(text, bx + 6 + gw, by + h / 2 + 0.5);
    return by;
  }

  api.onDrawWorld((ctx, now) => {
    if (!owners.size) return;
    const blink = ((now / 530) | 0) % 2;
    const plates = [];
    for (const d of DESKS) {
      const name = owners.get(deskKey(d.x, d.y));
      if (name === undefined) continue;
      const o = occupant(d);
      const task = o?.ex?.task;
      const wx = d.mx * T, wy = d.my * T;
      if (d.monitor) {
        if (task) {
          ctx.fillStyle = '#1d2030';
          ctx.fillRect(wx + 7, wy - 4, 18, 13);
          ctx.fillStyle = '#9fd8fb';
          ctx.fillRect(wx + 9, wy - 2, 9, 1);
          ctx.fillStyle = '#8b93ff';
          ctx.fillRect(wx + 11, wy + 1, 10, 1);
          ctx.fillStyle = '#9fd8fb';
          ctx.fillRect(wx + 11, wy + 4, 6, 1);
          if (blink) { ctx.fillStyle = '#ffffff'; ctx.fillRect(wx + 18, wy + 3, 2, 3); }
        } else if (!o) {
          ctx.fillStyle = 'rgba(29,32,48,.55)';
          ctx.fillRect(wx + 7, wy - 4, 18, 13);
        }
      }
      plates.push({ d, name, task, online: !!o, mine: o === api.me });
    }
    const cam = api.camera;
    if (cam.zoom < 0.9) return;
    const dpr = devicePixelRatio || 1;
    ctx.save();
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    for (const p of plates) {
      const s = api.worldToScreen((p.d.mx + 0.5) * T, p.d.my * T - 7);
      const label = p.mine ? 'Your desk' : short(p.name, 16);
      const top = pill(ctx, s.x, s.y, label, '600 10.5px -apple-system, system-ui, sans-serif', p.online ? '#1d2030' : '#8a8fa0', 'rgba(255,255,255,.94)', '#e6e7ec', false);
      if (p.task && cam.zoom >= 1.25) pill(ctx, s.x, top - 3, short(p.task.title, 22), '500 10px -apple-system, system-ui, sans-serif', '#3b45e0', '#eef0ff', null, false);
    }
    ctx.restore();
  });

  api.onDrawScreen((ctx, o, sx, above) => {
    const t = o.ex?.task;
    if (!t) return above;
    ctx.save();
    const top = pill(ctx, sx, above, short(t.title, 22), '500 10.5px -apple-system, system-ui, sans-serif', '#3b45e0', '#eef0ff', 'rgba(59,69,224,.25)', true);
    ctx.restore();
    ctx.textAlign = 'center';
    return top - 4;
  });

  render();
}
