export const COURT_W = 2;
export const COURT_H = 1;
export const PAD_H = 0.3;
export const PAD_X = 0.08;
export const BALL_R = 0.035;
export const PAD_SPEED = 2.6;
export const SERVE_SPEED = 0.9;
export const MAX_SPEED = 2.4;
export const WIN_SCORE = 5;
export const SERVE_DELAY_MS = 1200;
export const TICK_MS = 33;

export const ARCADE_MAX_PER_SEC = 3;
export const ARCADE_MAX_SCORE = 5000;
export const ARCADE_MAX_MS = 30 * 60 * 1000;
export const SCORES_KEEP = 10;

export const SNAKE_N = 20;
export const SNAKE_STEP_MS = 110;
export const SNAKE_FOOD_MIN_DIST = 4;

export const COOLER_RADIUS = 3;
export const JOY_DEAD = 14;
export const PINCH_STEP = 1.18;
export const USE_RADIUS = 2;

export const ICEBREAKERS = [
  'What is a small thing that made your week better?',
  'Which tool could you not work without?',
  'What was your first job?',
  'If you could learn one skill overnight, what would it be?',
  'What is the best meal you have had this year?',
  'Which app on your phone do you open most?',
  'What is a hobby you picked up recently?',
  'Mountains or beach, and why?',
  'What song is stuck in your head right now?',
  'What is the most useful thing you learned from a teammate?',
  'Which book or article changed how you work?',
  'What would you build if you had a free week?',
  'Tea, coffee, or something else entirely?',
  'What is the best piece of advice you have been given?',
  'Where would you work from if you could pick anywhere?',
  'What was your favourite game growing up?',
  'What is one thing on your desk right now that has a story?',
  'Early bird or night owl?',
  'What is a bug you will never forget?',
  'Which city would you like to spend a month in?',
  'What is a small habit that helps you focus?',
  'What did you want to be when you were ten?',
  'What is the last thing you fixed around the house?',
  'Which emoji do you use far too often?',
  'What is a show or film you recommend to everyone?',
  'Keyboard shortcuts you swear by?',
  'What is something you are looking forward to this month?',
  'If our team had a mascot, what should it be?',
];

export const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

export function plausibleScore(score, ms) {
  if (!Number.isInteger(score) || score < 0 || score > ARCADE_MAX_SCORE) return false;
  if (!Number.isFinite(ms) || ms < 0 || ms > ARCADE_MAX_MS) return false;
  return score <= Math.floor((ms / 1000) * ARCADE_MAX_PER_SEC) + 1;
}

export function insertScore(list, entry, keep = SCORES_KEEP) {
  if (!(entry.score > 0)) return { list, rank: 0, top: false };
  const prevBest = list.length ? list[0].score : -1;
  const mine = list.find((s) => s.k === entry.k);
  if (mine && mine.score >= entry.score) return { list, rank: 0, top: false };
  const next = list.filter((s) => s.k !== entry.k);
  next.push(entry);
  next.sort((a, b) => b.score - a.score || a.ts - b.ts);
  next.length = Math.min(next.length, keep);
  const rank = next.indexOf(entry) + 1;
  return { list: next, rank, top: rank === 1 && entry.score > prevBest && entry.score > 0 };
}

export function findTables(obj, char = 'G') {
  const seen = new Set();
  const out = [];
  for (let y = 0; y < obj.length; y++) {
    for (let x = 0; x < obj[y].length; x++) {
      if (obj[y][x] !== char || seen.has(y * 1000 + x)) continue;
      let x1 = x, y1 = y;
      while (obj[y][x1 + 1] === char) x1++;
      while (obj[y1 + 1]?.[x] === char) y1++;
      for (let yy = y; yy <= y1; yy++) for (let xx = x; xx <= x1; xx++) seen.add(yy * 1000 + xx);
      out.push({ id: `${x},${y}`, x0: x, y0: y, x1, y1 });
    }
  }
  return out;
}

export function pongSide(t, x, y) {
  if (y < t.y0 - 1 || y > t.y1 + 1) return null;
  if (x === t.x0 - 1 || x === t.x0 - 2) return 'L';
  if (x === t.x1 + 1 || x === t.x1 + 2) return 'R';
  return null;
}

export function endTiles(t, side) {
  const x = side === 'L' ? t.x0 - 1 : t.x1 + 1;
  const out = [];
  for (let y = t.y0; y <= t.y1; y++) out.push([x, y]);
  return out;
}

export function nearTable(t, x, y, r) {
  return x >= t.x0 - r && x <= t.x1 + r && y >= t.y0 - r && y <= t.y1 + r;
}

export function nearTile(obj, x, y, char, r) {
  let best = null, bd = Infinity;
  for (let yy = y - r; yy <= y + r; yy++) {
    for (let xx = x - r; xx <= x + r; xx++) {
      if (obj[yy]?.[xx] !== char) continue;
      const d = Math.abs(xx - x) + Math.abs(yy - y);
      if (d <= r && d < bd) { bd = d; best = { x: xx, y: yy }; }
    }
  }
  return best;
}

export function pickQuestion(prev, rnd = Math.random) {
  if (ICEBREAKERS.length < 2) return ICEBREAKERS[0];
  let q = prev;
  while (q === prev) q = ICEBREAKERS[Math.floor(rnd() * ICEBREAKERS.length) % ICEBREAKERS.length];
  return q;
}

export function dayKey(ts) {
  return new Date(ts).toISOString().slice(0, 10);
}

export function newPong(table) {
  return {
    table,
    players: { L: null, R: null },
    names: { L: '', R: '' },
    pad: { L: COURT_H / 2, R: COURT_H / 2 },
    target: { L: COURT_H / 2, R: COURT_H / 2 },
    ball: { x: COURT_W / 2, y: COURT_H / 2, vx: 0, vy: 0 },
    score: { L: 0, R: 0 },
    status: 'wait',
    serveAt: 0,
    winner: null,
  };
}

export function serve(g, toward, now, rnd = Math.random) {
  const a = (rnd() - 0.5) * (Math.PI / 3);
  const dir = toward === 'L' ? -1 : 1;
  g.ball = { x: COURT_W / 2, y: COURT_H / 2, vx: dir * SERVE_SPEED * Math.cos(a), vy: SERVE_SPEED * Math.sin(a) };
  g.serveAt = now + SERVE_DELAY_MS;
}

export function startPong(g, now, rnd = Math.random) {
  g.score = { L: 0, R: 0 };
  g.pad = { L: COURT_H / 2, R: COURT_H / 2 };
  g.target = { L: COURT_H / 2, R: COURT_H / 2 };
  g.status = 'play';
  g.winner = null;
  serve(g, rnd() < 0.5 ? 'L' : 'R', now, rnd);
}

export function setPad(g, side, y) {
  const v = Number(y);
  if (!Number.isFinite(v) || (side !== 'L' && side !== 'R')) return;
  g.target[side] = clamp(v, PAD_H / 2, COURT_H - PAD_H / 2);
}

function movePads(g, dt) {
  const max = PAD_SPEED * dt;
  for (const s of ['L', 'R']) g.pad[s] += clamp(g.target[s] - g.pad[s], -max, max);
}

function bounce(g, side) {
  const b = g.ball;
  const off = clamp((b.y - g.pad[side]) / (PAD_H / 2), -1, 1);
  const speed = Math.min(MAX_SPEED, Math.hypot(b.vx, b.vy) * 1.07);
  const a = off * (Math.PI * 55 / 180);
  const dir = side === 'L' ? 1 : -1;
  b.vx = dir * speed * Math.cos(a);
  b.vy = speed * Math.sin(a);
}

export function stepPong(g, dt, now, rnd = Math.random) {
  if (g.status !== 'play') return null;
  movePads(g, dt);
  if (now < g.serveAt) return null;
  const b = g.ball;
  const steps = Math.max(1, Math.ceil(dt / 0.008));
  const h = dt / steps;
  for (let i = 0; i < steps; i++) {
    const px = b.x;
    b.x += b.vx * h;
    b.y += b.vy * h;
    if (b.y < BALL_R) { b.y = 2 * BALL_R - b.y; b.vy = Math.abs(b.vy); }
    if (b.y > COURT_H - BALL_R) { b.y = 2 * (COURT_H - BALL_R) - b.y; b.vy = -Math.abs(b.vy); }
    const lf = PAD_X + BALL_R, rf = COURT_W - PAD_X - BALL_R;
    if (b.vx < 0 && px >= lf && b.x < lf && Math.abs(b.y - g.pad.L) <= PAD_H / 2 + BALL_R) { b.x = 2 * lf - b.x; bounce(g, 'L'); }
    else if (b.vx > 0 && px <= rf && b.x > rf && Math.abs(b.y - g.pad.R) <= PAD_H / 2 + BALL_R) { b.x = 2 * rf - b.x; bounce(g, 'R'); }
    if (b.x < 0 || b.x > COURT_W) {
      const scorer = b.x < 0 ? 'R' : 'L';
      g.score[scorer]++;
      if (g.score[scorer] >= WIN_SCORE) {
        g.status = 'over';
        g.winner = scorer;
        b.vx = 0; b.vy = 0; b.x = COURT_W / 2; b.y = COURT_H / 2;
      } else serve(g, scorer === 'L' ? 'R' : 'L', now, rnd);
      return scorer;
    }
  }
  return null;
}

export function packPong(g, now = Date.now()) {
  const r = (v) => Math.round(v * 1000) / 1000;
  return {
    id: g.table.id,
    s: g.status,
    b: [r(g.ball.x), r(g.ball.y)],
    p: [r(g.pad.L), r(g.pad.R)],
    sc: [g.score.L, g.score.R],
    ids: [g.players.L, g.players.R],
    n: [g.names.L, g.names.R],
    w: g.winner,
    wait: g.status === 'play' && g.serveAt > now ? 1 : 0,
  };
}

export function snakeFood(snake, n = SNAKE_N, rnd = Math.random, minDist = SNAKE_FOOD_MIN_DIST) {
  const head = snake[0];
  const taken = new Set(snake.map(([x, y]) => y * n + x));
  const free = [];
  const far = [];
  for (let y = 0; y < n; y++) {
    for (let x = 0; x < n; x++) {
      if (taken.has(y * n + x)) continue;
      free.push([x, y]);
      if (Math.abs(x - head[0]) + Math.abs(y - head[1]) >= minDist) far.push([x, y]);
    }
  }
  const pool = far.length ? far : free;
  return pool.length ? pool[Math.floor(rnd() * pool.length) % pool.length] : null;
}

export function snakeStep(st, rnd = Math.random) {
  if (st.dead) return st;
  if (st.next && !(st.next[0] === -st.dir[0] && st.next[1] === -st.dir[1])) st.dir = st.next;
  st.next = null;
  const [hx, hy] = st.snake[0];
  const nx = hx + st.dir[0], ny = hy + st.dir[1];
  const grow = st.food && nx === st.food[0] && ny === st.food[1];
  const body = grow ? st.snake : st.snake.slice(0, -1);
  if (nx < 0 || ny < 0 || nx >= st.n || ny >= st.n || body.some(([x, y]) => x === nx && y === ny)) {
    st.dead = true;
    return st;
  }
  st.snake = [[nx, ny], ...body];
  if (grow) {
    st.score++;
    st.food = snakeFood(st.snake, st.n, rnd);
  }
  return st;
}

export function newSnake(n = SNAKE_N, rnd = Math.random) {
  const c = n >> 1;
  const snake = [[c, c], [c - 1, c], [c - 2, c]];
  return { n, snake, dir: [1, 0], next: null, food: snakeFood(snake, n, rnd), score: 0, dead: false };
}

export function joystickDir(dx, dy, dead = JOY_DEAD) {
  if (Math.hypot(dx, dy) < dead) return null;
  return Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? 'right' : 'left') : (dy > 0 ? 'down' : 'up');
}

export function pinchSteps(d0, d, step = PINCH_STEP) {
  if (!(d0 > 0) || !(d > 0)) return 0;
  return Math.round(Math.log(d / d0) / Math.log(step));
}
