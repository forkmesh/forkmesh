import { test } from 'node:test';
import assert from 'node:assert/strict';
import { W, H, OFFICE_W, PARK_X0, RIDES, ZONES, ROOMS, ROOM_ANCHORS, LABELS, DECOR, CHAIRS, SEATS, floor, obj, isWalkable, zoneAt, chairDir, randomSpawn } from '../public/map.js';
import { tableAtSeat } from '../public/features/meetings/tables.js';

const tiles = (zone) => {
  const out = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (floor[y][x] === zone) out.push([x, y]);
  return out;
};

function reachable(sx, sy) {
  const seen = new Set([sy * W + sx]);
  const q = [[sx, sy]];
  for (let i = 0; i < q.length; i++) {
    const [cx, cy] = q[i];
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const nx = cx + dx, ny = cy + dy, k = ny * W + nx;
      if (seen.has(k) || !isWalkable(nx, ny)) continue;
      seen.add(k);
      q.push([nx, ny]);
    }
  }
  return seen;
}

test('every floor tile has a named zone and every room an anchor inside it', () => {
  for (let y = 0; y < H; y++) for (const f of floor[y]) assert.ok(f === 'g' || f === '#' || ZONES[f], f);
  for (const [z, a] of Object.entries(ROOM_ANCHORS)) {
    assert.ok(ZONES[z], z);
    assert.ok(isWalkable(a.x, a.y), z);
    assert.equal(zoneAt(a.x, a.y), z);
  }
  for (const z of ['k', 'm', 's']) {
    assert.ok(ZONES[z].private, z);
    assert.ok(ROOM_ANCHORS[z], z);
    assert.ok(ROOMS[z], z);
    assert.ok(LABELS.some((l) => l.text === ZONES[z].name), z);
  }
});

test('every seat, and every free tile of the new rooms, can be reached from the spawn', () => {
  const s = randomSpawn();
  const seen = reachable(s.x, s.y);
  for (const z of ['k', 'm', 's']) for (const [x, y] of tiles(z)) if (isWalkable(x, y)) assert.ok(seen.has(y * W + x), `${z} ${x},${y}`);
  const [px, py] = RIDES['drive-park'].exit;
  const park = reachable(px, py);
  for (const [x, y] of SEATS) assert.ok((x < OFFICE_W ? seen : park).has(y * W + x), `${x},${y}`);
});

test('the car links the office and the water park, and every ride boards from walkable ground', () => {
  const s = randomSpawn();
  const office = reachable(s.x, s.y);
  const [px, py] = RIDES['drive-park'].exit;
  const park = reachable(px, py);
  for (const t of park) assert.ok(t % W >= PARK_X0, `park reaches ${t % W}`);
  for (const [id, r] of Object.entries(RIDES)) {
    const [x0, y0, x1, y1] = r.board;
    const side = x0 < OFFICE_W ? office : park;
    let ok = false;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) if (side.has(y * W + x)) ok = true;
    assert.ok(ok, `${id} board`);
    if (r.exit) assert.ok((r.exit[0] < OFFICE_W ? office : park).has(r.exit[1] * W + r.exit[0]), `${id} exit`);
  }
});

test('the Library and the Media Room open onto the hallway, and the Media Room onto the Lobby', () => {
  const doors = (z) => tiles(z).filter(([x, y]) => x === ROOMS[z].x0 || x === ROOMS[z].x1 || y === ROOMS[z].y0 || y === ROOMS[z].y1);
  assert.deepEqual(doors('k'), [[51, 16], [52, 16]]);
  assert.deepEqual(doors('m'), [[51, 22], [52, 22], [44, 28], [44, 29]]);
  for (const [x, y] of doors('k')) assert.equal(zoneAt(x, y + 1), '.');
  for (const [x, y] of doors('m').slice(0, 2)) assert.equal(zoneAt(x, y - 1), '.');
  for (const [x, y] of doors('m').slice(2)) {
    assert.equal(zoneAt(x - 1, y), 'l');
    assert.ok(isWalkable(x - 1, y) && isWalkable(x + 1, y));
  }
  assert.equal(ROOMS.l.x1, ROOMS.m.x0);
});

test('the Lobby Couches are a rug inside the Lobby with lounge seats and no table call', () => {
  const rug = tiles('s');
  assert.equal(rug.length, 20);
  const b = ROOMS.s, l = ROOMS.l;
  assert.ok(b.x0 > l.x0 && b.x1 < l.x1 && b.y0 > l.y0 && b.y1 < l.y1);
  for (const [x, y] of rug) assert.ok(x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1);
  const seats = rug.filter(([x, y]) => chairDir(x, y) !== undefined);
  assert.deepEqual(seats.map(([x, y]) => obj[y][x]).sort(), ['c', 'c', 'd', 'n', 'n', 'o', 'u']);
  for (const [x, y] of seats) {
    assert.ok(isWalkable(x, y));
    assert.equal(tableAtSeat(x, y), null);
  }
  for (let i = 0; i < 50; i++) {
    const p = randomSpawn();
    assert.equal(zoneAt(p.x, p.y), 'l');
  }
});

test('lounge seats face the way their backs are drawn', () => {
  assert.deepEqual([CHAIRS.n, CHAIRS.u, CHAIRS.d, CHAIRS.c, CHAIRS.o], [0, 1, 2, 3, 0]);
  for (const [x, y] of SEATS) assert.equal(chairDir(x, y), CHAIRS[obj[y][x]]);
});

test('labels stay on the map and wall art hangs on a wall with a room under it', () => {
  for (const l of LABELS) assert.ok(l.x > 0 && l.x < W && l.y > 0 && l.y < H, l.text);
  assert.deepEqual(DECOR.map((d) => d.art).sort(), ['mondrian', 'squiggle', 'tv']);
  for (const d of DECOR) {
    assert.ok(Number.isInteger(d.y) && Number.isInteger(d.x * 2), d.art);
    for (const x of [Math.floor(d.x - 0.5), Math.ceil(d.x - 0.5)]) {
      assert.equal(floor[d.y][x], '#', d.art);
      assert.ok(ZONES[floor[d.y + 1][x]], d.art);
    }
  }
});
