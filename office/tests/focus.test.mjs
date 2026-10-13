import { test } from 'node:test';
import assert from 'node:assert/strict';
import { W, H, obj, floor, isWalkable, chairDir, isCouch, COUCHES, ZONES, ROOM_ANCHORS } from '../public/map.js';
import { findDesks } from '../public/features/work/logic.js';
import { TABLES } from '../public/features/meetings/tables.js';
import { cleanFocus, joinFocus, phase, fmtClock, describe, shortLeft, roundsMinutes, nearestCouch, WORK_MS, BREAK_MS } from '../public/features/focus/logic.js';

const MIN = 60e3;

test('the south wing adds desks, couches and two walkable zones', () => {
  assert.equal(findDesks(obj).length, 28);
  assert.equal(ZONES.w.name, 'Workspace');
  assert.equal(ZONES.f.name, 'Focus Lounge');
  for (const z of ['w', 'f']) {
    const a = ROOM_ANCHORS[z];
    assert.equal(floor[a.y][a.x], z);
    assert.ok(isWalkable(a.x, a.y));
  }
  let couches = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    if (!COUCHES.has(obj[y][x])) continue;
    couches++;
    assert.ok(isCouch(x, y) && isWalkable(x, y) && chairDir(x, y) !== undefined);
  }
  assert.equal(couches, 22);
  assert.equal(TABLES.length, 5);
  const seen = new Set();
  const start = ROOM_ANCHORS.l;
  const q = [[start.x, start.y]];
  seen.add(start.y * W + start.x);
  while (q.length) {
    const [x, y] = q.pop();
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const nx = x + dx, ny = y + dy, k = ny * W + nx;
      if (seen.has(k) || !isWalkable(nx, ny)) continue;
      seen.add(k);
      q.push([nx, ny]);
    }
  }
  for (const d of findDesks(obj)) assert.ok(seen.has(d.y * W + d.x), `desk ${d.x},${d.y} is reachable`);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (isCouch(x, y)) assert.ok(seen.has(y * W + x), `couch ${x},${y} is reachable`);
});

test('a session needs a couch, a known mode and a listed length', () => {
  assert.deepEqual(cleanFocus({ mode: 'focused', mins: 60, topic: ' ship  it ' }, true, 1000), { mode: 'focused', at: 1000, topic: 'ship it', mins: 60, end: 1000 + 60 * MIN });
  assert.deepEqual(cleanFocus({ mode: 'pomodoro', rounds: 2 }, true, 0), { mode: 'pomodoro', at: 0, rounds: 2, end: 55 * MIN });
  assert.deepEqual(cleanFocus({ mode: 'freeform' }, true, 7), { mode: 'freeform', at: 7 });
  assert.equal(cleanFocus({ mode: 'focused', mins: 60 }, false, 0), undefined);
  assert.equal(cleanFocus({ mode: 'focused', mins: 17 }, true, 0), undefined);
  assert.equal(cleanFocus({ mode: 'pomodoro', rounds: 9 }, true, 0), undefined);
  assert.equal(cleanFocus({ mode: 'sleep' }, true, 0), undefined);
  assert.equal(cleanFocus('focused', true, 0), undefined);
  assert.equal(roundsMinutes(1), 25);
  assert.equal(roundsMinutes(4), 115);
});

test('joining copies the host timer and refuses a finished session', () => {
  const host = cleanFocus({ mode: 'focused', mins: 30, topic: 'docs' }, true, 1000);
  assert.deepEqual(joinFocus(host, 'h1', true, 5000), { mode: 'focused', at: 1000, host: 'h1', topic: 'docs', mins: 30, end: host.end });
  assert.equal(joinFocus({ ...host, host: 'root' }, 'h1', true, 5000).host, 'root');
  assert.equal(joinFocus(host, 'h1', true, host.end), undefined);
  assert.equal(joinFocus(host, 'h1', false, 5000), undefined);
  assert.equal(joinFocus(host, '', true, 5000), undefined);
  assert.equal(joinFocus(undefined, 'h1', true, 5000), undefined);
});

test('phases count down work and breaks', () => {
  const f = { mode: 'focused', at: 0, mins: 30, end: 30 * MIN };
  assert.deepEqual(phase(f, 10 * MIN), { kind: 'work', left: 20 * MIN });
  assert.deepEqual(phase(f, 30 * MIN), { kind: 'done', left: 0 });
  const p = { mode: 'pomodoro', at: 0, rounds: 2, end: 55 * MIN };
  assert.deepEqual(phase(p, 0), { kind: 'work', left: WORK_MS, round: 1 });
  assert.deepEqual(phase(p, 26 * MIN), { kind: 'break', left: 4 * MIN, round: 1 });
  assert.deepEqual(phase(p, 30 * MIN), { kind: 'work', left: WORK_MS, round: 2 });
  assert.equal(phase(p, 55 * MIN).kind, 'done');
  assert.deepEqual(phase({ mode: 'freeform', at: 1000 }, 61000), { kind: 'work', left: null, elapsed: 60000 });
  assert.equal(phase(null, 0), null);
  assert.equal(WORK_MS + BREAK_MS, 30 * MIN);
});

test('clocks and labels read naturally', () => {
  assert.equal(fmtClock(0), '0:00');
  assert.equal(fmtClock(59_001), '1:00');
  assert.equal(fmtClock(42 * MIN + 10_000), '42:10');
  assert.equal(fmtClock(61 * MIN), '1:01:00');
  assert.equal(describe({ mode: 'focused', mins: 45 }), '45 min');
  assert.equal(describe({ mode: 'pomodoro', rounds: 3 }), '3 × 25 min');
  assert.equal(describe({ mode: 'freeform' }), 'No timer');
  assert.equal(shortLeft({ mode: 'focused', at: 0, end: 30 * MIN }, 29.5 * MIN), '1m');
  assert.equal(shortLeft({ mode: 'pomodoro', at: 0, rounds: 2, end: 55 * MIN }, 27 * MIN), 'Break 3m');
  assert.equal(shortLeft({ mode: 'freeform', at: 0 }, 5), 'Focus');
  assert.equal(shortLeft({ mode: 'focused', at: 0, end: 10 }, 20), '');
});

test('the nearest free couch is picked', () => {
  const couches = [{ x: 1, y: 1 }, { x: 5, y: 1 }, { x: 9, y: 9 }];
  assert.deepEqual(nearestCouch(couches, new Set(), { x: 4, y: 1 }), { x: 5, y: 1 });
  assert.deepEqual(nearestCouch(couches, new Set([1005]), { x: 4, y: 1 }), { x: 1, y: 1 });
  assert.equal(nearestCouch(couches, new Set([1001, 1005, 9009]), { x: 0, y: 0 }), null);
});

import { pushRecent, cleanRecent, readRecent, recentLabel, startMessage, focusStatusOf, RECENT_MAX } from '../public/features/focus/logic.js';

test('a focus session can carry one emoji, and joiners keep it', () => {
  assert.deepEqual(cleanFocus({ mode: 'freeform', emoji: ' 🚀 ' }, true, 5), { mode: 'freeform', at: 5, emoji: '🚀' });
  assert.equal(cleanFocus({ mode: 'freeform', emoji: 'ab' }, true, 5).emoji, undefined);
  assert.equal(cleanFocus({ mode: 'freeform', emoji: '<' }, true, 5).emoji, undefined);
  const host = cleanFocus({ mode: 'focused', mins: 25, emoji: '📚' }, true, 5);
  assert.equal(joinFocus(host, 'h', true, 10).emoji, '📚');
  assert.deepEqual(focusStatusOf(host), { s: 'focus', emoji: '📚', text: 'Focus session' });
  assert.deepEqual(focusStatusOf({ mode: 'freeform', topic: 'docs' }), { s: 'focus', emoji: '🎧', text: 'docs' });
});

test('recent sessions keep the newest five distinct settings and restart with one message', () => {
  let list = [];
  for (const mins of [15, 25, 30, 45, 60, 90]) list = pushRecent(list, { mode: 'focused', mins }, { status: true });
  assert.equal(list.length, RECENT_MAX);
  assert.equal(list[0].mins, 90);
  list = pushRecent(list, { mode: 'focused', mins: 30 }, { status: false, notify: true });
  assert.deepEqual(list.slice(0, 2).map((r) => r.mins), [30, 90]);
  assert.equal(list.filter((r) => r.mins === 30).length, 1);
  assert.deepEqual(list[0], { mode: 'focused', mins: 30, status: false, notify: true });
  const r = pushRecent([], { mode: 'pomodoro', rounds: 2, topic: ' ship it ', emoji: '🚀', at: 1, end: 9 }, {})[0];
  assert.deepEqual(r, { mode: 'pomodoro', topic: 'ship it', emoji: '🚀', rounds: 2, status: true, notify: false });
  assert.equal(recentLabel(r), '🚀 ship it · 2 × 25 min');
  assert.deepEqual(startMessage(r), { t: 'focus-start', mode: 'pomodoro', notify: false, topic: 'ship it', emoji: '🚀', rounds: 2 });
  assert.deepEqual(cleanFocus(startMessage(r), true, 0).rounds, 2);
  assert.deepEqual(readRecent('not json'), []);
  assert.deepEqual(readRecent(JSON.stringify([{ mode: 'nap' }, { mode: 'focused', mins: 61 }, { mode: 'freeform' }])), [{ mode: 'freeform', status: true, notify: false }]);
  assert.deepEqual(cleanRecent(null), []);
});
