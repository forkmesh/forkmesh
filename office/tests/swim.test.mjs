import { test } from 'node:test';
import assert from 'node:assert/strict';
import { W, OFFICE_W, obj, isWalkable, zoneAt, WATER, ROOM_ANCHORS } from '../public/map.js';
import { inWater, strokePose, nearestWater, waterTiles, seedOf } from '../public/features/swim/logic.js';

test('the garden pond can be walked into and is still in the garden', () => {
  const tiles = waterTiles().filter(([x]) => x < OFFICE_W);
  assert.equal(tiles.length, 8);
  for (const [x, y] of tiles) {
    assert.equal(obj[y][x], WATER);
    assert.ok(isWalkable(x, y), `${x},${y}`);
    assert.equal(zoneAt(x, y), 'p');
  }
});

test('a person on a pond tile swims, including mid-step', () => {
  const [x, y] = waterTiles()[0];
  assert.ok(inWater({ x, y }));
  assert.ok(inWater({ x: x - 1, y, rx: x - 0.4, ry: y }));
  assert.ok(!inWater({ x: x - 1, y, rx: x - 0.6, ry: y }));
  assert.ok(!inWater(ROOM_ANCHORS.p));
});

test('strokes alternate arms and each swimmer has a steady phase', () => {
  const seed = seedOf('abc');
  assert.equal(seed, seedOf('abc'));
  const poses = new Set();
  for (let t = 0; t < 2000; t += 100) poses.add(strokePose(t, seed));
  assert.deepEqual([...poses].sort(), [4, 5]);
});

test('Go for a swim picks the nearest free pond tile', () => {
  const tiles = waterTiles();
  const [x, y] = tiles[0];
  assert.deepEqual(nearestWater({ x: x - 3, y }), { x, y });
  const taken = new Set([y * W + x]);
  const next = nearestWater({ x: x - 3, y }, taken);
  assert.ok(next && !(next.x === x && next.y === y));
  assert.equal(nearestWater({ x, y }, new Set(tiles.map(([a, b]) => b * W + a))), null);
});
