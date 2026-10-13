import { test } from 'node:test';
import assert from 'node:assert/strict';
import { OFFICE_W as W, H, COASTER_PLATFORM, GATE, obj, isWalkable } from '../public/map.js';
import { buildTrack, rideState, rideDuration, loopCenters, LOOP_R } from '../public/features/coaster/track.js';

test('the track is one closed lap around the whole office with three loops', () => {
  const t = buildTrack();
  const first = t.pts[0], last = t.pts[t.pts.length - 1];
  assert.ok(Math.hypot(first.x - last.x, first.y - last.y) < 0.2);
  for (const p of t.pts) {
    assert.ok(p.x >= 0 && p.x <= W && p.y - p.lift >= -LOOP_R && p.y <= H, JSON.stringify(p));
  }
  const loops = loopCenters(t);
  assert.equal(loops.length, 3);
  for (const l of loops) assert.ok(l.x - l.r >= 0 && l.x + l.r <= W && l.y - l.r >= 0 && l.y + l.r <= H, JSON.stringify(l));
});

test('a ride lasts one lap and ends on its own', () => {
  const t = buildTrack();
  const d = rideDuration(t);
  assert.ok(d > 20000 && d < 60000, String(d));
  assert.deepEqual(rideState(1000, 999, t), t.pts[0]);
  assert.equal(rideState(10000, 4999, t), null);
  assert.deepEqual(rideState(1000, 1000, t), t.pts[0]);
  assert.ok(rideState(1000, 1000 + d / 2, t));
  assert.equal(rideState(1000, 1001 + d, t), null);
  assert.equal(rideState(undefined, 1000, t), null);
});

test('the boarding platform is walkable and ends at the gate', () => {
  const { y, x0, x1 } = COASTER_PLATFORM;
  for (let x = x0; x <= x1; x++) assert.ok(isWalkable(x, y), `${x},${y}`);
  assert.equal(obj[y][x1 + 1], GATE);
});

test('boarding waits for the server instead of cancelling on the next frame', async () => {
  globalThis.addEventListener ??= () => {};
  const { default: setup } = await import('../public/features/rides.js');
  const hooks = { tick: [], objects: new Map(), init: [] };
  const sent = [];
  const { y, x0 } = COASTER_PLATFORM;
  const me = { id: 1, x: x0, y, ex: {} };
  const api = {
    me, others: new Map(),
    notice() {}, hudNote() {},
    setExtra(k, v) { if (v === null) delete me.ex[k]; else me.ex[k] = v; sent.push(v); },
    onInit: (fn) => hooks.init.push(fn), onObject: (c, fn) => hooks.objects.set(c, fn),
    onPlace() {}, onPose() {}, onTick: (fn) => hooks.tick.push(fn),
    onDrawUnder() {}, onDrawWorld() {}, paletteItems() {},
  };
  setup(api);
  hooks.objects.get(GATE)();
  for (let i = 0; i < 5; i++) for (const fn of hooks.tick) fn();
  assert.deepEqual(sent, [{ go: 1 }]);
  me.ex.ride = { id: 'coaster', at: Date.now() + 150 };
  for (const fn of hooks.tick) fn();
  assert.deepEqual(sent, [{ go: 1 }]);
  me.x += 1;
  for (const fn of hooks.tick) fn();
  assert.deepEqual(sent, [{ go: 1 }, null]);
});
