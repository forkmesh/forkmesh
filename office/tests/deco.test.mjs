import { test } from 'node:test';
import assert from 'node:assert/strict';
import { SLOTS, SLOT_LABEL, ITEMS, DESK_COLORS, DESK_NAMES, CHAIR_COLORS, CHAIR_NAMES, CROP, EMPTY, SPRITE_W, SPRITE_H, cleanDeco, decoEmpty, itemsFor, shade, drawDeco, sampleFor } from '../public/features/work/deco.js';
import { deskList } from '../public/features/work/logic.js';

function recorder() {
  const rects = [];
  return {
    rects,
    set fillStyle(v) { this.color = v; },
    fillRect(x, y, w, h) { rects.push({ color: this.color, x, y, w, h }); },
  };
}

test('the catalog is consistent', () => {
  assert.equal(SLOTS.length, 4);
  assert.equal(SLOT_LABEL.length, SLOTS.length);
  assert.equal(DESK_COLORS.length, DESK_NAMES.length);
  assert.equal(CHAIR_COLORS.length, CHAIR_NAMES.length);
  assert.equal(ITEMS.length - 1, 14);
  assert.equal(ITEMS[0], null);
  const names = new Set();
  for (const it of ITEMS.slice(1)) {
    assert.ok(SLOTS.includes(it.kind) && it.name && CROP[it.kind]);
    names.add(it.name);
  }
  assert.equal(names.size, 14);
  assert.deepEqual(itemsFor(0).map((i) => i.id), [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.deepEqual(itemsFor(1).map((i) => i.id), itemsFor(0).map((i) => i.id));
  assert.deepEqual(itemsFor(2).map((i) => i.id), [9, 10, 11]);
  assert.deepEqual(itemsFor(3).map((i) => i.id), [12, 13, 14]);
});

test('decorations are validated slot by slot', () => {
  assert.deepEqual(cleanDeco({ desk: 2, chair: 3, slots: [1, 5, 10, 14] }), { desk: 2, chair: 3, slots: [1, 5, 10, 14] });
  assert.deepEqual(cleanDeco({ slots: [0, 0, 0, 0] }), EMPTY);
  assert.equal(cleanDeco({ slots: [9, 0, 0, 0] }), undefined);
  assert.equal(cleanDeco({ slots: [0, 0, 0, 1] }), undefined);
  assert.equal(cleanDeco({ desk: DESK_COLORS.length, slots: [0, 0, 0, 0] }), undefined);
  assert.equal(cleanDeco({ chair: -1, slots: [0, 0, 0, 0] }), undefined);
  assert.equal(cleanDeco({ slots: [0, 0, 0] }), undefined);
  assert.equal(cleanDeco('x'), undefined);
  assert.equal(decoEmpty(EMPTY), true);
  assert.equal(decoEmpty(undefined), true);
  assert.equal(decoEmpty({ desk: 0, chair: 1, slots: [0, 0, 0, 0] }), false);
  assert.deepEqual(deskList({ a: { x: 1, y: 2, name: 'A', deco: EMPTY }, b: { x: 3, y: 4, name: 'B' } }), [{ x: 1, y: 2, name: 'A', deco: EMPTY }, { x: 3, y: 4, name: 'B' }]);
});

test('every item, desk and chair draws inside the sprite', () => {
  const empty = recorder();
  drawDeco(empty, EMPTY);
  assert.equal(empty.rects.length, 0);
  for (let id = 1; id < ITEMS.length; id++) {
    const c = recorder();
    drawDeco(c, sampleFor(id));
    assert.ok(c.rects.length >= 4, ITEMS[id].name);
    for (const r of c.rects) {
      assert.ok(r.x >= 0 && r.y >= 0 && r.x + r.w <= SPRITE_W && r.y + r.h <= SPRITE_H, `${ITEMS[id].name} stays inside`);
      assert.match(r.color, /^#[0-9a-f]{6}([0-9a-f]{2})?$/i);
    }
    const [sx, sy, sw, sh] = CROP[ITEMS[id].kind];
    assert.ok(c.rects.some((r) => r.x >= sx && r.y >= sy && r.x + r.w <= sx + sw && r.y + r.h <= sy + sh), `${ITEMS[id].name} is in its thumbnail`);
  }
  for (let i = 1; i < DESK_COLORS.length; i++) {
    const c = recorder();
    drawDeco(c, { desk: i, chair: 0, slots: [0, 0, 0, 0] });
    assert.ok(c.rects.some((r) => r.color === DESK_COLORS[i][0]));
  }
  for (let i = 1; i < CHAIR_COLORS.length; i++) {
    const c = recorder();
    drawDeco(c, { desk: 0, chair: i, slots: [0, 0, 0, 0] });
    assert.ok(c.rects.some((r) => r.color === CHAIR_COLORS[i]));
  }
  assert.equal(shade('#808080', 0.5), '#404040');
  assert.equal(shade('#ffffff', 2), '#ffffff');
});
