import { test } from 'node:test';
import assert from 'node:assert/strict';
import { W, H, obj, floor, isWalkable, chairDir, zoneAt, ZONES, ROOMS, ROOM_ANCHORS, FIRE, FIRE_ZONE, FIRE_SEATS, LABELS } from '../public/map.js';
import { SEATS, cleanTop, topByName, seatPlan, describeTop, accountOf } from '../public/features/campfire/logic.js';

const fire = (() => {
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (obj[y][x] === FIRE) return { x, y };
  return null;
})();

test('the campfire is a private zone beside the garden with eight log seats facing the fire', () => {
  assert.deepEqual(ZONES[FIRE_ZONE], { name: 'Campfire', private: true });
  assert.ok(ROOMS[FIRE_ZONE]);
  assert.equal(zoneAt(fire.x, fire.y), FIRE_ZONE);
  assert.ok(!isWalkable(fire.x, fire.y));
  assert.equal(FIRE_SEATS.length, 8);
  assert.equal(SEATS, 8);
  const facing = [[0, 1], [0, -1], [-1, 0], [1, 0]];
  for (const [x, y] of FIRE_SEATS) {
    assert.equal(floor[y][x], FIRE_ZONE);
    assert.ok(isWalkable(x, y));
    const [fx, fy] = facing[chairDir(x, y)];
    const before = Math.abs(fire.x - x) + Math.abs(fire.y - y);
    assert.ok(Math.abs(fire.x - x - fx) + Math.abs(fire.y - y - fy) < before, `${x},${y} faces the fire`);
  }
  assert.ok(LABELS.some((l) => l.text === 'Campfire'));
  const a = ROOM_ANCHORS[FIRE_ZONE];
  assert.equal(floor[a.y][a.x], FIRE_ZONE);
});

test('the campfire can be reached from the garden', () => {
  const start = ROOM_ANCHORS.p;
  const seen = new Set([start.y * W + start.x]);
  const q = [[start.x, start.y]];
  while (q.length) {
    const [x, y] = q.pop();
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const nx = x + dx, ny = y + dy, k = ny * W + nx;
      if (seen.has(k) || !isWalkable(nx, ny)) continue;
      seen.add(k);
      q.push([nx, ny]);
    }
  }
  for (const [x, y] of FIRE_SEATS) assert.ok(seen.has(y * W + x));
});

test('the top list is validated and ordered by rank', () => {
  const top = cleanTop([
    { n: 'bo', r: 2, t: 'ember', s: 55 },
    { n: 'ann', r: 1, t: 'blaze', s: 140 },
    { n: '<x>', r: 3, t: 'blaze', s: 1 },
    { n: 'cy', r: 9, t: 'blaze', s: 1 },
    { n: 'dee', r: 2, t: 'blaze', s: 1 },
    { n: 'eve', r: 4, t: 'hot', s: 1 },
    null,
  ]);
  assert.deepEqual(top, [{ n: 'ann', r: 1, t: 'blaze', s: 100 }, { n: 'bo', r: 2, t: 'ember', s: 55 }]);
  assert.deepEqual(cleanTop('nope'), []);
  assert.equal(topByName(top).get('ann').r, 1);
  assert.match(describeTop(top), /#1 ann \(Blazing, 100\)/);
  assert.match(describeTop([]), /Nobody/);
});

test('each rank owns a seat, and present members are matched by account', () => {
  const top = cleanTop([{ n: 'Ann', r: 1, t: 'blaze', s: 80 }, { n: 'bo', r: 3, t: 'ember', s: 50 }]);
  const plan = seatPlan(top, [{ ex: { fm: { a: 'ann' } } }, { name: 'bo', ex: {} }]);
  assert.deepEqual(plan.map((s) => [s.x, s.y]), [FIRE_SEATS[0], FIRE_SEATS[2]]);
  assert.deepEqual(plan.map((s) => s.here), [true, false]);
  assert.equal(accountOf({ name: 'x' }), '');
});
