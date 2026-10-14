import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { mentionQuery, chanForZone, roomLabel, ROOMS, receiptState, receiptShapes, RECEIPT_LABELS, lastLine, listTime } from '../public/features/chat/core.js';
import { plainTextFromRichSource } from '../../app/public/chat-rich-text.js';

const CHAT = readFileSync(new URL('../public/features/chat.js', import.meta.url), 'utf8');

test('mentionQuery finds the token before the caret', () => {
  assert.deepEqual(mentionQuery('hi @an', 6), { start: 3, q: 'an' });
  assert.deepEqual(mentionQuery('@', 1), { start: 0, q: '' });
  assert.equal(mentionQuery('mail a@b', 8), null);
  assert.equal(mentionQuery('hi @an there', 12), null);
});

test('office rooms map onto channels', () => {
  assert.equal(chanForZone('a'), 'engineering');
  assert.equal(chanForZone('c'), 'coffee');
  assert.equal(chanForZone('.'), null);
  assert.equal(roomLabel('coffee'), 'coffee-huddle');
  assert.deepEqual(ROOMS.map((r) => r.ch), ['general', 'engineering', 'design', 'coffee', 'lobby', 'garden']);
});

test('the office chat is the site chat: same transport, seal, moderation, retention and renderer', () => {
  for (const mod of ['chat-room-transport.js', 'chat-seal.js', 'chat-moderation.js', 'chat-retention.js', 'chat-rich-text.js']) {
    assert.ok(CHAT.includes(`'../../../app/public/${mod}'`), mod);
  }
  assert.ok(CHAT.includes('/api/chat/channels/'));
  assert.ok(CHAT.includes('/api/chat/direct-messages'));
  assert.ok(CHAT.includes('allowsControllerAccess: () => false'));
  assert.ok(!/api\.send\(\{ t: 'chat/.test(CHAT), 'chat must not go through the office socket');
});

test('reading a room posts its read mark and marks read elsewhere clear it', () => {
  assert.ok(CHAT.includes("request('/api/chat/reads', { method: 'POST'"));
  assert.ok(CHAT.includes("await request('/api/chat/reads')"));
  assert.ok(/if \(viewing\(\)\) postReadMark\(c\);/.test(CHAT));
  assert.ok(/if \(!readElsewhere\(c, plain\.ts\)\) c\.unread\+\+/.test(CHAT));
  assert.ok(/await loadReadMarks\(\);\s+renderList\(\);/.test(CHAT));
});

test('read marks and new direct messages arrive live over the account event channel', () => {
  assert.ok(CHAT.includes("import { createAccountEventChannel } from '../../../app/public/account-events.js';"));
  assert.ok(CHAT.includes("sessionToken: () => 'cookie'"));
  assert.ok(CHAT.includes("if (topic === 'direct-messages' || topic === 'chat-read') loadDms();"));
  assert.ok(CHAT.includes('onConnected: () => { if (connectedOnce) loadDms(); connectedOnce = true; }'));
  assert.ok(!/setInterval\([^)]*loadDms/.test(CHAT));
});

test('own messages step through sending, sent, delivered and read; channels stop at sent', () => {
  assert.equal(receiptState({ receipt: 'pending' }, false, true), '');
  assert.equal(receiptState({ receipt: 'pending' }, true, true), 'pending');
  assert.equal(receiptState({ receipt: 'pending', recipients: 1 }, true, true), 'sent');
  assert.equal(receiptState({}, true, true), 'sent');
  assert.equal(receiptState({ receipt: 'delivered' }, true, true), 'delivered');
  assert.equal(receiptState({ receipt: 'read' }, true, true), 'read');
  assert.equal(receiptState({ receipt: 'delivered' }, true, false), 'sent');
  assert.equal(receiptState({ receipt: 'read' }, true, false), 'sent');
  assert.deepEqual(RECEIPT_LABELS, { pending: 'Sending', sent: 'Sent', delivered: 'Delivered', read: 'Read' });
});

test('the receipt mark uses the shared 22 x 14 geometry', () => {
  const count = (svg, tag) => (svg.match(new RegExp(`<${tag}\\b`, 'g')) || []).length;
  assert.ok(CHAT.includes('<svg viewBox="0 0 22 14" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${receiptShapes(state)}</svg>'));
  assert.ok(receiptShapes('pending').includes('stroke-dasharray'));
  assert.equal(count(receiptShapes('pending'), 'path'), 0);
  assert.equal(count(receiptShapes('sent'), 'circle'), 1);
  assert.equal(count(receiptShapes('sent'), 'path'), 1);
  assert.equal(count(receiptShapes('delivered'), 'path'), 3);
  assert.ok(receiptShapes('delivered').includes('cx="14" cy="7" r="5.2"'));
  assert.ok(receiptShapes('read').includes('fill="currentColor"'));
  assert.equal((receiptShapes('read').match(/class="rc-cut"/g) || []).length, 2);
});

test('the office chat shows receipts on own messages and withdraws a deleted direct message', () => {
  assert.ok(CHAT.includes("isDirectRoom: (room) => String(room?.scope || '').startsWith('direct:'),"));
  assert.ok(CHAT.includes("onDelivered: (ids, room) => advance(room, 'delivered', ids),"));
  assert.ok(CHAT.includes("onRead: (room) => advance(room, 'read'),"));
  assert.ok(/onRecipients,\s+onDelivered/.test(CHAT));
  assert.ok(CHAT.includes("keep(c, plain, 'pending');"));
  assert.ok(CHAT.indexOf("keep(c, plain, 'pending');") < CHAT.indexOf('const sent = await S.transport.send(plain, { persist: true });'));
  assert.ok(CHAT.includes("if (type === 'delete' && c.kind === 'dm') S.transport.supersede?.([target]);"));
  assert.ok(CHAT.includes('m.recipients = plain.relayRecipients;'));
  assert.ok(CHAT.includes('${receiptNode(m, mine, direct)}'));
});

test('edits and deletes from the author still reach the transcript', () => {
  assert.ok(CHAT.includes('function mutate(c, plain) {'));
  assert.ok(CHAT.includes('if (!m || m.senderId !== plain.senderId) return false;'));
  assert.ok(CHAT.includes('const changed = keep(c, plain) || mutate(c, plain);'));
});

test('the conversation list shows the newest line of each conversation', () => {
  const m = (fields) => ({ sender: 'ana', text: '', ...fields });
  assert.equal(lastLine(undefined, false, true), '');
  assert.equal(lastLine(m({ text: 'see you\n  soon' }), false, true), 'see you soon');
  assert.equal(lastLine(m({ text: 'on my way' }), true, true), 'You: on my way');
  assert.equal(lastLine(m({ text: 'standup?' }), false, false), 'ana: standup?');
  assert.equal(lastLine(m({ text: 'mine' }), true, false), 'You: mine');
  assert.equal(lastLine(m({ fileName: 'plan.pdf' }), false, false), 'ana: 📎 plan.pdf');
  assert.equal(lastLine(m({ text: '**bold** and _it_', richText: { v: 1, source: '**bold** and _it_' } }), false, false, plainTextFromRichSource), 'ana: bold and it');
  const now = new Date(2026, 9, 13, 18, 0).getTime();
  const at = (d) => d.getTime();
  assert.equal(listTime(at(new Date(2026, 9, 13, 14, 2)), now), new Date(2026, 9, 13, 14, 2).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }));
  assert.equal(listTime(at(new Date(2026, 9, 10, 9)), now), new Date(2026, 9, 10, 9).toLocaleDateString([], { weekday: 'short' }));
  assert.equal(listTime(at(new Date(2026, 9, 3, 9)), now), new Date(2026, 9, 3, 9).toLocaleDateString([], { day: 'numeric', month: 'short' }));
  assert.equal(listTime(0, now), '');
});

test('the list repaints when a line arrives, and direct rows show the peer picture', () => {
  assert.ok(CHAT.includes("const lastMsg = (c) => c.msgs.findLast((m) => m.text || m.fileName);"));
  assert.ok(/trim\(c\);\s+renderList\(\);\s+if \(S\.cur === c\.key\) \{/.test(CHAT));
  assert.ok(/c\.ready = true;\s+renderList\(\);/.test(CHAT));
  assert.ok(/renderMsgs\('bottom'\);\s+renderList\(\);/.test(CHAT));
  assert.ok(CHAT.includes('/avatar?size=128'));
  assert.ok(CHAT.includes("img.onerror = () => { S.noPic.add(img.dataset.peer); img.remove(); };"));
  assert.ok(CHAT.includes("${o ? html`<b class=\"dot${o.away ? ' away' : ''}\"></b>` : ''}"));
  assert.ok(CHAT.includes("Math.max(b.updatedAt || 0, newestTs(b))"));
});
