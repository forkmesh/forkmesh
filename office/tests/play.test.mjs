import { test, describe, after } from 'node:test';
import assert from 'node:assert/strict';
import { obj } from '../public/map.js';
import * as L from '../public/features/play/logic.js';


describe('arcade score validation', () => {
  test('accepts plausible scores and rejects fast ones', () => {
    assert.equal(L.plausibleScore(0, 0), true);
    assert.equal(L.plausibleScore(1, 0), true);
    assert.equal(L.plausibleScore(2, 0), false);
    assert.equal(L.plausibleScore(4, 1000), true);
    assert.equal(L.plausibleScore(5, 1000), false);
    assert.equal(L.plausibleScore(31, 10000), true);
    assert.equal(L.plausibleScore(32, 10000), false);
  });
  test('rejects junk', () => {
    for (const s of [-1, 1.5, NaN, '3', null, L.ARCADE_MAX_SCORE + 1]) assert.equal(L.plausibleScore(s, 1e6), false);
    assert.equal(L.plausibleScore(1, -5), false);
    assert.equal(L.plausibleScore(1, L.ARCADE_MAX_MS + 1), false);
  });
  test('insertScore keeps one best per person, sorted, capped', () => {
    let list = [];
    let r = L.insertScore(list, { k: 'a', name: 'A', score: 5, ts: 1 });
    assert.equal(r.rank, 1);
    assert.equal(r.top, true);
    list = r.list;
    r = L.insertScore(list, { k: 'a', name: 'A', score: 3, ts: 2 });
    assert.equal(r.rank, 0);
    assert.equal(r.list, list);
    r = L.insertScore(list, { k: 'b', name: 'B', score: 5, ts: 3 });
    assert.equal(r.rank, 2);
    assert.equal(r.top, false);
    list = r.list;
    r = L.insertScore(list, { k: 'b', name: 'B', score: 9, ts: 4 });
    assert.equal(r.top, true);
    assert.deepEqual(r.list.map((s) => s.k), ['b', 'a']);
    list = [];
    for (let i = 0; i < 15; i++) list = L.insertScore(list, { k: 'p' + i, name: 'P', score: i, ts: i }).list;
    assert.equal(list.length, L.SCORES_KEEP);
    assert.equal(list[0].score, 14);
  });
});

describe('ping-pong physics', () => {
  const table = { id: 't', x0: 10, y0: 5, x1: 13, y1: 6 };
  const play = () => {
    const g = L.newPong(table);
    g.players = { L: 'a', R: 'b' };
    L.startPong(g, 0, () => 0.5);
    return g;
  };

  test('table detection and sides on the real map', () => {
    const ts = L.findTables(obj, 'G');
    assert.equal(ts.length, 1);
    const t = ts[0];
    assert.deepEqual([t.x1 - t.x0, t.y1 - t.y0], [3, 1]);
    assert.equal(L.pongSide(t, t.x0 - 1, t.y0), 'L');
    assert.equal(L.pongSide(t, t.x1 + 1, t.y1), 'R');
    assert.equal(L.pongSide(t, t.x0 + 1, t.y0 - 1), null);
    assert.equal(L.pongSide(t, t.x0 - 1, t.y0 - 3), null);
  });

  test('ball waits for the serve delay, then moves', () => {
    const g = play();
    L.stepPong(g, 0.033, 100);
    assert.equal(g.ball.x, L.COURT_W / 2);
    L.stepPong(g, 0.033, L.SERVE_DELAY_MS + 1);
    assert.notEqual(g.ball.x, L.COURT_W / 2);
  });

  test('ball bounces off an aligned paddle', () => {
    const g = play();
    g.serveAt = 0;
    g.ball = { x: 0.3, y: 0.5, vx: -1, vy: 0 };
    g.pad.L = 0.5;
    g.target.L = 0.5;
    let pt = null;
    for (let i = 0; i < 20 && !pt; i++) pt = L.stepPong(g, 0.033, 1e6);
    assert.equal(pt, null);
    assert.ok(g.ball.vx > 0, 'ball reversed');
    assert.ok(Math.hypot(g.ball.vx, g.ball.vy) > 1, 'ball sped up');
  });

  test('missed ball scores for the other side and re-serves toward the loser', () => {
    const g = play();
    g.serveAt = 0;
    g.ball = { x: 0.3, y: 0.9, vx: -1.5, vy: 0 };
    g.pad.L = 0.15;
    g.target.L = 0.15;
    let pt = null;
    for (let i = 0; i < 30 && !pt; i++) pt = L.stepPong(g, 0.033, 5000);
    assert.equal(pt, 'R');
    assert.deepEqual(g.score, { L: 0, R: 1 });
    assert.equal(g.ball.x, L.COURT_W / 2);
    assert.ok(g.ball.vx < 0);
    assert.equal(g.serveAt, 5000 + L.SERVE_DELAY_MS);
  });

  test('walls reflect the ball and it never leaves the court vertically', () => {
    const g = play();
    g.serveAt = 0;
    g.ball = { x: 1, y: 0.5, vx: 0.05, vy: 2 };
    for (let i = 0; i < 60; i++) {
      L.stepPong(g, 0.033, 1e6);
      assert.ok(g.ball.y >= L.BALL_R - 1e-9 && g.ball.y <= L.COURT_H - L.BALL_R + 1e-9);
    }
  });

  test('paddles move at a capped speed and clamp to the court', () => {
    const g = play();
    L.setPad(g, 'L', 99);
    assert.equal(g.target.L, L.COURT_H - L.PAD_H / 2);
    L.setPad(g, 'R', 'junk');
    assert.equal(g.target.R, L.COURT_H / 2);
    L.setPad(g, 'X', 0.2);
    L.stepPong(g, 0.033, 0);
    assert.ok(Math.abs(g.pad.L - 0.5 - L.PAD_SPEED * 0.033) < 1e-9);
  });

  test('first to five wins and the game stops', () => {
    const g = play();
    let t = 0;
    let guard = 0;
    while (g.status === 'play' && guard++ < 100000) {
      g.pad.L = 0.15; g.target.L = 0.15;
      g.pad.R = 0.15; g.target.R = 0.15;
      if (t >= g.serveAt && Math.abs(g.ball.vy) > 0) g.ball.y = Math.max(g.ball.y, 0.8);
      L.stepPong(g, 0.033, t);
      t += 33;
    }
    assert.equal(g.status, 'over');
    assert.equal(Math.max(g.score.L, g.score.R), L.WIN_SCORE);
    assert.ok(g.winner === 'L' || g.winner === 'R');
    assert.equal(L.stepPong(g, 0.033, t), null);
  });

  test('packPong is compact and rounded', () => {
    const g = play();
    g.names = { L: 'Ann', R: 'Bo' };
    const p = L.packPong(g, 0);
    assert.deepEqual(p.ids, ['a', 'b']);
    assert.deepEqual(p.n, ['Ann', 'Bo']);
    assert.equal(p.s, 'play');
    assert.equal(p.wait, 1);
    assert.ok(JSON.stringify(p).length < 200);
  });
});

describe('snake, cooler and touch helpers', () => {
  test('snake eats, grows and dies on the wall', () => {
    const st = L.newSnake(10, () => 0);
    st.food = [st.snake[0][0] + 1, st.snake[0][1]];
    L.snakeStep(st);
    assert.equal(st.score, 1);
    assert.equal(st.snake.length, 4);
    for (let i = 0; i < 20 && !st.dead; i++) L.snakeStep(st);
    assert.equal(st.dead, true);
  });
  test('snake ignores a reversal', () => {
    const st = L.newSnake(10);
    st.next = [-1, 0];
    L.snakeStep(st);
    assert.deepEqual(st.dir, [1, 0]);
    assert.equal(st.dead, false);
  });
  test('food spawns at least the minimum distance away', () => {
    for (let i = 0; i < 50; i++) {
      const st = L.newSnake(20);
      const [hx, hy] = st.snake[0];
      assert.ok(Math.abs(st.food[0] - hx) + Math.abs(st.food[1] - hy) >= L.SNAKE_FOOD_MIN_DIST);
    }
  });
  test('the max arcade rate covers a perfect snake run', () => {
    const perSec = 1000 / L.SNAKE_STEP_MS / L.SNAKE_FOOD_MIN_DIST;
    assert.ok(perSec <= L.ARCADE_MAX_PER_SEC);
  });
  test('icebreakers do not repeat back to back', () => {
    let q = null;
    for (let i = 0; i < 50; i++) {
      const n = L.pickQuestion(q);
      assert.notEqual(n, q);
      q = n;
    }
  });
  test('nearTile finds the map objects', () => {
    const c = L.nearTile(obj, 0, 0, 'C', 60);
    assert.ok(c);
    assert.equal(obj[c.y][c.x], 'C');
    assert.equal(L.nearTile(obj, c.x, c.y + 1, 'C', 1).x, c.x);
    assert.equal(L.nearTile(obj, c.x + 5, c.y + 5, 'C', 2), null);
  });
  test('joystick and pinch maths', () => {
    assert.equal(L.joystickDir(3, 2), null);
    assert.equal(L.joystickDir(30, 5), 'right');
    assert.equal(L.joystickDir(-30, 5), 'left');
    assert.equal(L.joystickDir(4, -30), 'up');
    assert.equal(L.joystickDir(4, 30), 'down');
    assert.equal(L.pinchSteps(100, 100), 0);
    assert.equal(L.pinchSteps(100, 100 * L.PINCH_STEP ** 2), 2);
    assert.equal(L.pinchSteps(100, 100 / L.PINCH_STEP), -1);
    assert.equal(L.pinchSteps(0, 50), 0);
  });
});
