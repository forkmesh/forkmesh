import { test } from 'node:test';
import assert from 'node:assert/strict';
import { RIDES, isWalkable, obj, PARK_X0 as K } from '../public/map.js';
import { ANCHORS as A } from '../public/park/art.js';
import { timeline, IDS, INFO, RACE_EVERY_MS } from '../public/features/rides/defs.js';

globalThis.addEventListener ??= () => {};
const { rideAt, inBoard, nearestAttraction, ATTRACTIONS } = await import('../public/features/rides.js');

test('every ride has a name, a gate and a timeline that ends where the server puts the rider', () => {
  for (const id of IDS) {
    assert.ok(INFO[id], id);
    const tl = timeline(id, 123456);
    assert.ok(tl.duration > 3000 && tl.duration < 90000, `${id} ${tl.duration}`);
    if (id === 'coaster') continue;
    const r = RIDES[id];
    assert.ok(r, id);
    const last = tl.s[tl.s.length - 1];
    const [ex, ey] = r.exit || [r.board[0], r.board[1]];
    if (r.exit) {
      assert.equal(Math.floor(last.x), ex, `${id} ends x`);
      assert.equal(Math.floor(last.y), ey, `${id} ends y`);
      assert.ok(isWalkable(ex, ey), `${id} exit walkable`);
    }
    assert.ok(last.lift < 0.05, `${id} lands`);
    const first = tl.s[0];
    const [x0, y0, x1, y1] = r.board;
    assert.ok(first.x >= x0 - 3 && first.x <= x1 + 4 && first.y >= y0 - 3 && first.y <= y1 + 4, `${id} starts near its board`);
  }
});

test('racers who board within the same window leave the gate together', () => {
  const at = 1_000_000;
  const launch = (id, t) => {
    const tl = timeline(id, t);
    const i = tl.s.findIndex((p) => p.phase === 'gate');
    let j = i;
    while (tl.s[j].phase === 'gate') j++;
    return t + j * 50;
  };
  const a = launch('race1', at), b = launch('race2', at + 400), c = launch('race3', at + 900);
  assert.ok(Math.abs(a - b) <= 50 && Math.abs(a - c) <= 50, `${a} ${b} ${c}`);
  assert.equal(a % RACE_EVERY_MS < 100 || a % RACE_EVERY_MS > RACE_EVERY_MS - 100, true);
});

test('a ride survives a browser clock that runs a little behind the server', () => {
  const o = { ex: { ride: { id: 'lazy', at: 10_000 } } };
  assert.ok(rideAt(o, 9_800));
  assert.equal(rideAt(o, 4_000), null);
  assert.ok(rideAt(o, 30_000));
  assert.equal(rideAt(o, 10_000 + timeline('lazy').duration + 10), null);
  assert.ok(rideAt({ ex: { ride: { at: 5_000 } } }, 5_000), 'old coaster extras still ride');
});

test('boarding areas match the server rule', () => {
  const [x0, y0, x1, y1] = RIDES['drive-park'].board;
  assert.ok(inBoard('drive-park', x0, y0) && inBoard('drive-park', x1, y1));
  assert.ok(!inBoard('drive-park', x1 + 1, y1));
  assert.ok(!inBoard('nope', x0, y0));
});

test('walking up to a ride offers its start button, and only near it', () => {
  const [fx, fy] = RIDES.flush.gate;
  assert.equal(nearestAttraction(fx - 1, fy).key, 'flush');
  const [sx0, sy0] = RIDES.drop.board;
  assert.equal(nearestAttraction(sx0, sy0).key, 'summit');
  const [cx, cy] = RIDES['drive-park'].gate;
  assert.equal(nearestAttraction(cx - 1, cy + 1).key, 'drive-park');
  assert.equal(nearestAttraction(30, 20), null);
  for (const a of ATTRACTIONS) assert.ok(RIDES[a.ride], a.key);
});

const screen = (p) => ({ x: p.x, y: p.y - p.lift });
const near = (a, b, d = 0.35) => Math.abs(a.x - b.x) <= d && Math.abs(a.y - b.y) <= d;

test('ride paths line up with the structures they are drawn on', () => {
  for (const id of ['drop', 'funnel', 'blaster']) {
    const s = timeline(id).s;
    const top = s.reduce((m, p) => (p.lift > m.lift ? p : m), s[0]);
    assert.ok(near(screen(top), A.crater), `${id} summit meets the crater`);
  }
  const inBowl = (p, b) => Math.hypot(p.x - b.x, p.y - b.y) <= b.r + 0.35 && p.lift <= 0.35;
  const spins = (id, b) => timeline(id).s.filter((p) => Math.hypot(p.x - b.x, p.y - b.y) <= b.r + 0.05);
  for (const [id, b] of [['funnel', A.funnel], ['flush', A.flush]]) {
    const ss = spins(id, b);
    assert.ok(ss.length > 40, `${id} spins in the bowl`);
    for (const p of ss) assert.ok(inBowl(p, b), `${id} stays inside the bowl at ${p.x.toFixed(2)},${p.y.toFixed(2)} lift ${p.lift.toFixed(2)}`);
  }
  const d = A.flushDeck;
  assert.ok(timeline('flush').s.some((p) => { const q = screen(p); return q.x >= d.x0 && q.x <= d.x1 && Math.abs(q.y - d.y) < 0.3; }), 'flush climbs onto the deck');
  for (let lane = 1; lane <= 4; lane++) {
    const gate = timeline(`race${lane}`, 7777).s.filter((p) => p.phase === 'gate');
    assert.ok(gate.length);
    for (const p of gate) {
      assert.ok(Math.abs(screen(p).y - A.racerPads) < 0.05, `race${lane} waits on its pad`);
      assert.equal(Math.floor(p.x), K + 91 + lane, `race${lane} lane`);
    }
  }
  const cliff = timeline('cliff').s;
  const peak = cliff.reduce((m, p) => (p.lift > m.lift && p.phase === '' && !p.wild ? p : m), cliff[0]);
  assert.ok(near(screen(peak), A.skullPeak), 'the dive leaves from the top of Skull Rock');
  const river = timeline('lazy').s.slice(30, -30);
  for (const p of river) {
    const tx = Math.floor(p.x - 0.01), ty = Math.floor(p.y - 0.01);
    const bridges = [[40, 18], [61, 18], [50, 9], [50, 28]];
    const underBridge = ([x, y]) => bridges.some(([bx, by]) => x >= K + bx && x <= K + bx + 1 && y >= by && y <= by + 1);
    const cell = [[tx, ty], [tx + 1, ty], [tx, ty + 1], [tx + 1, ty + 1]].some(([x, y]) => obj[y]?.[x] === '~' || underBridge([x, y]));
    assert.ok(cell, `the raft floats on the river at ${p.x},${p.y}`);
  }
});
