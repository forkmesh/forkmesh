import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { mentionQuery, chanForZone, roomLabel, ROOMS } from '../public/features/chat/core.js';

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
