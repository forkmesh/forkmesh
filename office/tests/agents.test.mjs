import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  STATES, MODELS, EMBLEMS, MAX_BOTS, KEEP_FINISHED_MS, normState, modelKey, providerLabel, effortLabel, botFromTask, botsFromPayload,
  ownerKeys, personKeys, isMine, findOwner, anchorFor, slotOffset, layout, diffStates, cleanRelay, applyRelay, relTime, filterBots, counts,
} from '../public/features/agents/model.js';

const NOW = 1_790_000_000_000;

const PAYLOAD = {
  tasks: [
    { id: 'a'.repeat(32), title: 'Fix login redirect', createdBy: 'kaif', status: 'active', createdAt: NOW - 600e3, startedAt: NOW - 590e3, updatedAt: NOW - 10e3, repository: 'forkmesh/forkmesh', agent: { provider: 'claude', model: 'claude-opus-4', strength: 'high', sessionId: 'sess-1', status: 'running', startedBy: 'claude-7' } },
    { id: 'b'.repeat(32), title: 'Write release notes', createdBy: 'kaif', status: 'open', createdAt: NOW - 300e3, updatedAt: NOW - 300e3, agent: { provider: 'codex', model: '', strength: 'medium', sessionId: 'sess-2', status: 'waiting' } },
    { id: 'c'.repeat(32), title: 'Refactor store', createdBy: 'maya', status: 'done', createdAt: NOW - 3600e3, updatedAt: NOW - 1800e3, agent: { provider: 'openai', sessionId: 'sess-3', status: 'success' } },
    { id: 'd'.repeat(32), title: 'Old finished run', createdBy: 'maya', status: 'done', createdAt: NOW - 3 * 86400e3, updatedAt: NOW - 2 * 86400e3, agent: { provider: 'claude', sessionId: 'sess-4', status: 'failed' } },
    { id: 'e'.repeat(32), title: 'No agent here', createdBy: 'kaif', status: 'open', createdAt: NOW },
    { id: 'f'.repeat(32), title: 'Deleted', createdBy: 'kaif', deletedAt: NOW, agent: { sessionId: 'sess-5', status: 'running' } },
    { id: '1'.repeat(32), title: 'Website session', createdBy: 'maya', status: 'active', createdAt: NOW - 60e3, agentSessionId: 'web-9', agent: null },
    { id: '2'.repeat(32), title: 'Bad id', createdBy: 'maya', status: 'active', createdAt: NOW, agent: { sessionId: 'has space', status: 'running' } },
  ],
};

test('normState maps agent and task statuses onto the six states', () => {
  assert.equal(normState('running'), 'running');
  assert.equal(normState('security_pending'), 'waiting');
  assert.equal(normState('success'), 'success');
  assert.equal(normState('rejected'), 'failed');
  assert.equal(normState('cancelled'), 'stopped');
  assert.equal(normState('', 'done'), 'success');
  assert.equal(normState('', 'active'), 'running');
  assert.equal(normState('weird', 'open'), 'queued');
  assert.equal(normState(null, null), 'queued');
  for (const k of ['queued', 'running', 'waiting', 'success', 'failed', 'stopped']) assert.ok(STATES[k].label && /^#[0-9a-f]{6}$/.test(STATES[k].color));
  assert.deepEqual(Object.fromEntries(Object.entries(STATES).map(([k, v]) => [k, v.label])), { queued: 'Queued', running: 'Working', waiting: 'Waiting', success: 'Done', failed: 'Failed', stopped: 'Stopped' });
  assert.equal(STATES.running.color, '#3b45e0');
});

test('modelKey matches model names first, then provider defaults', () => {
  assert.equal(modelKey('claude-opus-4'), 'opus');
  assert.equal(modelKey('Sonnet 5', 'claude'), 'sonnet');
  assert.equal(modelKey('', 'claude'), 'sonnet');
  assert.equal(modelKey('', 'codex'), 'sol');
  assert.equal(modelKey('gpt-5', 'openai'), 'luna');
  assert.equal(modelKey('', ''), 'terra');
  for (const k of Object.keys(MODELS)) {
    assert.equal(EMBLEMS[k].length, 5);
    for (const row of EMBLEMS[k]) assert.match(row, /^[01]{5}$/);
  }
  assert.equal(providerLabel('claude-code'), 'Claude');
  assert.equal(providerLabel(''), 'Agent');
  assert.equal(effortLabel('HIGH'), 'High');
});

test('botsFromPayload turns /api/tasks into bots, drops stale, deleted and invalid rows', () => {
  const bots = botsFromPayload(PAYLOAD, NOW);
  const ids = bots.map((b) => b.id);
  assert.deepEqual(new Set(ids), new Set(['sess-1', 'sess-2', 'sess-3', 'web-9']));
  assert.equal(ids[ids.length - 1], 'sess-3');
  const b1 = bots.find((b) => b.id === 'sess-1');
  assert.equal(b1.state, 'running');
  assert.equal(b1.modelKey, 'opus');
  assert.equal(b1.modelLabel, 'Opus');
  assert.equal(b1.effort, 'High');
  assert.equal(b1.label, 'claude-7');
  assert.equal(b1.owner, 'kaif');
  assert.equal(b1.title, 'Fix login redirect');
  assert.equal(b1.startedAt, NOW - 590e3);
  assert.equal(b1.at, NOW - 10e3);
  const b2 = bots.find((b) => b.id === 'sess-2');
  assert.equal(b2.state, 'waiting');
  assert.equal(b2.modelKey, 'sol');
  assert.equal(b2.label, 'Codex ss-2');
  const web = bots.find((b) => b.id === 'web-9');
  assert.equal(web.state, 'running');
  assert.equal(botsFromPayload(null).length, 0);
  assert.equal(botsFromPayload({ tasks: 'nope' }).length, 0);
  assert.equal(botsFromPayload(PAYLOAD.tasks, NOW).length, 4);
  const many = { tasks: Array.from({ length: 40 }, (_, i) => ({ id: String(i), createdBy: 'x', createdAt: NOW - i, agent: { sessionId: `s${i}`, status: 'running' } })) };
  assert.equal(botsFromPayload(many, NOW).length, MAX_BOTS);
  const old = { tasks: [{ createdAt: NOW - KEEP_FINISHED_MS - 5, agent: { sessionId: 'z', status: 'success' } }] };
  assert.equal(botsFromPayload(old, NOW).length, 0);
});

test('duplicate sessions keep the newest row', () => {
  const bots = botsFromPayload({ tasks: [
    { createdAt: 1, updatedAt: NOW - 5, agent: { sessionId: 'x', status: 'running' } },
    { createdAt: 1, updatedAt: NOW - 1, agent: { sessionId: 'x', status: 'success' } },
  ] }, NOW);
  assert.equal(bots.length, 1);
  assert.equal(bots[0].state, 'success');
});

test('ownership and placement next to the desk or the avatar', () => {
  const bots = botsFromPayload(PAYLOAD, NOW);
  const acct = { id: 'KAIF', name: 'Kaif', org: 'fm' };
  const keys = ownerKeys(acct);
  assert.deepEqual(keys, ['kaif']);
  assert.deepEqual(ownerKeys(null), []);
  assert.deepEqual(filterBots(bots, 'mine', keys).map((b) => b.id).sort(), ['sess-1', 'sess-2']);
  assert.equal(filterBots(bots, 'all', keys).length, 4);
  const me = { id: 'u1', x: 10, y: 10, rx: 10, ry: 10, ex: { desk: { x: 4, y: 6 } } };
  const maya = { id: 'u2', name: 'Maya', x: 20, y: 12, rx: 20, ry: 12, ex: { fm: { a: 'maya', n: 'maya s' } } };
  assert.deepEqual(personKeys(maya), ['maya', 'maya s']);
  assert.ok(isMine(bots[0], keys) || isMine(bots[1], keys));
  assert.equal(findOwner(bots.find((b) => b.id === 'sess-3'), me, [maya], keys), maya);
  assert.equal(findOwner({ owner: 'ghost' }, me, [maya], keys), null);
  assert.deepEqual(anchorFor(me), { x: 4, y: 6, desk: true });
  assert.deepEqual(anchorFor(maya), { x: 20, y: 12, desk: false });
  const pos = layout(bots, me, [maya], keys);
  assert.equal(pos.size, 4);
  const p1 = pos.get('sess-1'), p2 = pos.get('sess-2');
  assert.equal(p1.owner, 'u1');
  assert.ok(p1.desk);
  assert.ok(Math.abs(p1.x - 4.5) <= 2 && Math.abs(p1.y - 6.5) <= 2);
  assert.notDeepEqual([p1.x, p1.y], [p2.x, p2.y]);
  const p3 = pos.get('sess-3');
  assert.equal(p3.owner, 'u2');
  assert.ok(!p3.desk);
  assert.ok(Math.abs(p3.x - 20.5) <= 2);
  const slots = new Set(Array.from({ length: 12 }, (_, i) => slotOffset(i).join(',')));
  assert.equal(slots.size, 12);
  const noOwner = layout(bots, me, [], []);
  assert.equal(noOwner.size, 0);
});

test('state diffs, relay cleaning and relay application', () => {
  const bots = botsFromPayload(PAYLOAD, NOW);
  const prev = new Map(bots.map((b) => [b.id, b.state]));
  const next = botsFromPayload(PAYLOAD, NOW).map((b) => (b.id === 'sess-1' ? { ...b, state: 'success' } : b));
  assert.deepEqual(diffStates(prev, next), [{ id: 'sess-1', from: 'running', to: 'success' }]);
  const cleaned = cleanRelay([
    { id: 'sess-1', st: 'success', at: NOW },
    { id: 'bad id', st: 'success', at: NOW },
    { id: 'sess-2', st: 'exploded', at: NOW },
    { id: 'sess-2', st: 'failed', at: 1.5 },
    { id: 'sess-2', st: 'failed', at: NOW + 3600e3 },
    null,
  ], NOW);
  assert.deepEqual(cleaned, [{ id: 'sess-1', st: 'success', at: NOW }]);
  assert.deepEqual(cleanRelay('x'), []);
  assert.equal(cleanRelay(Array.from({ length: 50 }, (_, i) => ({ id: `s${i}`, st: 'running', at: NOW })), NOW).length, 24);
  const changed = applyRelay(bots, [{ id: 'sess-1', st: 'success', at: NOW }, { id: 'unknown', st: 'failed', at: NOW }, { id: 'sess-2', st: 'failed', at: 5 }]);
  assert.deepEqual(changed, ['sess-1']);
  assert.equal(bots.find((b) => b.id === 'sess-1').state, 'success');
  assert.equal(bots.find((b) => b.id === 'sess-2').state, 'waiting');
  assert.deepEqual(counts(bots), { live: 2, running: 1 });
});

test('relTime', () => {
  assert.equal(relTime(0, NOW), '');
  assert.equal(relTime(NOW - 10e3, NOW), 'just now');
  assert.equal(relTime(NOW - 5 * 60e3, NOW), '5m ago');
  assert.equal(relTime(NOW - 3 * 3600e3, NOW), '3h ago');
  assert.equal(relTime(NOW - 3 * 86400e3, NOW), '3d ago');
});

test('layout skips slots on blocked tiles when a walkable check is given', () => {
  const bots = botsFromPayload({ tasks: Array.from({ length: 4 }, (_, i) => ({ createdBy: 'kaif', createdAt: NOW - i, agent: { sessionId: `w${i}`, status: 'running' } })) }, NOW);
  const me = { id: 'u1', x: 10, y: 10, rx: 10, ry: 10, ex: {} };
  const ok = (x, y) => y <= 10;
  const pos = layout(bots, me, [], ['kaif'], ok);
  assert.equal(pos.size, 4);
  for (const p of pos.values()) assert.ok(Math.floor(p.y) <= 10, `bot at ${p.x},${p.y} is on a blocked tile`);
  const none = layout(bots, me, [], ['kaif'], () => false);
  assert.equal(none.size, 4);
});

test('agent runs without a task become bots owned by whoever started them', async () => {
  const { botFromRun } = await import('../public/features/agents/model.js');
  const run = { sessionId: '168', owner: 'Jett', status: 'running', provider: 'claude-code', model: 'claude-opus-5', startedAt: NOW - 60e3, updatedAt: NOW - 5e3 };
  const b = botFromRun(run);
  assert.equal(b.id, 'jett:168');
  assert.equal(b.owner, 'jett');
  assert.equal(b.state, 'running');
  assert.equal(b.modelKey, 'opus');
  assert.equal(b.title, 'Claude agent run');
  assert.equal(b.startedAt, NOW - 60e3);
  assert.equal(botFromRun({ ...run, owner: '' }), null);
  assert.equal(botFromRun({ ...run, sessionId: 'a b' }), null);
  const bots = botsFromPayload({ tasks: [{ createdBy: 'kaif', createdAt: NOW - 9e3, agent: { sessionId: '168', status: 'running' } }], agentRuns: [run, { ...run, owner: 'kaif', status: 'success', updatedAt: NOW - KEEP_FINISHED_MS - 1 }] }, NOW);
  assert.deepEqual(bots.map((x) => x.id).sort(), ['168', 'jett:168']);
  const taken = botsFromPayload({ tasks: [{ createdBy: 'kaif', createdAt: NOW - 9e3, agent: { sessionId: '12', status: 'running', runBy: 'Mary' } }] }, NOW);
  assert.equal(taken[0].owner, 'mary');
});
