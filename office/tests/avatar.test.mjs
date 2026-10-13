import { test } from 'node:test';
import assert from 'node:assert/strict';
import { AV_MAX, AV_CUSTOM, AV_COMBOS, FIELDS, SKIN, HAIR_COLOR, TOP, cleanAvatar, packAvatar, unpackAvatar, lookOf, randomLook } from '../public/avatar.js';

const seq = (seed) => () => {
  seed = (seed * 1103515245 + 12345) % 2147483648;
  return seed / 2147483648;
};

test('only hashed codes and packed custom codes are valid avatars', () => {
  assert.equal(AV_COMBOS, 7077888);
  assert.equal(cleanAvatar(0), 0);
  assert.equal(cleanAvatar(AV_MAX - 1), AV_MAX - 1);
  assert.equal(cleanAvatar(AV_MAX), undefined);
  assert.equal(cleanAvatar(AV_CUSTOM - 1), undefined);
  assert.equal(cleanAvatar(AV_CUSTOM), AV_CUSTOM);
  assert.equal(cleanAvatar(AV_CUSTOM + AV_COMBOS - 1), AV_CUSTOM + AV_COMBOS - 1);
  assert.equal(cleanAvatar(AV_CUSTOM + AV_COMBOS), undefined);
  for (const bad of [-1, 1.5, '7', null, undefined, NaN, true, {}]) assert.equal(cleanAvatar(bad), undefined);
});

test('every look packs to a valid code and unpacks to itself', () => {
  const rng = seq(7);
  for (let i = 0; i < 500; i++) {
    const look = randomLook(rng);
    for (const [key, size] of FIELDS) assert.ok(Number.isInteger(look[key]) && look[key] >= 0 && look[key] < size, key);
    const av = packAvatar(look);
    assert.equal(cleanAvatar(av), av);
    assert.deepEqual(unpackAvatar(av), look);
    assert.deepEqual(lookOf(av), look);
  }
  const top = Object.fromEntries(FIELDS.map(([k, n]) => [k, n - 1]));
  assert.equal(packAvatar(top), AV_CUSTOM + AV_COMBOS - 1);
  assert.equal(packAvatar({}), AV_CUSTOM);
  assert.deepEqual(unpackAvatar(packAvatar({ skin: -1, hair: 99 })).skin, SKIN.length - 1);
});

test('hashed codes keep the look they always had', () => {
  for (const av of [0, 1, 34, 279, 280, 4242, 9999]) {
    const look = lookOf(av);
    assert.equal(look.skin, av % 5);
    assert.equal(look.hairColor, ((av / 5) | 0) % 7);
    assert.equal(look.top, ((av / 35) | 0) % 8);
    assert.equal(look.hair, ((av / 280) | 0) % 3);
    assert.deepEqual([look.bottom, look.face, look.extra, look.shoes], [0, 0, 0, 0]);
  }
  assert.equal(SKIN[0], '#f7d3b0');
  assert.equal(HAIR_COLOR[6], '#b8453a');
  assert.equal(TOP[7], '#ec5f9b');
  assert.equal(lookOf(undefined).skin, 0);
});
