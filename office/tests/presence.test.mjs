import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { cleanStatus, effective, statusLine, badgeEmoji, focusBlocks, shouldAutoAway, fmtDur, followStep, STATUS_COLOR, IDLE_MS } from '../public/features/presence/logic.js';

describe('status validation', () => {
  test('accepts every status and trims text to 60 characters', () => {
    for (const s of ['available', 'focus', 'meeting', 'away', 'break']) assert.deepEqual(cleanStatus({ s }), { s });
    const v = cleanStatus({ s: 'focus', emoji: ' 🎯 ', text: '  x'.repeat(50) });
    assert.equal(v.emoji, '🎯');
    assert.equal([...v.text].length, 60);
  });
  test('rejects bad shapes and letters as emoji', () => {
    assert.equal(cleanStatus(null), undefined);
    assert.equal(cleanStatus('focus'), undefined);
    assert.equal(cleanStatus({ s: 'sleeping' }), undefined);
    assert.equal(cleanStatus({ s: 'focus', emoji: 'abc' }), undefined);
    assert.equal(cleanStatus({ s: 'focus', emoji: '<' }), undefined);
  });
  test('keeps one grapheme of a compound emoji and strips control characters', () => {
    assert.equal(cleanStatus({ s: 'away', emoji: '👩‍💻🎉' }).emoji, '👩‍💻');
    assert.equal(cleanStatus({ s: 'away', text: 'a\u0000\nb' }).text, 'a b');
  });
  test('auto flag only survives on away', () => {
    assert.equal(cleanStatus({ s: 'away', auto: true }).auto, true);
    assert.equal(cleanStatus({ s: 'focus', auto: true }).auto, undefined);
  });
});

describe('effective status', () => {
  test('meeting extra wins over a picked status except away', () => {
    assert.equal(effective({ ex: { meeting: { table: 't1' }, status: { s: 'focus' } } }), 'meeting');
    assert.equal(effective({ ex: { meeting: { table: 't1' }, status: { s: 'away' } } }), 'away');
    assert.equal(effective({ ex: {}, away: true }), 'away');
    assert.equal(effective({ ex: {} }), 'available');
  });
  test('status line, badge emoji and colours', () => {
    assert.equal(statusLine({ ex: { status: { s: 'break', text: 'lunch' } } }), 'On a break: lunch');
    assert.equal(badgeEmoji({ ex: { status: { s: 'focus' } } }), '🎧');
    assert.equal(badgeEmoji({ ex: { status: { s: 'focus', emoji: '🎯' } } }), '🎯');
    assert.equal(badgeEmoji({ ex: {} }), '');
    assert.equal(STATUS_COLOR.meeting, '#3b45e0');
  });
  test('focus blocks calls unless both sit at the same meeting table', () => {
    const f = { ex: { status: { s: 'focus' } } };
    const a = { ex: {} };
    assert.equal(focusBlocks(f, a), true);
    assert.equal(focusBlocks(a, f), true);
    assert.equal(focusBlocks(a, a), false);
    const ft = { ex: { status: { s: 'focus' }, meeting: { table: 'x' } } };
    assert.equal(focusBlocks(ft, { ex: { meeting: { table: 'x' } } }), false);
    assert.equal(focusBlocks(ft, { ex: { meeting: { table: 'y' } } }), false);
    assert.equal(focusBlocks({ ex: { status: { s: 'focus' } } }, { ex: { meeting: { table: 'y' } } }), true);
  });
});

describe('auto away and follow helpers', () => {
  test('idle or hidden for ten minutes', () => {
    const now = 1e9;
    assert.equal(shouldAutoAway(now, now - IDLE_MS + 1, null), false);
    assert.equal(shouldAutoAway(now, now - IDLE_MS, null), true);
    assert.equal(shouldAutoAway(now, now, now - IDLE_MS), true);
    assert.equal(shouldAutoAway(now, now, now - 1000), false);
  });
  test('durations', () => {
    assert.equal(fmtDur(20e3), '<1m');
    assert.equal(fmtDur(5 * 60e3), '5m');
    assert.equal(fmtDur(125 * 60e3), '2h 05m');
  });
  test('follow re-walks when the target moves and waits when adjacent', () => {
    assert.equal(followStep({ x: 0, y: 0 }, null, null, 0, 0), 'stop');
    assert.equal(followStep({ x: 0, y: 0 }, { x: 5, y: 0 }, { x: 4, y: 0, at: 0 }, 3, 100), 'walk');
    assert.equal(followStep({ x: 0, y: 0 }, { x: 5, y: 0 }, { x: 5, y: 0, at: 0 }, 3, 100), 'wait');
    assert.equal(followStep({ x: 4, y: 0 }, { x: 5, y: 0 }, { x: 6, y: 0, at: 0 }, 0, 100), 'wait');
    assert.equal(followStep({ x: 0, y: 0 }, { x: 5, y: 0 }, { x: 5, y: 0, at: 0 }, 0, 5000), 'walk');
  });
});
