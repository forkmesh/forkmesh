import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CLAP_MS, CONFETTI, poseFor, burst, stepParticles, canHear } from '../public/features/cheer/logic.js';
import { cleanSound, applauseHits } from '../public/sound.js';

const seq = (seed) => () => {
  seed = (seed * 1103515245 + 12345) % 2147483648;
  return seed / 2147483648;
};

test('clapping alternates two arm poses and dancing turns and steps', () => {
  assert.equal(poseFor({}, 1000), null);
  assert.equal(poseFor(null, 1000), null);
  const clap = { clapUntil: 1000 + CLAP_MS };
  assert.deepEqual(poseFor(clap, 1000), { pose: 1 });
  assert.deepEqual(poseFor(clap, 1150), { pose: 2 });
  assert.deepEqual(poseFor(clap, 1300), { pose: 1 });
  assert.equal(poseFor(clap, 1000 + CLAP_MS), null);
  const dance = { danceUntil: 5000 };
  const seen = new Set();
  for (let t = 0; t < 960; t += 240) {
    const p = poseFor(dance, t);
    assert.equal(p.pose, 3);
    assert.ok([1, 2].includes(p.step));
    seen.add(p.dir);
  }
  assert.deepEqual([...seen].sort(), [0, 2, 3]);
  assert.deepEqual(poseFor({ clapUntil: 9000, danceUntil: 9000 }, 0).pose, 1);
});

test('confetti rises, falls and expires', () => {
  const parts = burst(100, 200, 40, seq(3));
  assert.equal(parts.length, 40);
  for (const p of parts) {
    assert.ok(p.vy < 0.03 && CONFETTI.includes(p.c) && p.life >= 900 && p.life <= 1800);
    assert.ok(p.s === 2 || p.s === 3);
  }
  const first = { ...parts[0] };
  stepParticles(parts, 16);
  assert.ok(parts[0].vy > first.vy);
  assert.notEqual(parts[0].x, first.x);
  for (let i = 0; i < 200; i++) stepParticles(parts, 16);
  assert.equal(parts.length, 0);
});

test('claps are heard in the same room or within eight tiles', () => {
  const me = { x: 10, y: 10 };
  assert.equal(canHear(me, me, false), true);
  assert.equal(canHear(me, { x: 40, y: 30 }, true), true);
  assert.equal(canHear(me, { x: 18, y: 10 }, false), true);
  assert.equal(canHear(me, { x: 19, y: 10 }, false), false);
  assert.equal(canHear(null, me, true), false);
});

test('sound settings are clamped and applause swells then fades', () => {
  assert.deepEqual(cleanSound(null), { on: true, vol: 0.6 });
  assert.deepEqual(cleanSound({ on: false, vol: 3 }), { on: false, vol: 1 });
  assert.deepEqual(cleanSound({ on: 0, vol: -1 }), { on: true, vol: 0 });
  assert.deepEqual(cleanSound({ vol: 'loud' }), { on: true, vol: 0.6 });
  const hits = applauseHits(2.8, 6, seq(11));
  assert.ok(hits.length > 20 && hits.length < 90);
  for (let i = 1; i < hits.length; i++) assert.ok(hits[i - 1].t <= hits[i].t);
  for (const h of hits) assert.ok(h.t >= 0 && h.t < 2.8 && h.gain > 0 && h.gain < 0.5 && h.freq >= 900 && h.freq <= 2400 && Math.abs(h.pan) <= 0.8);
  const middle = hits.filter((h) => h.t > 0.5 && h.t < 1.6).length;
  const tail = hits.filter((h) => h.t > 2.4).length;
  assert.ok(middle > tail);
  assert.ok(applauseHits(2.8, 100, seq(1)).length <= 14 * 2.8 * 5);
});
