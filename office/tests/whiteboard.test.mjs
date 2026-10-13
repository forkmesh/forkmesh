import test from 'node:test';
import assert from 'node:assert/strict';
import { obj, isWalkable, zoneAt } from '../public/map.js';
import {
  BW, BH, MAX_PTS, MAX_ITEMS, sanitizeItem, applyOp, newBoard, loadBoard, boardItems, simplify, splitPoints,
  thumbOf, chunkItems, bucket, findBoards, parseBoardId, hitTest, bounds, moveItem,
} from '../public/features/whiteboard/model.js';


const stroke = (id, n = 5, extra = {}) => ({ id, k: 's', c: '#1d2030', w: 4, pts: Array.from({ length: n }, (_, i) => [100 + i * 10, 100 + i * 5, 5]).flat(), ...extra });

test('findBoards locates the whiteboard tiles in Engineering and Design', () => {
  const boards = findBoards(obj);
  assert.equal(boards.length, 2);
  const zones = boards.map((b) => { const p = parseBoardId(b); return zoneAt(p.x, p.y); }).sort();
  assert.deepEqual(zones, ['a', 'b']);
  assert.equal(parseBoardId('x.y'), null);
  assert.equal(parseBoardId('1.2.3'), null);
});

test('sanitizeItem cleans and rejects items', () => {
  assert.equal(sanitizeItem(null), null);
  assert.equal(sanitizeItem({ id: 'a', k: 'zz' }), null);
  assert.equal(sanitizeItem({ id: 'bad id!', k: 's', pts: [1, 2, 3] }), null);
  assert.equal(sanitizeItem({ id: 'a', k: 's', pts: [1, 2] }), null);
  assert.equal(sanitizeItem({ id: 'a', k: 's', pts: new Array(MAX_PTS + 3).fill(1) }), null);
  assert.equal(sanitizeItem({ id: 'a', k: 's', pts: [1, 'x', 3] }), null);
  const s = sanitizeItem({ id: 'a', k: 's', c: 'red', w: 999, pts: [99999, -99999, 42, 5.6, 7.4, 3], evil: 1, hl: true });
  assert.equal(s.c, '#1d2030');
  assert.equal(s.w, 60);
  assert.deepEqual(s.pts, [BW + 50, -50, 9, 6, 7, 3]);
  assert.equal(s.hl, 1);
  assert.equal(s.evil, undefined);
  assert.equal(sanitizeItem({ id: 't', k: 't', x: 1, y: 1, text: '   ' }), null);
  const t = sanitizeItem({ id: 't', k: 't', x: -5, y: 5000, text: 'x'.repeat(1000), sz: 3 });
  assert.equal(t.x, 0);
  assert.equal(t.y, BH);
  assert.equal(t.text.length, 280);
  assert.equal(t.sz, 10);
  assert.deepEqual(sanitizeItem({ id: 'n', k: 'n', x: 5, y: 6, c: '#FFF3A3', text: '' }), { id: 'n', k: 'n', c: '#fff3a3', x: 5, y: 6, text: '' });
  assert.equal(sanitizeItem({ id: 'r', k: 'r', x: 5, y: 6 }), null);
  assert.deepEqual(sanitizeItem({ id: 'r', k: 'a', x: 5, y: 6, x2: 9, y2: 10, w: 0 }), { id: 'r', k: 'a', c: '#1d2030', x: 5, y: 6, x2: 9, y2: 10, w: 1 });
});

test('applyOp add, move, update, delete and clear', () => {
  const b = newBoard();
  let r = applyOp(b, { o: 'add', items: [stroke('s1'), { id: 'n1', k: 'n', x: 10, y: 10, c: '#fff3a3', text: 'hi' }] }, 'u1');
  assert.ok(r.ok);
  assert.equal(b.items.size, 2);
  assert.equal(b.items.get('s1').z, 1);
  assert.equal(b.items.get('n1').z, 2);
  assert.equal(b.items.get('n1').by, 'u1');
  r = applyOp(b, { o: 'add', items: [stroke('s1')] });
  assert.equal(r.ok, false);
  r = applyOp(b, { o: 'mv', ids: ['n1', 'nope'], dx: 5, dy: -20 });
  assert.ok(r.ok);
  assert.deepEqual(r.op, { o: 'mv', ids: ['n1'], dx: 5, dy: -20 });
  assert.equal(b.items.get('n1').x, 15);
  assert.equal(b.items.get('n1').y, 0);
  r = applyOp(b, { o: 'upd', id: 'n1', text: 'hello\nthere' });
  assert.ok(r.ok);
  assert.equal(b.items.get('n1').text, 'hello\nthere');
  assert.equal(applyOp(b, { o: 'upd', id: 's1', text: 'x' }).ok, false);
  r = applyOp(b, { o: 'del', ids: ['s1', 'ghost'] });
  assert.deepEqual(r.del, ['s1']);
  assert.equal(b.items.size, 1);
  const bytes = b.bytes;
  assert.ok(bytes > 0);
  r = applyOp(b, { o: 'clear' });
  assert.deepEqual(r.del, ['n1']);
  assert.equal(b.items.size, 0);
  assert.equal(b.bytes, 0);
  assert.equal(applyOp(b, { o: 'nuke' }).err, 'bad');
  assert.equal(applyOp(b, { o: 'mv', ids: ['a'], dx: 0, dy: 0 }).err, 'bad');
  assert.equal(applyOp(b, { o: 'add', items: new Array(61).fill(stroke('q')) }).err, 'bad');
});

test('applyOp refuses to grow a board past its item cap', () => {
  const b = newBoard();
  for (let i = 0; i < MAX_ITEMS; i += 50) applyOp(b, { o: 'add', items: Array.from({ length: 50 }, (_, j) => stroke(`i${i + j}`, 2)) });
  assert.equal(b.items.size, MAX_ITEMS);
  const r = applyOp(b, { o: 'add', items: [stroke('over', 2)] });
  assert.equal(r.ok, false);
  assert.equal(r.err, 'full');
});

test('loadBoard restores z order and byte count', () => {
  const b = newBoard();
  applyOp(b, { o: 'add', items: [stroke('a'), stroke('b'), stroke('c')] });
  const stored = boardItems(b).reverse().map((it) => JSON.parse(JSON.stringify(it)));
  const l = loadBoard(stored);
  assert.deepEqual(boardItems(l).map((i) => i.id), ['a', 'b', 'c']);
  assert.equal(l.z, 3);
  assert.equal(l.bytes, b.bytes);
  applyOp(l, { o: 'add', items: [stroke('d')] });
  assert.equal(l.items.get('d').z, 4);
});

test('simplify and splitPoints keep strokes small enough for one message', () => {
  const line = Array.from({ length: 500 }, (_, i) => [i, i * 2, 5]).flat();
  const s = simplify(line, 0.8);
  assert.deepEqual(s, [0, 0, 5, 499, 998, 5]);
  const wiggle = Array.from({ length: 3000 }, (_, i) => [i % 1600, Math.round(500 + 300 * Math.sin(i / 7)), (i % 9)]).flat();
  const parts = splitPoints(wiggle);
  assert.ok(parts.length > 1);
  for (const p of parts) {
    assert.ok(p.length <= MAX_PTS);
    assert.equal(p.length % 3, 0);
    assert.ok(JSON.stringify({ t: 'wb.op', b: '17.9', s: 1, op: { o: 'add', items: [stroke('x', 1, { pts: p })] } }).length < 16384);
  }
  for (let i = 1; i < parts.length; i++) assert.deepEqual(parts[i].slice(0, 3), parts[i - 1].slice(-3));
});

test('hitTest, bounds and moveItem agree', () => {
  const r = { id: 'r', k: 'r', c: '#000000', x: 100, y: 100, x2: 200, y2: 150, w: 4 };
  assert.ok(hitTest(r, 100, 125));
  assert.equal(hitTest(r, 150, 125), false);
  const s = sanitizeItem(stroke('s'));
  assert.ok(hitTest(s, 120, 110));
  assert.equal(hitTest(s, 500, 500), false);
  const b0 = bounds(s);
  moveItem(s, 10, 20);
  const b1 = bounds(s);
  assert.equal(b1.x0 - b0.x0, 10);
  assert.equal(b1.y0 - b0.y0, 20);
});

test('thumbOf, chunkItems and bucket', () => {
  const b = newBoard();
  applyOp(b, { o: 'add', items: [stroke('a', 40), { id: 't', k: 't', x: 0, y: 0, text: 'hi', c: '#e5484d' }, { id: 'e', k: 'e', x: 0, y: 0, x2: BW, y2: BH, c: '#3b45e0' }] });
  const th = thumbOf(b);
  assert.equal(th.length, 3);
  assert.equal(th[0][0], 's');
  assert.ok(th[0][2].length <= 28);
  assert.deepEqual(th[2], ['e', '#3b45e0', [0, 0, 63, 63]]);
  const items = Array.from({ length: 30 }, (_, i) => sanitizeItem(stroke(`c${i}`, 300)));
  const chunks = chunkItems(items, 20000);
  assert.ok(chunks.length > 1);
  assert.equal(chunks.flat().length, 30);
  assert.deepEqual(chunkItems([]), [[]]);
  const st = {};
  let ok = 0;
  for (let i = 0; i < 20; i++) if (bucket(st, 'k', 5, 1, 1, 1000)) ok++;
  assert.equal(ok, 5);
  assert.equal(bucket(st, 'k', 5, 1, 1, 3000), true);
});
