import { html, setHtml, trusted } from '../html.js';
import {
  POLL_MS, STATES, MODELS, botsFromPayload, ownerKeys, personKeys, isMine, findOwner, diffStates,
  cleanRelay, applyRelay, relTime, filterBots, counts, stationLayout, wrapText, stepBot,
} from './agents/model.js';
import { AGENT_STATIONS, CONSOLE } from '../map.js';
import { overlay } from './play/ui.js';
import { bubbleLines, paintBubble } from './agents/bubble.js';
import { botSprite, botDataUrl, drawLight, BOT_W, BOT_H, BOT_FOOT, LIGHT_X, LIGHT_Y } from './agents/sprite.js';

const TASK_PAGES = 4;

const svg = (d, s = 16) => html`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${d}</svg>`;
const IC = {
  bot: html`<path d="M12 8V4H8"/><rect width="16" height="12" x="4" y="8" rx="2"/><path d="M2 14h2"/><path d="M20 14h2"/><path d="M15 13v2"/><path d="M9 13v2"/>`,
  refresh: html`<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>`,
  x: html`<path d="M18 6 6 18"/><path d="m6 6 12 12"/>`,
  walk: html`<path d="M13 4a1 1 0 1 0 2 0 1 1 0 1 0-2 0"/><path d="m9 20 3-6 3 3v4"/><path d="m6 8 4-2 3 3 3 1"/><path d="M12 14 10 9"/>`,
  ext: html`<path d="M15 3h6v6"/><path d="M10 14 21 3"/><path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/>`,
};
const APP = '';

export default function setup(api) {
  const { esc, T } = api;

  let bots = [];
  let status = 'idle';
  let errMsg = '';
  let lastFetch = 0;
  let lastKeys = '';
  let loading = null;
  let confirmTimer = null;
  let filter = api.store.get('agents-filter') === 'all' ? 'all' : 'mine';
  let hover = null;
  let pinned = null;
  let downBot = null;
  let lastT = 0;
  const prev = new Map();
  const pos = new Map();
  const phase = new Map();

  const signed = () => !!api.account && !!api.tasksEnabled;
  const keys = () => ownerKeys(api.account);
  const byId = (id) => bots.find((b) => b.id === id);
  const others = () => [...api.others.values()];
  const ownerName = (b) => {
    const o = findOwner(b, api.me, others(), keys());
    if (o === api.me) return 'You';
    return o?.name || b.owner || 'Unknown';
  };

  const { panel, button } = api.addPanel({
    id: 'agents',
    iconHtml: svg(IC.bot, 18),
    tip: 'Agents',
    html: html`<div class="ag-head"><h3>Agents</h3><button class="icon-btn" data-ag="refresh" data-tip="Refresh">${svg(IC.refresh, 14)}</button></div>
      <div class="ag-seg" role="tablist"><button data-f="mine">Mine</button><button data-f="all">All</button></div>
      <ul class="ag-list"></ul>
      <div class="ag-foot"><p class="ag-note" hidden></p><button class="btn btn-sm btn-block" data-ag="summon">${svg(IC.walk, 14)}Walk to my agents</button><small class="muted ag-upd"></small></div>`,
  });
  button.classList.add('ag-rail');
  const list = panel.querySelector('.ag-list');
  const upd = panel.querySelector('.ag-upd');
  const agNote = panel.querySelector('.ag-note');
  const panelShown = () => panel.getClientRects().length > 0;
  const tell = (title, body, tone) => (panelShown() ? api.setNote(agNote, { title, body, tone }) : api.hudNote({ key: 'agents', title, body, tone }));
  document.addEventListener('office:panel', (e) => { if (e.detail === 'agents') button.querySelector('.badge')?.remove(); });

  const card = document.createElement('div');
  card.className = 'ag-card';
  card.hidden = true;
  document.querySelector('#stage')?.append(card);

  function renderPanel() {
    for (const b of panel.querySelectorAll('.ag-seg button')) b.classList.toggle('on', b.dataset.f === filter);
    const k = keys();
    const shown = filterBots(bots, filter, k);
    panel.querySelector('[data-ag="summon"]').disabled = !signed();
    upd.textContent = lastFetch && status === 'ok' ? `Updated ${relTime(lastFetch)}` : '';
    if (!signed()) {
      setHtml(list, html`<li class="ag-empty">${svg(IC.bot, 22)}<b>${api.account ? 'Agents are not shown in this office' : 'Sign in to see agents'}</b><span>${api.account ? 'Agent sessions come from the ForkMesh task board, which is not shared with this office.' : 'Desktop and runner agent sessions from your ForkMesh account and your organization show up here and next to their owner’s desk.'}</span></li>`);
      return;
    }
    if (status === 'error' && !bots.length) {
      setHtml(list, html`<li class="ag-empty"><b>Couldn't load agents</b><span>${(errMsg || 'ForkMesh did not answer.')}</span><button class="btn btn-sm" data-ag="refresh">Try again</button></li>`);
      return;
    }
    if (!shown.length) {
      setHtml(list, status === 'loading' || status === 'idle'
        ? html`<li class="ag-empty"><span>Loading agent sessions…</span></li>`
        : html`<li class="ag-empty">${svg(IC.bot, 22)}<b>${filter === 'mine' ? 'No agents of yours right now' : 'No agent sessions right now'}</b><span>Start one from the ForkMesh desktop app and it walks over to your desk.</span></li>`);
      return;
    }
    const now = Date.now();
    setHtml(list, html`${shown.map((b) => {
      const s = STATES[b.state];
      const meta = [b.modelLabel, b.effort && `${b.effort} effort`, relTime(b.startedAt, now)].filter(Boolean).join(' · ');
      return html`<li data-id="${(b.id)}" class="${pinned === b.id ? 'on' : ''}"><img class="ag-spr" alt="" src="${botDataUrl(b.modelKey)}">
        <div class="ag-main"><b>${(b.label)}</b><small>${(b.title)}</small><small class="ag-meta">${(meta)}${filter === 'all' ? ` · ${(ownerName(b))}` : ''}</small></div>
        <span class="ag-pill${s.pulse ? ' pulse' : ''}" style="--c:${s.color}">${(s.label)}</span></li>`;
    })}`);
  }

  function renderCard() {
    const id = pinned || hover;
    const b = id && byId(id);
    if (!b) { card.hidden = true; card.dataset.id = ''; return; }
    const s = STATES[b.state];
    const p = posOf(b);
    const link = b.repository && /^[\w.-]+\/[\w.-]+$/.test(b.repository) ? `${APP}/${b.repository}/agents/${encodeURIComponent(b.id)}` : '';
    card.dataset.id = b.id;
    card.classList.toggle('pinned', !!pinned);
    setHtml(card, html`<div class="ag-card-h"><img class="ag-spr" alt="" src="${botDataUrl(b.modelKey)}"><div><b>${(b.label)}</b><span class="ag-pill${s.pulse ? ' pulse' : ''}" style="--c:${s.color}">${(s.label)}</span></div>${pinned ? html`<button class="icon-btn" data-ag="close" aria-label="Close">${svg(IC.x, 14)}</button>` : ''}</div>
      <p class="ag-task">${(b.title)}</p>
      <dl><dt>Model</dt><dd>${(b.modelLabel)}${b.model && b.model.toLowerCase() !== b.modelLabel.toLowerCase() ? html` <span class="muted">${(b.model)}</span>` : ''}</dd>
      ${b.effort ? html`<dt>Effort</dt><dd>${(b.effort)}</dd>` : ''}
      <dt>Started</dt><dd>${(b.startedAt ? `${new Date(b.startedAt).toLocaleString([], { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' })} · ${relTime(b.startedAt)}` : '—')}</dd>
      <dt>Owner</dt><dd>${(ownerName(b))}</dd>
      ${b.repository ? html`<dt>Repo</dt><dd>${(b.repository)}</dd>` : ''}</dl>
      ${pinned ? html`<div class="ag-card-a">${p ? html`<button class="btn btn-sm" data-ag="walk">${svg(IC.walk, 14)}Walk here</button>` : ''}${link ? html`<a class="btn btn-sm" href="${(link)}" target="_blank" rel="noopener">${svg(IC.ext, 14)}Open</a>` : ''}</div>` : ''}`);
    card.hidden = false;
    placeCard();
  }

  function placeCard() {
    const b = card.dataset.id && byId(card.dataset.id);
    const p = b && pos.get(b.id);
    const stage = card.parentElement;
    if (!b || !stage) return;
    const sw = stage.clientWidth, sh = stage.clientHeight;
    const cw = card.offsetWidth, ch = card.offsetHeight;
    if (!p || sw < 480) {
      card.classList.add('docked');
      card.style.left = '';
      card.style.top = '';
      return;
    }
    card.classList.remove('docked');
    const z = api.zoom;
    const s = api.worldToScreen(p.x * T, p.y * T);
    let left = s.x - cw / 2;
    let top = s.y - BOT_H * z - ch - 10;
    if (top < 8) top = s.y + 10;
    left = Math.max(8, Math.min(sw - cw - 8, left));
    top = Math.max(8, Math.min(sh - ch - 8, top));
    card.style.left = `${Math.round(left)}px`;
    card.style.top = `${Math.round(top)}px`;
  }

  function pin(id) {
    pinned = id;
    renderCard();
    renderPanel();
  }

  function posOf(b) {
    return pos.get(b.id) || stationLayout(bots, AGENT_STATIONS).get(b.id);
  }

  function focusBot(id) {
    const b = byId(id);
    if (!b) return;
    const p = posOf(b);
    if (!p) {
      tell(`${b.label} has no station`, `All ${AGENT_STATIONS.length} stations in the Agent Room are taken.`);
      pin(id);
      return;
    }
    api.walkToTile(Math.floor(p.x), Math.floor(p.y));
    pin(id);
  }

  function summon() {
    if (!api.me) return;
    if (!signed()) return tell('Sign in to see your agents');
    const mine = bots.filter((b) => isMine(b, keys()));
    if (!mine.length) return tell('No agents of yours right now', 'Start one from the ForkMesh desktop app.');
    const d = api.me.ex?.desk;
    if (d && Number.isInteger(d.x) && Number.isInteger(d.y)) {
      if (api.me.x === d.x && api.me.y === d.y) return tell('Your agents are at your desk with you');
      api.walkToTile(d.x, d.y);
      return tell(`Walking to your desk`, `${mine.length} agent${mine.length === 1 ? '' : 's'} waiting there.`);
    }
    tell('Your agents are right beside you', 'Claim a desk and they will wait for you there.');
  }

  panel.addEventListener('click', (e) => {
    const f = e.target.closest('[data-f]');
    if (f) {
      filter = f.dataset.f;
      api.store.set('agents-filter', filter);
      return renderPanel();
    }
    const a = e.target.closest('[data-ag]')?.dataset.ag;
    if (a === 'refresh') return load(true);
    if (a === 'summon') return summon();
    const li = e.target.closest('li[data-id]');
    if (li) focusBot(li.dataset.id);
  });
  card.addEventListener('click', (e) => {
    const a = e.target.closest('[data-ag]')?.dataset.ag;
    if (a === 'close') { pinned = null; renderCard(); renderPanel(); }
    if (a === 'walk' && card.dataset.id) focusBot(card.dataset.id);
  });
  card.addEventListener('pointerdown', (e) => e.stopPropagation());

  async function forkmesh(path) {
    const ctl = typeof AbortController === 'function' ? new AbortController() : null;
    const t = ctl && setTimeout(() => ctl.abort(), 10000);
    try {
      return await api.forkmesh(path, { method: 'GET', cache: 'no-store', signal: ctl?.signal, headers: { accept: 'application/json' } });
    } finally {
      clearTimeout(t);
    }
  }

  async function load() {
    if (loading) return loading;
    if (!signed()) {
      bots = [];
      status = 'idle';
      prev.clear();
      renderPanel();
      renderCard();
      return;
    }
    if (!bots.length) status = 'loading';
    renderPanel();
    loading = (async () => {
      try {
        const r = await forkmesh('/api/tasks');
        const body = await r.json().catch(() => null);
        if (!r.ok) {
          if (r.status === 404 || body?.error === 'no_organization' || body?.error === 'organization_required') {
            apply(botsFromPayload([]));
            return;
          }
          throw new Error(body?.error ? String(body.error).replace(/_/g, ' ') : `HTTP ${r.status}`);
        }
        const tasks = Array.isArray(body?.tasks) ? body.tasks.slice() : [];
        let page = body;
        for (let i = 1; i < TASK_PAGES && page?.hasMore && Number.isInteger(page.nextOffset); i++) {
          const more = await forkmesh(`/api/tasks?offset=${page.nextOffset}`);
          page = more.ok ? await more.json().catch(() => null) : null;
          if (Array.isArray(page?.tasks)) tasks.push(...page.tasks);
        }
        apply(botsFromPayload({ tasks, agentRuns: body?.agentRuns }));
      } catch (err) {
        status = 'error';
        errMsg = err?.name === 'AbortError' ? 'ForkMesh took too long to answer.' : String(err?.message || err).slice(0, 120);
      } finally {
        lastFetch = Date.now();
        loading = null;
        renderPanel();
        renderCard();
      }
    })();
    return loading;
  }

  function apply(next) {
    const first = !prev.size && !bots.length;
    const changes = first ? [] : diffStates(prev, next);
    bots = next;
    status = 'ok';
    prev.clear();
    for (const b of bots) prev.set(b.id, b.state);
    if (pinned && !byId(pinned)) pinned = null;
    if (hover && !byId(hover)) hover = null;
    if (!changes.length) return;
    api.send({ t: 'agents', ch: changes.map((c) => ({ id: c.id, st: c.to, at: byId(c.id).at || Date.now() })) });
    notify(changes.map((c) => c.id));
  }

  function notify(ids) {
    const k = keys();
    const mineChanged = ids.some((id) => { const b = byId(id); return b && isMine(b, k) && b.state !== 'queued'; });
    if (!mineChanged || panelShown() || button.querySelector('.badge')) return;
    const dot = document.createElement('b');
    dot.className = 'badge';
    button.append(dot);
  }

  api.onMessage('agents', (m) => {
    const from = api.others.get(m.f);
    const theirs = from ? personKeys(from) : [];
    const ch = cleanRelay(m.ch).filter((c) => { const b = byId(c.id); return b && theirs.includes(b.owner); });
    const changed = applyRelay(bots, ch);
    if (!changed.length) return;
    for (const id of changed) prev.set(id, byId(id).state);
    notify(changed);
    renderPanel();
    renderCard();
    clearTimeout(confirmTimer);
    confirmTimer = setTimeout(() => { if (Date.now() - lastFetch > 10000) load(); }, 2000 + Math.random() * 6000);
  });

  api.onInit(() => {
    lastKeys = keys().join('|');
    load();
  });

  setInterval(() => {
    if (!api.me || document.hidden) return;
    const k = keys().join('|');
    if (k !== lastKeys) {
      lastKeys = k;
      prev.clear();
      bots = [];
      return load();
    }
    if (Date.now() - lastFetch >= POLL_MS) load();
  }, 5000);

  function worldAt(e) {
    const cv = document.querySelector('#cv');
    const r = cv.getBoundingClientRect();
    const c = api.camera;
    return { x: (e.clientX - r.left - c.ox) / c.zoom / T, y: (e.clientY - r.top - c.oy) / c.zoom / T };
  }
  function hit(e) {
    if (!bots.length) return null;
    const w = worldAt(e);
    let best = null, bd = Infinity;
    for (const [id, p] of pos) {
      const dx = Math.abs(w.x - p.x), dy = p.y + 0.05 - w.y;
      if (dx < 0.34 && dy > 0 && dy < 0.8) {
        const d = dx + Math.abs(dy - 0.4);
        if (d < bd) { bd = d; best = id; }
      }
    }
    return best;
  }

  const stage = document.querySelector('#stage');
  if (stage) {
    stage.addEventListener('pointerdown', (e) => {
      if (!api.me || e.button !== 0 || e.target.id !== 'cv') return;
      const id = hit(e);
      if (!id) return;
      downBot = id;
      e.stopPropagation();
    }, true);
    stage.addEventListener('pointerup', (e) => {
      if (!downBot) return;
      const id = hit(e);
      const was = downBot;
      downBot = null;
      if (id === was) {
        e.stopPropagation();
        pin(pinned === id ? null : id);
      }
    }, true);
    stage.addEventListener('pointermove', (e) => {
      if (e.target.id !== 'cv' || e.buttons) return;
      const id = hit(e);
      if (id !== hover) {
        hover = id;
        e.target.style.cursor = id ? 'help' : '';
        if (!pinned) renderCard();
      }
    });
    stage.addEventListener('pointerleave', () => {
      if (hover) { hover = null; if (!pinned) renderCard(); }
    });
  }

  api.onDrawWorld((ctx, now) => {
    const dt = Math.min(100, lastT ? now - lastT : 16);
    lastT = now;
    if (!bots.length) { pos.clear(); return; }
    const targets = stationLayout(bots, AGENT_STATIONS);
    for (const id of [...pos.keys()]) if (!targets.has(id)) pos.delete(id);
    const k = Math.min(1, dt / 220);
    const order = [];
    for (const b of bots) {
      const t = targets.get(b.id);
      if (!t) continue;
      let p = pos.get(b.id);
      if (!p) {
        p = { x: t.x, y: t.y, born: now };
        pos.set(b.id, p);
      } else if (Math.abs(t.x - p.x) > 12 || Math.abs(t.y - p.y) > 12) {
        p.x = t.x; p.y = t.y; p.born = now;
      } else {
        p.x += (t.x - p.x) * k;
        p.y += (t.y - p.y) * k;
      }
      if (!phase.has(b.id)) phase.set(b.id, [...b.id].reduce((a, c) => a + c.charCodeAt(0), 0) % 628 / 100);
      order.push(b);
    }
    order.sort((a, b) => pos.get(a.id).y - pos.get(b.id).y);
    const sel = pinned || hover;
    for (const b of order) {
      const p = pos.get(b.id);
      const s = STATES[b.state];
      const ph = phase.get(b.id);
      const drop = Math.max(0, 1 - (now - p.born) / 600);
      const lift = Math.round(drop * drop * drop * 40);
      const bob = Math.round(Math.sin(now / (s.live ? 380 : 700) + ph) * (s.live ? 1.5 : 1));
      const blink = ((now / 1000 + ph) % 4.2) < 0.14;
      const x = Math.round(p.x * T - BOT_W / 2), y = Math.round(p.y * T - BOT_FOOT + bob - lift);
      ctx.fillStyle = 'rgba(0,0,0,.14)';
      ctx.beginPath();
      ctx.ellipse(p.x * T, p.y * T, 6 - Math.abs(bob) * 0.5, 2.2, 0, 0, Math.PI * 2);
      ctx.fill();
      if (sel === b.id) {
        ctx.strokeStyle = 'rgba(59,69,224,.9)';
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        ctx.ellipse(p.x * T, p.y * T, 9, 3.5, 0, 0, Math.PI * 2);
        ctx.stroke();
      }
      ctx.globalAlpha = b.state === 'stopped' ? 0.72 : 1;
      ctx.drawImage(botSprite(b.modelKey, blink), x, y);
      ctx.globalAlpha = 1;
      let glow = 0;
      if (s.pulse) glow = 0.2 + 0.35 * (0.5 + 0.5 * Math.sin(now / 170 + ph));
      else if (b.state === 'waiting') glow = ((now / 600 + ph) % 2) < 1 ? 0.35 : 0;
      else if (b.state === 'failed') glow = 0.25;
      drawLight(ctx, x + LIGHT_X, y + LIGHT_Y, s.color, glow);
    }
    for (const b of order) drawBubble(ctx, b, pos.get(b.id), now);
  });

  const bubbleCache = new Map();
  function drawBubble(ctx, b, p, now) {
    if (!p) return;
    const key = `${b.title}|${b.prose}`;
    let hit = bubbleCache.get(b.id);
    if (!hit || hit.key !== key) {
      hit = { key, lines: bubbleLines(ctx, b) };
      bubbleCache.set(b.id, hit);
      if (bubbleCache.size > 64) bubbleCache.delete(bubbleCache.keys().next().value);
    }
    paintBubble(ctx, hit.lines, STATES[b.state], p.x * T, p.y * T - BOT_FOOT - 3, now);
  }

  let consoleView = null;
  let consoleFilter = 'all';
  function consoleList() {
    return filterBots(bots, consoleFilter, keys());
  }
  function showBot(id) {
    const b = byId(id);
    if (!b) return;
    pinned = id;
    renderCard();
    renderPanel();
    const p = posOf(b);
    if (p) api.lookAt(p.x - 0.5, p.y - 0.5);
    paintConsole();
  }
  function paintConsole() {
    if (!consoleView) return;
    const list = consoleList();
    const cur = byId(pinned);
    const s = cur && STATES[cur.state];
    setHtml(consoleView.q('.ac-body'), html`
      <div class="ac-top"><div class="ac-seg">${['all', 'mine'].map((f) => html`<button type="button" data-cf="${f}" class="${consoleFilter === f ? 'on' : ''}">${f === 'all' ? 'All agents' : 'Mine'}</button>`)}</div>
        <span class="muted">${list.length} of ${bots.length} · ${AGENT_STATIONS.length} stations</span></div>
      <div class="ac-now">
        <button type="button" class="icon-btn" data-step="-1" aria-label="Previous agent">‹</button>
        ${cur ? html`<div class="ac-cur"><img alt="" src="${botDataUrl(cur.modelKey)}"><div><b>${cur.label}</b> <span class="ag-pill${s.pulse ? ' pulse' : ''}" style="--c:${s.color}">${s.label}</span><p>${cur.title}</p>${cur.prose ? html`<p class="muted">${cur.prose}</p>` : ''}<small class="muted">${ownerName(cur)} · ${cur.modelLabel}</small></div></div>` : html`<div class="ac-cur muted">Pick an agent below, or use the arrows.</div>`}
        <button type="button" class="icon-btn" data-step="1" aria-label="Next agent">›</button>
      </div>
      <div class="ac-grid">${list.length ? list.map((b) => {
        const st = STATES[b.state];
        return html`<button type="button" class="ac-tile${b.id === pinned ? ' on' : ''}" data-id="${b.id}" title="${b.title}"><img alt="" src="${botDataUrl(b.modelKey)}"><span class="ac-l">${b.label}</span><i style="background:${st.color}"></i></button>`;
      }) : html`<p class="muted">${signed() ? 'No agents right now.' : 'Sign in to see agents.'}</p>`}</div>`);
  }
  function openConsole() {
    if (consoleView) return;
    consoleView = overlay(api, {
      title: 'Agent control panel',
      body: html`<div class="ac-body"></div>`,
      modal: false,
      onClose: () => { consoleView = null; },
      onKey: (k, down) => {
        if (!down || (k !== 'arrowright' && k !== 'arrowleft' && k !== 'd' && k !== 'a')) return;
        const id = stepBot(consoleList(), pinned, k === 'arrowright' || k === 'd' ? 1 : -1);
        if (id) showBot(id);
      },
    });
    consoleView.el.classList.add('ac-ov');
    consoleView.el.addEventListener('click', (e) => {
      const f = e.target.closest('[data-cf]')?.dataset.cf;
      if (f) { consoleFilter = f; return paintConsole(); }
      const st = e.target.closest('[data-step]')?.dataset.step;
      if (st) { const id = stepBot(consoleList(), pinned, +st); if (id) showBot(id); return; }
      const id = e.target.closest('.ac-tile')?.dataset.id;
      if (id) showBot(id);
    });
    if (!pinned && bots.length) pinned = consoleList()[0]?.id || null;
    if (pinned) showBot(pinned);
    paintConsole();
    if (!bots.length) load(true);
  }
  api.onObject(CONSOLE, () => openConsole());

  api.onTick(() => {
    if (!card.hidden) placeCard();
  });

  document.addEventListener('office:account', () => {
    lastKeys = keys().join('|');
    prev.clear();
    bots = [];
    if (api.me) load();
    else renderPanel();
  });

  document.addEventListener('office:panel', (e) => {
    if (e.detail === 'agents') renderPanel();
  });

  api.onExtra((o, k) => {
    if (k === 'fm' || k === 'desk') renderPanel();
  });

  api.personBadge((o) => {
    if (!bots.length) return '';
    const pk = o === api.me ? [...keys(), ...personKeys(o)] : personKeys(o);
    if (!pk.length) return '';
    const n = bots.filter((b) => STATES[b.state].live && pk.includes(b.owner)).length;
    return n ? html` <span class="ag-badge" title="${n} agent${n === 1 ? '' : 's'} active">${svg(IC.bot, 11)}${n}</span>` : '';
  });

  api.paletteItems((q) => {
    if (!signed()) return [];
    const c = counts(bots.filter((b) => isMine(b, keys())));
    const items = [{ ic: svg(IC.bot, 14), label: 'Walk to my agents', sub: c.live ? `${c.live} active` : 'Agents', run: summon }];
    if (q) {
      for (const b of bots) {
        items.push({ ic: svg(IC.bot, 14), label: `${b.label}: ${b.title}`, sub: STATES[b.state].label, run: () => { api.showPanel('agents', true); focusBot(b.id); } });
      }
    }
    return items;
  });

  api.onLeave(() => {
    bots = [];
    pos.clear();
  });

  renderPanel();
  if (typeof window !== 'undefined') window.officeAgents = { get bots() { return bots; }, get pos() { return pos; }, load, models: MODELS };
}
