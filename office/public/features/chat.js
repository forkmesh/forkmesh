import { html, setHtml, trusted, join } from '../html.js';
import { ROOMS, MAX_TEXT, chanForZone, roomLabel, mentionQuery, receiptState, receiptShapes, RECEIPT_LABELS, lastLine, listTime } from './chat/core.js';
import { handleOf } from './profile/logic.js';
import { createChatRoomTransport } from '../../../app/public/chat-room-transport.js';
import { createRoomSealStore } from '../../../app/public/chat-seal.js';
import { moderateChatText } from '../../../app/public/chat-moderation.js';
import { createChatRetention, CHAT_RETENTION_ENDPOINT } from '../../../app/public/chat-retention.js';
import { renderRichText, plainTextFromRichSource } from '../../../app/public/chat-rich-text.js';
import { createAccountEventChannel } from '../../../app/public/account-events.js';

const svg = (p, s = 14) => trusted(`<svg class="i" width="${s}" height="${s}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${p}</svg>`);
const IC = {
  hash: (s) => svg('<path d="M4 9h16"/><path d="M4 15h16"/><path d="M10 3 8 21"/><path d="M16 3l-2 18"/>', s),
  chev: (s) => svg('<path d="m6 9 6 6 6-6"/>', s),
  send: (s) => svg('<path d="M14.54 21.69a.5.5 0 0 0 .94-.03l6.5-19a.5.5 0 0 0-.64-.63l-19 6.5a.5.5 0 0 0-.02.93l7.93 3.18a2 2 0 0 1 1.11 1.11z"/><path d="m21.85 2.15-10.94 10.94"/>', s),
  pin: (s) => svg('<path d="M20 10c0 4.99-5.54 10.19-7.4 11.8a1 1 0 0 1-1.2 0C9.54 20.19 4 14.99 4 10a8 8 0 0 1 16 0"/><circle cx="12" cy="10" r="3"/>', s),
  msg: (s) => svg('<path d="M7.9 20A9 9 0 1 0 4 16.1L2 22Z"/>', s),
  plus: (s) => svg('<path d="M5 12h14"/><path d="M12 5v14"/>', s),
  edit: (s) => svg('<path d="M21.17 6.81a1 1 0 0 0-3.99-3.99L3.84 16.17a2 2 0 0 0-.5.83l-1.32 4.35a.5.5 0 0 0 .62.62l4.35-1.32a2 2 0 0 0 .83-.5z"/><path d="m15 5 4 4"/>', s),
  trash: (s) => svg('<path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/>', s),
};
const SELF_KEY = 'forkmesh.chat.selfId';
const PRESENCE_MS = 60e3;
const READ_MARK_FUTURE_SLACK_MS = 24 * 60 * 60 * 1000;
const MAX_KEEP = 300;

function selfId(store) {
  let id = store.get(SELF_KEY);
  if (!id) {
    id = crypto.randomUUID ? crypto.randomUUID() : String(Math.random()).slice(2) + Date.now();
    store.set(SELF_KEY, id);
  }
  return id;
}

async function request(path, init = {}) {
  const r = await fetch(path, { credentials: 'same-origin', cache: 'no-store', ...init, headers: { accept: 'application/json', ...(init.body ? { 'content-type': 'application/json' } : {}), ...(init.headers || {}) } });
  let body = null;
  try { body = await r.json(); } catch {}
  if (!r.ok) {
    const err = new Error(body?.error || `HTTP ${r.status}`);
    err.status = r.status;
    throw err;
  }
  return body;
}

export default function setup(api) {
  const { $, tint } = api;
  const me = selfId(api.store);
  const seals = createRoomSealStore({ acct: () => account(), node: me });
  const retention = createChatRetention({ request: (body) => request(CHAT_RETENTION_ENDPOINT, { method: 'POST', body: JSON.stringify(body) }), storage: globalThis.localStorage });
  const S = { cur: null, view: 'chat', convs: new Map(), rooms: {}, here: null, transport: null, editing: null, ac: null, acSel: 0, presence: null, readMarks: new Map(), noPic: new Set() };

  const panel = $('#panel-chat');
  setHtml(panel, html`
    <div class="ch-bar">
      <button type="button" class="ch-switch" id="ch-switch" aria-haspopup="true"><span class="ch-ic" id="ch-ic">${IC.hash(14)}</span><b id="ch-name">general</b><span class="ch-other" id="ch-other" hidden></span>${IC.chev(12)}</button>
      <button type="button" class="ch-jump" id="ch-jump" hidden></button>
    </div>
    <div class="ch-sub" id="ch-sub" hidden></div>
    <div class="ch-list" id="ch-list" hidden></div>
    <div id="msgs"></div>
    <div class="chat-typing" id="chat-typing"></div>
    <p class="chat-note" id="chat-note" hidden></p>
    <ul class="chat-ac" id="chat-ac" hidden></ul>
    <form id="chat-form" autocomplete="off">
      <input id="chat-input" class="input" maxlength="${MAX_TEXT}" placeholder="Message #general" autocomplete="off">
      <button class="icon-btn" data-tip="Send">${IC.send(14)}</button>
    </form>`);
  const box = $('#msgs'), input = $('#chat-input'), list = $('#ch-list'), acEl = $('#chat-ac'), status = $('#chat-typing'), note = $('#chat-note');

  const panelOpen = () => !panel.hidden && !$('#app').classList.contains('collapsed');
  const atBottom = () => box.scrollHeight - box.scrollTop - box.clientHeight < 40;
  const account = () => api.account?.name || api.me?.name || '';

  function conv(key, extra) {
    let c = S.convs.get(key);
    if (!c) {
      c = { key, kind: key.startsWith('direct:') ? 'dm' : 'room', label: '', wire: '', msgs: [], ids: new Set(), unread: 0, ready: false, peer: '', room: '' };
      S.convs.set(key, c);
    }
    if (extra) Object.assign(c, extra);
    return c;
  }
  const title = (c) => (c.kind === 'dm' ? '@' + c.peer : '#' + (c.room ? roomLabel(c.room) : c.label));

  function setRooms(rooms) {
    S.rooms = rooms && typeof rooms === 'object' ? rooms : {};
    for (const r of ROOMS) {
      const id = S.rooms[r.ch];
      if (id) conv('private:' + id, { room: r.ch, label: r.ch });
    }
    for (const [key, c] of S.convs) if (c.kind === 'room' && !Object.values(S.rooms).includes(key.slice(8))) S.convs.delete(key);
  }

  async function loadChannels() {
    try {
      const body = await request('/api/chat/channels');
      for (const ch of body.channels || []) {
        const c = S.convs.get('private:' + ch.id);
        if (c) c.wire = '#' + ch.name;
      }
    } catch {}
  }

  const newestTs = (c) => (c.msgs.length ? Math.floor(Number(c.msgs[c.msgs.length - 1].ts) || 0) : 0);
  const readElsewhere = (c, ts) => Number(ts) > 0 && Number(ts) <= (S.readMarks.get(c.key) || 0);

  function postReadMark(c) {
    const ts = newestTs(c);
    if (!/^(private|direct):[0-9a-f]{32}$/.test(c.key) || ts <= (S.readMarks.get(c.key) || 0) || ts > Date.now() + READ_MARK_FUTURE_SLACK_MS) return;
    S.readMarks.set(c.key, ts);
    request('/api/chat/reads', { method: 'POST', body: JSON.stringify({ marks: { [c.key]: ts } }) }).catch(() => {});
  }

  async function loadReadMarks() {
    try {
      const body = await request('/api/chat/reads');
      for (const [key, ts] of Object.entries(body.marks || {})) if (Number(ts) > (S.readMarks.get(key) || 0)) S.readMarks.set(key, Number(ts));
    } catch { return; }
    for (const c of S.convs.values()) if (c.unread && c.msgs.length && readElsewhere(c, newestTs(c))) c.unread = 0;
  }

  async function loadDms() {
    try {
      const body = await request('/api/chat/direct-messages');
      for (const d of body.conversations || []) conv('direct:' + d.id, { peer: String(d.otherUser || ''), wire: 'direct:' + d.id, unread: S.cur === 'direct:' + d.id ? 0 : Number(d.unreadCount) || 0, updatedAt: d.updatedAt || 0 });
    } catch {}
    await loadReadMarks();
    renderList();
    renderHeader();
    renderBadge();
  }

  function keep(c, plain, receipt) {
    if (!plain || plain.type !== 'chat' || plain.accountKind !== 'user') return false;
    if (plain.channel !== c.wire || !plain.id || c.ids.has(plain.id)) return false;
    if (retention.isExpired(plain.ts, c.key)) return false;
    c.ids.add(plain.id);
    const m = { id: plain.id, senderId: plain.senderId, sender: String(plain.sender || 'someone').slice(0, 63), text: String(plain.text || ''), richText: plain.richText, ts: Number(plain.ts) || Date.now() };
    if (Number.isSafeInteger(plain.relayRecipients) && plain.relayRecipients >= 0) m.recipients = plain.relayRecipients;
    if (receipt) m.receipt = receipt;
    if (plain.fileName) m.fileName = String(plain.fileName).slice(0, 120);
    c.msgs.push(m);
    return true;
  }

  function mutate(c, plain) {
    if (!plain || (plain.conversation !== c.wire && plain.channel !== c.wire)) return false;
    const m = c.msgs.find((x) => x.id === plain.target);
    if (!m || m.senderId !== plain.senderId) return false;
    if (plain.type === 'delete') { c.msgs.splice(c.msgs.indexOf(m), 1); return true; }
    if (plain.type === 'edit') { Object.assign(m, { text: String(plain.text || ''), richText: plain.richText, edited: true }); return true; }
    return false;
  }

  function forget(c, id) {
    const i = c.msgs.findIndex((x) => x.id === id);
    if (i >= 0) c.msgs.splice(i, 1);
    c.ids.delete(id);
  }

  function onRecipients(id, count, room) {
    const c = S.convs.get(room?.scope);
    const m = c?.msgs.find((x) => x.id === id);
    if (!m || !Number.isSafeInteger(count)) return;
    m.recipients = count;
    if (S.cur === c.key) renderMsgs();
  }

  function advance(room, to, ids) {
    const c = S.convs.get(room?.scope);
    if (c?.kind !== 'dm') return;
    for (const m of c.msgs) {
      const state = receiptState(m, m.senderId === me, true);
      if (state && (!ids || ids.includes(m.id)) && state !== 'read' && (ids || state !== 'pending')) m.receipt = to;
    }
    if (S.cur === c.key) renderMsgs();
  }

  function trim(c) {
    c.msgs.sort((a, b) => a.ts - b.ts);
    if (c.msgs.length > MAX_KEEP) c.msgs.splice(0, c.msgs.length - MAX_KEEP);
  }

  function onPlain(plain, room) {
    const c = S.convs.get(room.scope);
    if (!c) return;
    const changed = keep(c, plain) || mutate(c, plain);
    if (!changed) return;
    trim(c);
    renderList();
    if (S.cur === c.key) {
      renderMsgs(atBottom() ? 'bottom' : null);
      if (!viewing()) { if (!readElsewhere(c, plain.ts)) c.unread++; renderBadge(); }
      else markRead(c);
    }
  }

  function onHistory(plains, room) {
    const c = S.convs.get(room.scope);
    if (!c) return;
    for (const p of plains) keep(c, p);
    for (const p of plains) if (p.type === 'edit' || p.type === 'delete') mutate(c, p);
    trim(c);
    c.ready = true;
    renderList();
    if (S.cur === c.key) renderMsgs('bottom');
  }

  function presenceFrame(type, extra) {
    return { type, id: crypto.randomUUID ? crypto.randomUUID() : String(Math.random()).slice(2), senderId: me, sender: account(), accountKind: 'user', ts: Date.now(), ...extra };
  }

  function connect(c) {
    S.transport?.dispose();
    clearInterval(S.presence);
    c.ready = false;
    status.textContent = 'Connecting…';
    const t = createChatRoomTransport({
      authorize: async (room) => {
        const path = room.scope.startsWith('direct:') ? `/api/chat/direct-messages/${room.scope.slice(7)}/room-access` : `/api/chat/channels/${room.scope.slice(8)}/room-access`;
        const access = await request(path);
        return { scope: room.scope, passphrase: access.passphrase, roomName: access.room, webSocketUrl: access.webSocketUrl };
      },
      allowsControllerAccess: () => false,
      isDirectRoom: (room) => String(room?.scope || '').startsWith('direct:'),
      resolveSeal: (room, roomName) => seals.resolve(room.scope, roomName, location.host, room.scope.startsWith('direct:') && S.convs.get(room.scope)?.peer && account() ? { participants: [account(), S.convs.get(room.scope).peer] } : {}),
      onPlain,
      onHistory,
      onRecipients,
      onDelivered: (ids, room) => advance(room, 'delivered', ids),
      onRead: (room) => advance(room, 'read'),
      onSealed: () => { status.textContent = 'Some earlier messages were sealed for other devices.'; },
      onTrust: (event) => { if (event.type === 'pending') status.textContent = 'A new device wants to join this chat. Check the safety number in Chat, then approve or block it.'; },
      onState: (state, detail) => {
        if (detail.room?.scope !== c.key) return;
        if (state === 'connected') {
          status.textContent = '';
          t.send(presenceFrame('hello', { channels: [c.wire] })).catch?.(() => {});
          S.presence = setInterval(() => t.send(presenceFrame('presence')), PRESENCE_MS);
        } else if (state === 'reconnecting' || state === 'connecting' || state === 'authorizing') status.textContent = 'Connecting…';
      },
    });
    S.transport = t;
    t.connect({ scope: c.key }).catch(() => { status.textContent = 'Could not open this conversation.'; });
  }

  function receiptNode(m, mine, direct) {
    const state = receiptState(m, mine, direct);
    return state ? html`<span class="rc is-${state}" role="img" aria-label="${RECEIPT_LABELS[state]}" title="${RECEIPT_LABELS[state]}">${trusted(`<svg viewBox="0 0 22 14" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${receiptShapes(state)}</svg>`)}</span>` : '';
  }

  function msgNode(m, prev, direct = false) {
    const head = !prev || prev.senderId !== m.senderId || m.ts - prev.ts > 5 * 60e3;
    const mine = m.senderId === me;
    const el = document.createElement('div');
    el.className = `msg${head ? ' head' : ''}${mine ? ' mine' : ''}`;
    el.dataset.mid = m.id;
    const time = new Date(m.ts).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
    const other = [...api.others.values()].find((o) => o.name === m.sender);
    setHtml(el, html`<span class="av" style="background:${tint(other?.av ?? (api.me?.name === m.sender ? api.me.av : [...m.sender].reduce((a, ch) => a + ch.charCodeAt(0), 0) * 97))}">${(m.sender[0] || '?').toUpperCase()}</span>
      <div class="meta">${m.sender}<time title="${new Date(m.ts).toLocaleString()}">${time}</time>${receiptNode(m, mine, direct)}</div><p></p>
      ${mine ? html`<span class="msg-acts"><button type="button" class="icon-btn" data-act="edit" data-tip="Edit">${IC.edit(13)}</button><button type="button" class="icon-btn" data-act="del" data-tip="Delete">${IC.trash(13)}</button></span>` : ''}`);
    const p = el.querySelector('p');
    renderRichText(p, m);
    if (m.edited) {
      const s = document.createElement('span');
      s.className = 'edited';
      s.textContent = ' (edited)';
      p.append(s);
    }
    return el;
  }

  function renderMsgs(mode) {
    const c = S.convs.get(S.cur);
    if (!c) { setHtml(box, html`<p class="chat-empty">No conversation yet.</p>`); return; }
    const prevTop = box.scrollTop;
    if (!c.ready) setHtml(box, html`<p class="chat-empty">Loading…</p>`);
    else if (!c.msgs.length) setHtml(box, html`<p class="chat-empty">${c.kind === 'dm' ? `This is the start of your conversation with ${c.peer}.` : `No messages in ${title(c)} yet.`}</p>`);
    else {
      box.replaceChildren(...c.msgs.map((m, i) => msgNode(m, c.msgs[i - 1], c.kind === 'dm')));
    }
    if (mode === 'bottom') box.scrollTop = box.scrollHeight;
    else box.scrollTop = prevTop;
  }

  function renderHeader() {
    const c = S.convs.get(S.cur);
    $('#ch-name').textContent = c ? title(c) : 'Chat';
    setHtml($('#ch-ic'), c?.kind === 'dm' ? IC.msg(14) : IC.hash(14));
    input.placeholder = c ? `Message ${title(c)}` : 'Choose a conversation';
    input.disabled = !c;
    let other = 0;
    for (const x of S.convs.values()) if (x.key !== S.cur) other += x.unread;
    const o = $('#ch-other');
    o.hidden = !other;
    o.textContent = other > 999 ? '999+' : other;
    const jump = $('#ch-jump');
    const hereKey = S.here && S.rooms[S.here] ? 'private:' + S.rooms[S.here] : null;
    jump.hidden = !hereKey || hereKey === S.cur || S.view !== 'chat';
    if (!jump.hidden) {
      setHtml(jump, html`${IC.pin(12)}#${roomLabel(S.here)}`);
      jump.dataset.tip = 'Open the channel for the room you are in';
    }
    $('#ch-sub').hidden = true;
  }

  const nameTint = (n) => [...n].reduce((a, ch) => a + ch.charCodeAt(0), 0) * 97;
  const mineMsg = (m) => m.senderId === me || m.sender.toLowerCase() === account().toLowerCase();
  const lastMsg = (c) => c.msgs.findLast((m) => m.text || m.fileName);

  function dmFace(c) {
    const peer = c.peer.toLowerCase();
    const o = [...api.others.values()].find((x) => handleOf(x).toLowerCase() === peer || x.name.toLowerCase() === peer);
    const pic = !S.noPic.has(peer) && `/api/accounts/${encodeURIComponent(peer)}/avatar?size=128`;
    return html`<span class="av dm" style="background:${tint(o?.av ?? nameTint(peer))}">${(peer[0] || '?').toUpperCase()}${pic ? html`<img class="pf-img" src="${pic}" alt="" data-peer="${peer}">` : ''}${o ? html`<b class="dot${o.away ? ' away' : ''}"></b>` : ''}</span>`;
  }

  function renderList() {
    if (S.view !== 'list') return;
    const item = (c) => {
      const m = lastMsg(c);
      return html`<button type="button" class="ch-item${c.key === S.cur ? ' on' : ''}${c.unread ? ' unread' : ''}" data-ch="${c.key}">${c.kind === 'dm' ? dmFace(c) : html`<span class="ch-ic">${IC.hash(14)}</span>`}<span class="ch-txt"><span class="ch-top"><span class="ch-lbl">${title(c)}</span>${m ? html`<time>${listTime(m.ts)}</time>` : ''}</span><span class="ch-last">${lastLine(m, m && mineMsg(m), c.kind === 'dm', plainTextFromRichSource)}</span></span>${c.unread ? html`<span class="ch-n">${c.unread > 999 ? '999+' : c.unread}</span>` : ''}</button>`;
    };
    const rooms = ROOMS.map((r) => S.rooms[r.ch] && S.convs.get('private:' + S.rooms[r.ch])).filter(Boolean);
    const dms = [...S.convs.values()].filter((c) => c.kind === 'dm').sort((a, b) => Math.max(b.updatedAt || 0, newestTs(b)) - Math.max(a.updatedAt || 0, newestTs(a)));
    const people = [...api.others.values()].sort((a, b) => a.name.localeCompare(b.name));
    setHtml(list, html`<div class="ch-sec">Channels</div>${rooms.length ? rooms.map(item) : html`<p class="ch-none">Office channels are not available.</p>`}
      <div class="ch-sec">Direct messages</div>${dms.length ? dms.map(item) : html`<p class="ch-none">No conversations yet.</p>`}
      <div class="ch-sec">Message someone in the office</div>${people.length ? people.map((o) => html`<button type="button" class="ch-item" data-dm="${o.name}"><span class="ch-ic"><span class="av sm" style="background:${tint(o.av)}">${api.initial(o)}</span></span><span class="ch-lbl">${o.name}</span>${IC.plus(13)}</button>`) : html`<p class="ch-none">No one else is here right now.</p>`}`);
    for (const img of list.querySelectorAll('img[data-peer]')) img.onerror = () => { S.noPic.add(img.dataset.peer); img.remove(); };
  }

  function renderBadge() {
    let n = 0;
    for (const c of S.convs.values()) n += c.unread;
    const b = $('#unread');
    if (!b) return;
    b.classList.toggle('count', n > 0);
    b.textContent = n > 0 ? (n > 999 ? '999+' : String(n)) : '';
    b.hidden = !n;
  }

  function viewing() { return S.view === 'chat' && panelOpen() && document.visibilityState !== 'hidden'; }

  function markRead(c) {
    const had = c.unread;
    c.unread = 0;
    if (had && c.kind === 'dm') request(`/api/chat/direct-messages/${c.key.slice(7)}/read`, { method: 'POST', body: '{}' }).catch(() => {});
    if (viewing()) postReadMark(c);
    renderHeader();
    renderBadge();
  }

  function setView(v) {
    S.view = v;
    list.hidden = v !== 'list';
    box.hidden = v === 'list';
    $('#chat-form').hidden = v === 'list';
    status.hidden = v === 'list';
    $('#ch-switch').classList.toggle('open', v === 'list');
    closeAc();
    if (v === 'list') loadDms();
    renderList();
    renderHeader();
  }

  function open(key) {
    const c = S.convs.get(key);
    if (!c) return;
    const same = S.cur === key && S.transport;
    S.cur = key;
    S.editing = null;
    api.setNote(note, null);
    setView('chat');
    if (!same) connect(c);
    renderMsgs('bottom');
    markRead(c);
  }

  async function openDm(username) {
    const name = String(username || '').trim().toLowerCase();
    if (!name || name === account().toLowerCase()) return;
    api.showPanel('chat', true);
    try {
      const body = await request('/api/chat/direct-messages', { method: 'POST', body: JSON.stringify({ username: name }) });
      const d = body.conversation;
      const c = conv('direct:' + d.id, { peer: String(d.otherUser || name), wire: 'direct:' + d.id });
      open(c.key);
      input.focus();
    } catch (err) {
      api.setNote(note, { title: "Couldn't open that conversation", body: err.status === 404 ? 'That account was not found.' : 'Try again in a moment.', tone: 'error', dismiss: true });
    }
  }

  async function sendText(text) {
    const c = S.convs.get(S.cur);
    if (!c || !S.transport) return;
    const clean = (await moderateChatText(text)).text;
    const plain = presenceFrame('chat', { channel: c.wire, text: clean, richText: { v: 1, source: clean } });
    keep(c, plain, 'pending');
    trim(c);
    renderMsgs('bottom');
    renderList();
    const sent = await S.transport.send(plain, { persist: true });
    if (!sent) {
      forget(c, plain.id);
      renderMsgs();
      if (!input.value) input.value = text;
      api.setNote(note, { title: 'Not sent', body: 'The conversation is reconnecting. Try again in a moment.', tone: 'error', dismiss: true });
      return;
    }
    api.setNote(note, null);
  }

  async function sendMutation(type, target, text) {
    const c = S.convs.get(S.cur);
    if (!c || !S.transport) return;
    const extra = { conversation: c.wire, target };
    if (type === 'edit') {
      const clean = (await moderateChatText(text)).text;
      Object.assign(extra, { text: clean, richText: { v: 1, source: clean }, editedAt: Date.now() });
    }
    const plain = presenceFrame(type, extra);
    if (await S.transport.send(plain, { persist: true })) {
      mutate(c, plain);
      renderMsgs();
      if (type === 'delete' && c.kind === 'dm') S.transport.supersede?.([target]);
    }
  }

  function startEdit(id) {
    const c = S.convs.get(S.cur);
    const m = c?.msgs.find((x) => x.id === id && x.senderId === me);
    const el = [...box.querySelectorAll('.msg')].find((x) => x.dataset.mid === id);
    if (!m || !el) return;
    const p = el.querySelector('p');
    setHtml(p, html`<input class="input msg-edit" maxlength="${MAX_TEXT}" aria-label="Edit message"><span class="edit-hint">Enter to save · Esc to cancel</span>`);
    const i = p.querySelector('input');
    i.value = m.text;
    i.focus();
    i.onkeydown = (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        const text = i.value.trim();
        if (!text) sendMutation('delete', id);
        else if (text !== m.text) sendMutation('edit', id, text);
        else renderMsgs();
        input.focus();
      } else if (e.key === 'Escape') {
        e.preventDefault();
        e.stopPropagation();
        renderMsgs();
        input.focus();
      }
    };
  }

  function closeAc() { S.ac = null; acEl.hidden = true; }
  function updateAc() {
    const q = mentionQuery(input.value, input.selectionStart ?? input.value.length);
    if (!q) return closeAc();
    const ql = q.q.toLowerCase();
    const cands = [...api.others.values()].filter((o) => !ql || o.name.toLowerCase().startsWith(ql)).sort((a, b) => a.name.localeCompare(b.name)).slice(0, 6);
    if (!cands.length) return closeAc();
    S.ac = { ...q, cands };
    S.acSel = Math.min(S.acSel, cands.length - 1);
    setHtml(acEl, join(cands.map((o, i) => html`<li data-i="${i}" class="${i === S.acSel ? 'sel' : ''}"><span class="av sm" style="background:${tint(o.av)}">${api.initial(o)}</span>${o.name}<small>${api.zoneName(o)}</small></li>`)));
    acEl.hidden = false;
  }
  function pickAc(i) {
    const o = S.ac?.cands[i];
    if (!o) return;
    const caret = input.selectionStart ?? input.value.length;
    const before = input.value.slice(0, S.ac.start) + '@' + o.name + ' ';
    input.value = before + input.value.slice(caret).replace(/^\S*\s?/, '');
    input.setSelectionRange(before.length, before.length);
    closeAc();
    input.focus();
  }

  $('#chat-form').onsubmit = (e) => {
    e.preventDefault();
    const text = input.value.trim();
    if (!text) return;
    input.value = '';
    closeAc();
    sendText(text.slice(0, MAX_TEXT));
  };
  input.addEventListener('keydown', (e) => {
    if (!S.ac) return;
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      S.acSel = (S.acSel + (e.key === 'ArrowDown' ? 1 : -1) + S.ac.cands.length) % S.ac.cands.length;
      updateAc();
    } else if (e.key === 'Enter' || e.key === 'Tab') { e.preventDefault(); pickAc(S.acSel); }
    else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); closeAc(); }
  });
  input.addEventListener('input', () => { S.acSel = 0; updateAc(); });
  input.addEventListener('blur', () => setTimeout(closeAc, 150));
  acEl.addEventListener('mousedown', (e) => { const li = e.target.closest('li[data-i]'); if (li) { e.preventDefault(); pickAc(+li.dataset.i); } });

  $('#ch-switch').onclick = () => setView(S.view === 'list' ? 'chat' : 'list');
  $('#ch-jump').onclick = () => S.here && S.rooms[S.here] && open('private:' + S.rooms[S.here]);
  list.onclick = (e) => {
    const b = e.target.closest('button');
    if (!b) return;
    if (b.dataset.ch) { open(b.dataset.ch); input.focus(); }
    else if (b.dataset.dm) openDm(b.dataset.dm);
  };
  box.onclick = (e) => {
    const b = e.target.closest('button[data-act]');
    const id = e.target.closest('.msg')?.dataset.mid;
    if (!b || !id) return;
    if (b.dataset.act === 'edit') startEdit(id);
    if (b.dataset.act === 'del') {
      if (b.classList.contains('arm')) sendMutation('delete', id);
      else { b.classList.add('arm'); b.dataset.tip = 'Click again to delete'; setTimeout(() => { b.classList.remove('arm'); b.dataset.tip = 'Delete'; }, 3000); }
    }
  };

  document.addEventListener('office:panel', (e) => {
    if (e.detail === 'chat') { const c = S.convs.get(S.cur); if (c) markRead(c); box.scrollTop = box.scrollHeight; }
  });
  document.addEventListener('office:dm', (e) => {
    const o = api.others.get(e.detail?.id);
    if (o) openDm(o.name);
  });

  let started = false;
  api.onInit(async () => {
    if (started) return;
    started = true;
    setRooms(api.officeChat);
    await loadChannels();
    await loadDms();
    retention.refresh().catch(() => {});
    const general = S.rooms.general ? 'private:' + S.rooms.general : null;
    if (general && S.convs.get(general)?.wire) open(general);
    else renderHeader();
    let connectedOnce = false;
    createAccountEventChannel({
      sessionToken: () => 'cookie',
      onTopic: (topic) => { if (topic === 'direct-messages' || topic === 'chat-read') loadDms(); },
      onConnected: () => { if (connectedOnce) loadDms(); connectedOnce = true; },
    }).start();
  });

  let lastTick = 0;
  api.onTick(() => {
    const now = performance.now();
    if (now - lastTick < 400 || !api.me) return;
    lastTick = now;
    const zoneChan = chanForZone(api.zoneAt(api.me.x, api.me.y));
    const here = zoneChan && S.rooms[zoneChan] ? zoneChan : null;
    if (here !== S.here) { S.here = here; renderHeader(); }
  });

  api.personActions((o) => [{ icon: IC.msg(14), tip: 'Message', run: () => openDm(o.name) }]);
  api.paletteItems((q) => {
    const items = [...S.convs.values()].filter((c) => c.wire).map((c) => ({ ic: c.kind === 'dm' ? IC.msg(14) : IC.hash(14), label: title(c), sub: c.kind === 'dm' ? 'Direct message' : 'Chat channel', run: () => { api.showPanel('chat', true); open(c.key); input.focus(); } }));
    if (q) for (const o of api.others.values()) items.push({ ic: IC.msg(14), label: `Message ${o.name}`, sub: 'Direct message', run: () => openDm(o.name) });
    return items;
  });
  renderHeader();
}
