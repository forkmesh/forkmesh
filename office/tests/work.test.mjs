import { test } from 'node:test';
import assert from 'node:assert/strict';
import { obj } from '../public/map.js';
import {
  findDesks, deskAt, ownerKey, cleanTask, claimDesk, releaseDesk, deskList, parseTaskPage, normTask, snapshot,
  diffTasks, findMentions, newReplies, filterTasks, sortTasks, pollInterval, fmtElapsed, liveElapsed, pingTargets, apiError,
} from '../public/features/work/logic.js';

const DESKS = findDesks(obj);

test('desks are the up-facing chairs under a desk or monitor', () => {
  assert.ok(DESKS.length >= 4);
  for (const d of DESKS) {
    assert.equal(obj[d.y][d.x], '^');
    assert.ok(['M', 'D'].includes(obj[d.my][d.mx]));
  }
  const d = DESKS[0];
  assert.equal(deskAt(DESKS, d.mx, d.my), d);
  assert.equal(deskAt(DESKS, d.x, d.y), d);
  assert.equal(deskAt(DESKS, 0, 0), null);
});

test('owner keys prefer the account id', () => {
  assert.equal(ownerKey({ acct: 'abc', name: 'Jett' }), 'a:abc');
  assert.equal(ownerKey({ name: ' Jett ' }), 'n:jett');
  assert.equal(ownerKey({ name: '' }), '');
});

test('cleanTask validates id and title', () => {
  assert.deepEqual(cleanTask({ id: 'a1b2', title: ' Ship <b>it</b> ' }), { id: 'a1b2', title: 'Ship  b it /b'.replace(/\s+/g, ' ') });
  assert.equal(cleanTask({ id: 'bad id', title: 'x' }), undefined);
  assert.equal(cleanTask({ id: 'ok', title: '   ' }), undefined);
  assert.equal(cleanTask(null), undefined);
  assert.equal(cleanTask({ id: 'ok', title: 'x'.repeat(500) }).title.length, 160);
});

test('claim, move and release desks', () => {
  const [a, b] = DESKS;
  let r = claimDesk({}, 'n:jett', 'Jett', a.x, a.y, DESKS);
  assert.ok(r.desks);
  assert.equal(r.prev, null);
  let desks = r.desks;
  r = claimDesk(desks, 'n:dana', 'Dana', a.x, a.y, DESKS);
  assert.equal(r.error, 'taken');
  assert.equal(r.by, 'Jett');
  assert.equal(claimDesk(desks, 'n:dana', 'Dana', 0, 0, DESKS).error, 'not_a_desk');
  assert.equal(claimDesk(desks, '', 'x', b.x, b.y, DESKS).error, 'no_owner');
  r = claimDesk(desks, 'n:jett', 'Jett', b.x, b.y, DESKS);
  assert.deepEqual(r.prev, { x: a.x, y: a.y, name: 'Jett' });
  desks = r.desks;
  assert.equal(deskList(desks).length, 1);
  r = releaseDesk(desks, 'n:jett');
  assert.deepEqual(r.desks, {});
  assert.equal(releaseDesk({}, 'n:jett').prev, null);
});

const page = {
  ok: true, actor: 'Jett', canManage: false, serverNow: 1000, hasMore: true, nextOffset: 25,
  tasks: [
    { id: 'a'.repeat(32), title: 'Write docs', status: 'idle', assignee: 'Jett', createdBy: 'dana', replyCount: 0, lastReplyAt: 0, priority: 10 },
    { id: 'b'.repeat(32), title: 'Fix bug', status: 'active', assignee: 'dana', createdBy: 'jett', elapsedMs: 120000, startedAt: 5, priority: 5 },
    { id: 'c'.repeat(32), title: 'Old', status: 'done', assignee: 'bob', createdBy: 'bob' },
    { nope: true },
  ],
};

test('parseTaskPage normalises the /api/tasks page', () => {
  const p = parseTaskPage(page);
  assert.equal(p.tasks.length, 3);
  assert.equal(p.actor, 'jett');
  assert.equal(p.nextOffset, 25);
  assert.equal(p.tasks[0].assignee, 'jett');
  assert.equal(parseTaskPage({ ...page, hasMore: false }).nextOffset, null);
  assert.deepEqual(parseTaskPage(null).tasks, []);
  assert.equal(normTask({ id: 'x', status: 'weird' }).status, 'idle');
});

test('filterTasks and sortTasks', () => {
  const { tasks } = parseTaskPage(page);
  assert.deepEqual(filterTasks(tasks, { scope: 'mine', status: 'open' }, 'jett').map((t) => t.title), ['Write docs', 'Fix bug']);
  assert.deepEqual(filterTasks(tasks, { scope: 'all', status: 'done' }, 'jett').map((t) => t.title), ['Old']);
  assert.deepEqual(filterTasks(tasks, { scope: 'all', status: 'any', q: 'bug' }, 'jett').map((t) => t.title), ['Fix bug']);
  assert.deepEqual(sortTasks(tasks).map((t) => t.status), ['active', 'idle', 'done']);
});

test('diffTasks finds new assignments, status changes and replies', () => {
  const { tasks } = parseTaskPage(page);
  assert.deepEqual(diffTasks(null, tasks, 'jett'), []);
  const snap = snapshot(tasks);
  assert.deepEqual(diffTasks(snap, tasks, 'jett'), []);
  const next = tasks.map((t) => ({ ...t }));
  next[1].status = 'done';
  next[2].assignee = 'jett';
  next[2].status = 'idle';
  next[0].replyCount = 2;
  next[0].lastReplyAt = 50;
  const ev = diffTasks(snap, next, 'jett');
  const kinds = ev.map((e) => `${e.kind}:${e.task.title}`).sort();
  assert.deepEqual(kinds, ['assigned:Old', 'reply:Write docs', 'status:Fix bug']);
  assert.equal(ev.find((e) => e.kind === 'status').to, 'done');
  const brandNew = [...tasks, normTask({ id: 'd'.repeat(32), title: 'New one', assignee: 'jett', createdBy: 'dana' })];
  assert.deepEqual(diffTasks(snap, brandNew, 'jett').map((e) => e.kind), ['assigned']);
  const self = [...tasks, normTask({ id: 'e'.repeat(32), title: 'Mine', assignee: 'jett', createdBy: 'jett' })];
  assert.deepEqual(diffTasks(snap, self, 'jett'), []);
  const other = tasks.map((t) => ({ ...t }));
  other[2].status = 'idle';
  assert.deepEqual(diffTasks(snap, other, 'jett'), []);
});

test('mentions and new replies', () => {
  const replies = [
    { author: 'dana', body: 'hey @jett can you look', createdAt: 10 },
    { author: 'dana', body: 'mail jett@x.com', createdAt: 11 },
    { author: 'jett', body: '@jett note to self', createdAt: 12 },
    { author: 'bob', body: 'cc @jettison', createdAt: 13 },
    { author: 'bob', body: 'old @jett', createdAt: 1 },
  ];
  assert.deepEqual(findMentions(replies, 'jett', 5).map((r) => r.createdAt), [10]);
  assert.deepEqual(newReplies(replies, 'jett', 5).map((r) => r.createdAt), [10, 11, 13]);
  assert.deepEqual(findMentions(null, 'jett', 0), []);
});

test('poll cadence, time formatting, ping targets, errors', () => {
  assert.equal(pollInterval({ panelOpen: true, atDesk: false }), 60e3);
  assert.equal(pollInterval({ panelOpen: false, atDesk: true }), 60e3);
  assert.equal(pollInterval({ panelOpen: false, atDesk: false }), 300e3);
  assert.equal(fmtElapsed(0), '0m');
  assert.equal(fmtElapsed(90 * 60e3), '1h 30m');
  assert.equal(fmtElapsed(26 * 3600e3), '1d 2h');
  assert.equal(liveElapsed({ status: 'active', elapsedMs: 1000, fetchedAt: 100 }, 1100), 2000);
  assert.equal(liveElapsed({ status: 'idle', elapsedMs: 1000, fetchedAt: 100 }, 1100), 1000);
  assert.deepEqual(pingTargets({ assignee: 'dana', createdBy: 'jett' }, 'jett'), ['dana']);
  assert.deepEqual(pingTargets({ assignee: 'agent', createdBy: 'bob' }, 'jett'), ['bob']);
  assert.match(apiError(401, null), /Sign in/);
  assert.match(apiError(409, { error: 'active_task_exists' }), /running timer/);
});
