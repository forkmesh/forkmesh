import { test } from 'node:test';
import assert from 'node:assert/strict';
import { TILE, ZONES, ROOMS, ROOM_ANCHORS, LABELS, floor, isWalkable, zoneAt, chairDir } from '../public/map.js';
import { focusRoom, roomRect, fade, ease, occupants, linkedRoom, roomLink, FADE_MS } from '../public/features/rooms/logic.js';
import { TABLE_BY_ID } from '../public/features/meetings/tables.js';

test('only private rooms with walls get a focus field', () => {
  assert.equal(focusRoom('c'), 'c');
  assert.equal(focusRoom('a'), 'a');
  assert.equal(focusRoom('b'), 'b');
  for (const z of ['.', 'l', 'p', 'g', '#', '', undefined, 'zz']) assert.equal(focusRoom(z), null);
  for (const z of Object.keys(ZONES)) if (focusRoom(z)) assert.ok(ROOMS[z]);
});

test('the lit rectangle is the room with its walls, and holds every tile of the zone', () => {
  const r = ROOMS.c;
  assert.deepEqual(roomRect('c'), { x: r.x0 * TILE, y: r.y0 * TILE, w: (r.x1 - r.x0 + 1) * TILE, h: (r.y1 - r.y0 + 1) * TILE });
  for (const z of Object.keys(ROOMS)) {
    if (!focusRoom(z)) continue;
    const b = ROOMS[z];
    for (let y = 0; y < floor.length; y++)
      for (let x = 0; x < floor[y].length; x++)
        if (floor[y][x] === z) assert.ok(x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1, `${z} ${x},${y}`);
  }
});

test('the Coffee Huddle has a side door, a two-tile back wall and a four-seat round table', () => {
  const r = ROOMS.c;
  const doors = [];
  for (let y = r.y0; y <= r.y1; y++) if (floor[y][r.x1] === 'c') doors.push(y);
  assert.equal(doors.length, 2);
  for (const y of doors) {
    assert.ok(isWalkable(r.x1, y));
    assert.ok(isWalkable(r.x1 + 1, y));
    assert.ok(isWalkable(r.x1 - 1, y));
  }
  for (let x = r.x0; x <= r.x1; x++) {
    assert.equal(floor[r.y0][x], '#');
    assert.equal(floor[r.y0 + 1][x], '#');
  }
  const t = [...TABLE_BY_ID.values()].find((x) => x.zone === 'c');
  assert.equal(t.tiles.length, 1);
  assert.deepEqual(t.chairs.map(([x, y]) => chairDir(x, y)).sort(), [0, 1, 2, 3]);
  assert.ok(isWalkable(ROOM_ANCHORS.c.x, ROOM_ANCHORS.c.y));
  assert.equal(zoneAt(ROOM_ANCHORS.c.x, ROOM_ANCHORS.c.y), 'c');
});

test('fade moves linearly, clamps, and eases at both ends', () => {
  assert.equal(fade(0, true, FADE_MS / 2), 0.5);
  assert.equal(fade(0.5, true, FADE_MS * 9), 1);
  assert.equal(fade(0.5, false, FADE_MS / 4), 0.25);
  assert.equal(fade(0.2, false, FADE_MS), 0);
  assert.equal(fade(0.4, true, -50), 0.4);
  assert.equal(ease(0), 0);
  assert.equal(ease(1), 1);
  assert.equal(ease(0.5), 0.5);
  assert.ok(ease(0.25) < 0.25 && ease(0.75) > 0.75);
});

test('occupants lists you first, then the people in the same room by name', () => {
  const at = (zone) => {
    for (let y = 0; y < floor.length; y++) for (let x = 0; x < floor[y].length; x++) if (floor[y][x] === zone) return { x, y };
    return null;
  };
  const c = at('c'), h = at('.');
  const me = { id: 'm', name: 'kaif', ...c };
  const others = [{ id: '1', name: 'zed', ...c }, { id: '2', name: 'out', ...h }, { id: '3', name: 'ann', ...c }];
  assert.deepEqual(occupants(me, others, 'c', zoneAt).map((o) => o.name), ['kaif', 'ann', 'zed']);
  assert.deepEqual(occupants(null, new Set(others).values(), 'c', zoneAt).map((o) => o.name), ['ann', 'zed']);
});

test('room links only name known rooms and keep the organization', () => {
  assert.equal(linkedRoom('?org=acme&room=c'), 'c');
  assert.equal(linkedRoom('?room=l'), 'l');
  for (const q of ['', '?room=', '?room=zz', '?room=__proto__', '?room=constructor', '?room=%23', '?room=g'])
    assert.equal(linkedRoom(q), null, q);
  assert.equal(roomLink('https://app.forkmesh.com/office?org=acme#x', 'c'), 'https://app.forkmesh.com/office?org=acme&room=c');
  assert.equal(roomLink('https://app.forkmesh.com/office?org=acme&room=a', 'c'), 'https://app.forkmesh.com/office?org=acme&room=c');
});

test('no room label sits on a door', () => {
  const doors = [];
  for (const [z, r] of Object.entries(ROOMS)) {
    if (z === '.') continue;
    for (let y = r.y0; y <= r.y1; y++)
      for (let x = r.x0; x <= r.x1; x++) {
        const edge = x === r.x0 || x === r.x1 || y === r.y0 || y === r.y1;
        if (edge && floor[y][x] === z && [[1, 0], [-1, 0], [0, 1], [0, -1]].some(([dx, dy]) => floor[y + dy]?.[x + dx] === '#')) doors.push([x, y]);
      }
  }
  assert.ok(doors.length >= 10);
  for (const l of LABELS)
    for (const [x, y] of doors)
      assert.ok(Math.abs(l.y - (y + 0.5)) >= 1 || Math.abs(l.x - (x + 0.5)) >= 1.5, `${l.text} covers the door at ${x},${y}`);
});
