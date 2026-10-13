import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ROOM_ANCHORS, isWalkable } from '../public/map.js';
import { placeKey, encodePlace, readPlace, MAX_AGE_MS } from '../public/features/place/logic.js';

const NOW = 1_800_000_000_000;

test('a remembered tile comes back after a refresh, per organization', () => {
  const spot = ROOM_ANCHORS.l;
  assert.ok(isWalkable(spot.x, spot.y));
  const saved = encodePlace({ x: spot.x, y: spot.y, d: 6 }, NOW);
  assert.deepEqual(readPlace(saved, NOW + 1000), { x: spot.x, y: spot.y, d: 2 });
  assert.equal(placeKey('Acme'), 'office-place:acme');
  assert.equal(placeKey('<bad>'), 'office-place:_');
  assert.equal(placeKey(undefined), 'office-place:_');
});

test('stale, malformed or blocked places are ignored so the server spawns as before', () => {
  const spot = ROOM_ANCHORS.l;
  assert.equal(readPlace(encodePlace({ x: spot.x, y: spot.y, d: 0 }, NOW), NOW + MAX_AGE_MS + 1), null);
  assert.equal(readPlace('not json', NOW), null);
  assert.equal(readPlace(JSON.stringify({ x: 1.5, y: 2, at: NOW }), NOW), null);
  assert.equal(readPlace(JSON.stringify({ x: 0, y: 0, at: NOW }), NOW), null);
  assert.equal(readPlace(JSON.stringify({ x: spot.x, y: spot.y, at: NOW + 3600e3 }), NOW), null);
  assert.equal(readPlace(null, NOW), null);
});
