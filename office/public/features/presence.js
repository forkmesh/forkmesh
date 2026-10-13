import { html, setHtml, trusted } from '../html.js';
import { STATUSES, STATUS_LABEL, STATUS_COLOR, TEXT_MAX, IDLE_MS, cleanStatus, effective, statusLine, badgeEmoji, focusBlocks, shouldAutoAway, fmtDur, followStep } from './presence/logic.js';

const svg = (d, s = 16) => html`<svg width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${d}</svg>`;
const IC = {
  clock: html`<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>`,
  follow: html`<path d="M9 18l6-6-6-6"/><path d="M3 18l6-6-6-6"/>`,
  stop: html`<rect x="6" y="6" width="12" height="12" rx="2"/>`,
  summon: html`<path d="M18 11V6a2 2 0 0 0-4 0"/><path d="M14 10V4a2 2 0 0 0-4 0v2"/><path d="M10 10.5V6a2 2 0 0 0-4 0v8"/><path d="M18 8a2 2 0 1 1 4 0v6a8 8 0 0 1-8 8h-2c-2.8 0-4.5-.86-5.99-2.34l-3.6-3.6a2 2 0 0 1 2.83-2.82L7 15"/>`,
  refresh: html`<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>`,
  check: html`<path d="M20 6 9 17l-5-5"/>`,
  x: html`<path d="M18 6 6 18"/><path d="m6 6 12 12"/>`,
};
const QUICK = ['🎯', '📅', '🍕', '🏠', '🤒', '🌴', '🚗', '🎧'];
const MOVE_KEYS = new Set(['arrowdown', 'arrowup', 'arrowleft', 'arrowright', 'w', 'a', 's', 'd']);

export default function setup(api) {
  const { $, esc, tint, initial } = api;

  let lastAway = false;
  let autoAway = null;
  let lastInput = Date.now();
  let hiddenSince = null;
  let lastIdleCheck = 0;
  let follow = null;
  let att = null;
  let attTab = 'today';
  let attTimer = null;

  const myStatus = () => api.me?.ex?.status || null;

  function setStatus(st) {
    const me = api.me;
    if (!me) return;
    const clean = st ? cleanStatus(st) : null;
    if (st && !clean) return;
    const away = clean?.s === 'away';
    if (!!me.away !== away) api.toggleAway();
    lastAway = !!me.away;
    if (!clean || (clean.s === 'available' && !clean.emoji && !clean.text)) {
      if (me.ex.status) api.setExtra('status', null);
      delete me.ex.status;
    } else api.setExtra('status', clean);
    paintMe();
    api.refreshPeople();
  }

  function paintMe() {
    const d = document.querySelector('#b-me .dot');
    if (d && api.me) d.style.background = STATUS_COLOR[effective(api.me)];
  }

  api.joinExtras(() => {
    const st = myStatus();
    return st ? { status: st } : {};
  });

  api.onInit(() => {
    lastAway = !!api.me?.away;
    paintMe();
    if (follow && !api.others.has(follow.id)) stopFollow(false);
  });

  api.onExtra((o, k) => {
    if (o !== api.me || k !== 'status') return;
    const away = myStatus()?.s === 'away';
    if (away !== !!api.me.away) {
      api.toggleAway();
      lastAway = !!api.me.away;
    }
    if (!away) autoAway = null;
    paintMe();
  });

  api.dotColor((o) => STATUS_COLOR[effective(o)]);

  api.proximity((o) => (api.me && focusBlocks(api.me, o) ? false : undefined));

  api.personBadge((o) => {
    const e = badgeEmoji(o);
    return e ? html` <span class="pr-badge" title="${(statusLine(o))}">${(e)}</span>` : '';
  });

  api.onDrawScreen((ctx, o, sx, above) => {
    const e = badgeEmoji(o);
    if (!e) return;
    ctx.font = '16px system-ui, sans-serif';
    ctx.textBaseline = 'alphabetic';
    ctx.textAlign = 'center';
    ctx.fillText(e, sx, above);
    return above - 20;
  });

  const people = $('#people');
  if (people) {
    new MutationObserver(() => {
      for (const li of people.children) {
        const id = li.dataset.id;
        if (!id) continue;
        const o = id === api.me?.id ? api.me : api.others.get(id);
        if (!o) continue;
        const dot = li.querySelector('.av .dot');
        if (dot) dot.style.background = STATUS_COLOR[effective(o)];
        const small = li.querySelector('.who small');
        if (small) small.textContent = `${statusLine(o)} · ${api.zoneName(o)}`;
      }
    }).observe(people, { childList: true });
  }

  const pop = $('#pop');
  if (pop) {
    new MutationObserver(() => {
      if (pop.hidden || pop.dataset.kind !== 'me' || pop.querySelector('.pr-status')) return;
      const menu = pop.querySelector('.menu');
      if (!menu) return;
      const lbl = menu.querySelector('.lbl');
      if (lbl && api.me) lbl.textContent = `${api.me.name} · ${STATUS_LABEL[effective(api.me)]}`;
      menu.querySelector('[data-act="away"]')?.remove();
      const box = document.createElement('div');
      box.className = 'pr-status';
      setHtml(box, statusMenu());
      menu.insertBefore(box, lbl ? lbl.nextSibling : menu.firstChild);
      wireMenu(box);
    }).observe(pop, { childList: true });
  }

  function statusMenu() {
    const cur = myStatus();
    const eff = api.me ? effective(api.me) : 'available';
    const rows = STATUSES.map((s) => html`<button type="button" data-pr-s="${s}"><span class="pr-ck">${eff === s ? svg(IC.check, 14) : ''}</span><i class="pr-sw" style="background:${STATUS_COLOR[s]}"></i>${STATUS_LABEL[s]}</button>`);
    return html`${rows}
      <form class="pr-custom">
        <div class="pr-quick">${QUICK.map((q) => html`<button type="button" data-pr-e="${q}" class="${cur?.emoji === q ? 'on' : ''}">${q}</button>`)}</div>
        <div class="pr-row"><input class="input pr-emoji" maxlength="16" placeholder="🙂" value="${(cur?.emoji || '')}" aria-label="Status emoji"><input class="input pr-text" maxlength="${TEXT_MAX}" placeholder="What's your status?" value="${(cur?.text || '')}" aria-label="Status text"></div>
        <p class="pr-err" hidden></p>
        <div class="pr-row"><button type="submit" class="btn btn-sm pr-save">Save</button><button type="button" class="btn btn-sm pr-clear">Clear</button></div>
      </form><hr>`;
  }

  function wireMenu(box) {
    const form = box.querySelector('.pr-custom');
    const base = () => myStatus()?.s || 'available';
    box.addEventListener('click', (e) => {
      e.stopPropagation();
      const b = e.target.closest('button');
      if (!b) return;
      if (b.dataset.prS) {
        const next = { s: b.dataset.prS, emoji: form.querySelector('.pr-emoji').value.trim(), text: form.querySelector('.pr-text').value.trim() };
        setStatus(cleanStatus(next) ? next : { s: next.s, text: next.text });
        api.closePop();
      } else if (b.dataset.prE) {
        const inp = form.querySelector('.pr-emoji');
        inp.value = inp.value === b.dataset.prE ? '' : b.dataset.prE;
        for (const q of box.querySelectorAll('[data-pr-e]')) q.classList.toggle('on', q.dataset.prE === inp.value);
        form.querySelector('.pr-text').focus();
      } else if (b.classList.contains('pr-clear')) {
        setStatus(null);
        api.closePop();
      } else if (b.classList.contains('pr-save')) {
        form.requestSubmit();
      }
    });
    form.addEventListener('submit', (e) => {
      e.preventDefault();
      const emoji = form.querySelector('.pr-emoji').value.trim();
      const text = form.querySelector('.pr-text').value.trim();
      const next = { s: base(), emoji, text };
      if (!cleanStatus(next)) return api.setNote(form.querySelector('.pr-err'), { title: "That emoji can't be used", body: 'Pick a single emoji.', tone: 'error' });
      setStatus(next);
      api.closePop();
    });
  }

  api.paletteItems(() => [
    ...STATUSES.map((s) => ({ ic: html`<i class="pr-sw" style="background:${STATUS_COLOR[s]}"></i>`, label: `Set status: ${STATUS_LABEL[s]}`, run: () => setStatus({ s, emoji: myStatus()?.emoji, text: myStatus()?.text }) })),
    { ic: svg(IC.x, 14), label: 'Clear status', run: () => setStatus(null) },
    { ic: svg(IC.clock, 14), label: 'Attendance', sub: 'Who is in today', run: openAttendance },
    ...(follow ? [{ ic: svg(IC.stop, 14), label: 'Stop following', run: () => stopFollow(true) }] : []),
  ]);

  const markInput = () => {
    lastInput = Date.now();
    if (autoAway) restore();
  };
  for (const ev of ['keydown', 'pointerdown', 'pointermove', 'wheel', 'touchstart']) addEventListener(ev, markInput, { passive: true, capture: true });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) hiddenSince = Date.now();
    else {
      hiddenSince = null;
      markInput();
    }
  });

  function restore() {
    const prev = autoAway?.prev;
    autoAway = null;
    if (myStatus()?.s !== 'away' || !myStatus()?.auto) return;
    setStatus(prev);
    api.notice('Welcome back', prev ? `Status set back to ${STATUS_LABEL[prev.s]}.` : 'You are available again.');
  }

  api.onTick(() => {
    const me = api.me;
    if (!me) return;
    if (!!me.away !== lastAway) {
      lastAway = !!me.away;
      const s = myStatus()?.s;
      if (me.away && s !== 'away') setStatus({ s: 'away', emoji: myStatus()?.emoji, text: myStatus()?.text });
      else if (!me.away && s === 'away') {
        autoAway = null;
        setStatus({ s: 'available', emoji: myStatus()?.emoji, text: myStatus()?.text });
      }
    }
    const now = Date.now();
    if (now - lastIdleCheck > 5000) {
      lastIdleCheck = now;
      if (!autoAway && myStatus()?.s !== 'away' && !me.away && shouldAutoAway(now, lastInput, hiddenSince, IDLE_MS)) {
        autoAway = { prev: myStatus() };
        setStatus({ s: 'away', auto: true });
      }
    }
    if (follow) tickFollow(now);
  });

  function say(key, title, body, actions = []) {
    return api.hudNote({ key, title, body, actions: actions.map((a) => ({ label: a.label, primary: a.primary, run: a.run })) });
  }

  let chip = null;
  function showChip() {
    const stage = $('#stage');
    if (!stage) return;
    if (!chip) {
      chip = document.createElement('div');
      chip.className = 'pr-chip';
      stage.append(chip);
      chip.addEventListener('click', (e) => { if (e.target.closest('button')) stopFollow(true); });
    }
    const o = follow && api.others.get(follow.id);
    chip.hidden = !o;
    if (o) setHtml(chip, html`${svg(IC.follow, 14)}<span>Following <b>${(o.name)}</b> <span class="muted">· move to stop</span></span><button class="btn btn-sm" type="button">Stop</button>`);
  }

  function startFollow(id) {
    const o = api.others.get(id);
    if (!o) return;
    follow = { id, last: null };
    api.walkTo(id);
    follow.last = { x: o.x, y: o.y, at: Date.now() };
    showChip();
    api.hudNote({ key: 'follow' });
    api.refreshPeople();
  }

  function stopFollow(announce, why) {
    if (!follow) return;
    const o = api.others.get(follow.id);
    follow = null;
    showChip();
    api.refreshPeople();
    if (announce) say('follow', why || `Stopped following ${o?.name || 'them'}`);
  }

  function tickFollow(now) {
    const o = api.others.get(follow.id);
    if (!o) return stopFollow(true, 'They left the office, so you stopped following.');
    const pathLen = api.path?.length || 0;
    const act = followStep(api.me, o, follow.last, pathLen, now);
    if (act === 'walk') {
      api.walkTo(follow.id);
      follow.last = { x: o.x, y: o.y, at: now };
    }
  }

  addEventListener('keydown', (e) => {
    if (!follow || e.metaKey || e.ctrlKey || /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) || e.target.isContentEditable) return;
    if (MOVE_KEYS.has(e.key.toLowerCase())) stopFollow(true);
  }, true);
  const cv = $('#cv');
  let down = null;
  cv?.addEventListener('pointerdown', (e) => { down = { x: e.clientX, y: e.clientY }; }, true);
  cv?.addEventListener('pointerup', (e) => {
    if (follow && down && Math.hypot(e.clientX - down.x, e.clientY - down.y) < 5) stopFollow(true);
    down = null;
  }, true);
  $('#minimap')?.addEventListener('click', () => stopFollow(true), true);

  api.personActions((o) => [
    follow?.id === o.id
      ? { icon: svg(IC.stop, 14), tip: 'Stop following', run: () => stopFollow(true) }
      : { icon: svg(IC.follow, 14), tip: 'Follow', run: () => startFollow(o.id) },
    { icon: svg(IC.summon, 14), tip: 'Ask to come over', run: () => api.send({ t: 'pr-summon', to: o.id }) },
  ]);

  api.onMessage('pr-summon-r', (m) => {
    if (m.ok) return say('summon', `Asked ${m.name} to come over`, "They'll get a request to walk to you.");
    if (m.reason === 'focus') return say('summon', `${m.name} is focusing`, 'They are not taking requests right now. Try a message instead.');
    if (m.reason === 'wait') return say('summon', 'Request already sent', 'Give them a moment before asking again.');
    say('summon', 'They are no longer here');
  });

  api.onMessage('pr-summon', (m) => {
    say(`summon:${m.f}`, `${m.name} asked you to come over`, 'Accept to walk next to them.', [
      { label: 'Accept', primary: true, run: () => { stopFollow(false); api.walkTo(m.f); api.send({ t: 'pr-summon-a', to: m.f }); } },
      { label: 'Not now', run: () => api.send({ t: 'pr-summon-a', to: m.f, no: true }) },
    ]);
  });

  api.onMessage('pr-summon-a', (m) => {
    say('summon', m.ok ? `${m.name} is on the way` : `${m.name} can't come right now`);
  });

  const { panel, button: railBtn } = api.addPanel({ id: 'attendance', iconHtml: svg(IC.clock, 18), tip: 'Attendance', html: html`<div class="pr-att"><p class="muted pr-empty">Loading…</p></div>` });
  const body = panel.querySelector('.pr-att');
  railBtn.addEventListener('click', (e) => {
    if (!matchMedia('(max-width: 760px)').matches) return;
    e.stopPropagation();
    openSheet();
  });

  const tz = () => new Date().getTimezoneOffset();
  const request = () => api.send({ t: 'pr-att', tz: tz() });

  function openAttendance() {
    if (matchMedia('(max-width: 760px)').matches) return openSheet();
    api.showPanel('attendance', true);
  }

  document.addEventListener('office:panel', (e) => {
    clearInterval(attTimer);
    attTimer = null;
    if (e.detail !== 'attendance') return;
    request();
    attTimer = setInterval(() => (panel.hidden ? (clearInterval(attTimer), (attTimer = null)) : request()), 60e3);
  });

  let sheet = null;
  function openSheet() {
    if (!sheet) {
      sheet = document.createElement('div');
      sheet.className = 'pr-sheet';
      setHtml(sheet, html`<div class="pr-sheet-card"><div class="pr-sheet-h"><b>Attendance</b><button class="icon-btn" type="button" data-pr-close>${svg(IC.x, 14)}</button></div><div class="pr-att"></div></div>`);
      sheet.addEventListener('click', (e) => { if (e.target === sheet || e.target.closest('[data-pr-close]')) sheet.hidden = true; });
      sheet.querySelector('.pr-att').addEventListener('click', onAttClick);
      document.body.append(sheet);
    }
    sheet.hidden = false;
    renderAtt();
    request();
  }

  const time = (t) => new Date(t).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });

  function attHtml() {
    if (!att) return html`<p class="muted pr-empty">Loading…</p>`;
    const board = attTab === 'today' ? att.today : att.thisWeek;
    const max = board[0]?.ms || 1;
    const who = att.inToday.map((p) => html`<li>
        <span class="av" style="background:${tint([...p.name].reduce((a, c) => a + c.charCodeAt(0), 0) * 97)}">${((p.name[0] || '?').toUpperCase())}${p.online ? html`<b class="dot"></b>` : ''}</span>
        <span class="who"><b>${(p.name)}${p.me ? html` <span class="muted">(you)</span>` : ''}</b><small>${p.visits.map(([a, b]) => `${time(a)}–${b ? time(b) : 'now'}`).join(', ')}</small></span>
        <span class="pr-dur">${fmtDur(p.ms)}</span></li>`);
    const rows = board.map((p, i) => html`<li class="${p.me ? 'me' : ''}"><span class="pr-rank">${i + 1}</span><span class="pr-name">${(p.name)}</span><span class="pr-bar"><i style="width:${Math.max(4, Math.round((p.ms / max) * 100))}%"></i></span><span class="pr-dur">${fmtDur(p.ms)}</span></li>`);
    return html`<div class="pr-att-head"><span>Your time in the office</span><button class="icon-btn" type="button" data-pr-refresh data-tip="Refresh">${svg(IC.refresh, 14)}</button></div>
      <div class="pr-me"><div><small>Today</small><b>${fmtDur(att.me.today)}</b></div><div><small>This week</small><b>${fmtDur(att.me.week)}</b></div></div>
      <div class="pr-sec pr-sec-tabs"><span>Time in office</span><span class="pr-tabs"><button type="button" data-pr-tab="today" class="${attTab === 'today' ? 'on' : ''}">Today</button><button type="button" data-pr-tab="week" class="${attTab === 'week' ? 'on' : ''}">This week</button></span></div>
      <ol class="pr-board">${rows.length ? rows : html`<li class="pr-empty muted">Nothing yet.</li>`}</ol>
      <div class="pr-sec">In today <span class="muted">(${att.inToday.length})</span></div>
      <ul class="pr-who">${who.length ? who : html`<li class="pr-empty muted">No one has been in yet today.</li>`}</ul>
      <p class="pr-foot muted">Visits are kept for 14 days.</p>`;
  }

  function renderAtt() {
    setHtml(body, attHtml());
    if (sheet && !sheet.hidden) setHtml(sheet.querySelector('.pr-att'), attHtml());
  }

  function onAttClick(e) {
    const b = e.target.closest('button');
    if (!b) return;
    if (b.dataset.prTab) { attTab = b.dataset.prTab; renderAtt(); }
    else if (b.hasAttribute('data-pr-refresh')) request();
  }
  body.addEventListener('click', onAttClick);

  api.onMessage('pr-att', (m) => {
    att = m;
    renderAtt();
  });
}
