import { html, setHtml, trusted } from '../../html.js';
import { overlay, svg, goUse } from './ui.js';
import { newSnake, snakeStep, SNAKE_STEP_MS } from './logic.js';

const DIRS = { arrowup: [0, -1], w: [0, -1], arrowdown: [0, 1], s: [0, 1], arrowleft: [-1, 0], a: [-1, 0], arrowright: [1, 0], d: [1, 0] };
const PAD = { up: [0, -1], down: [0, 1], left: [-1, 0], right: [1, 0] };

export default function arcade(api, arrive) {
  let scores = [];
  let ui = null;

  function table() {
    if (!scores.length) return html`<p class="pl-empty">No scores yet. Be the first.</p>`;
    return html`<ol class="pl-scores">${scores.map((s, i) => html`<li class="${s.name === api.me?.name ? 'me' : ''}"><span class="pl-rank">${i + 1}</span><span class="pl-name">${s.name}</span><b>${s.score}</b></li>`)}</ol>`;
  }

  function open() {
    if (ui) return;
    let game = null, timer = null, waiting = false;
    const h = overlay(api, {
      title: 'Arcade: Snake',
      iconName: 'gamepad',
      body: html`<div class="pl-arcade">
        <div class="pl-screen"><canvas class="pl-snake" width="320" height="320"></canvas><div class="pl-msg"></div></div>
        <div class="pl-side">
          <div class="pl-stat"><span class="muted">Score</span><b class="pl-score">0</b></div>
          <button class="btn btn-sm pl-start">Start</button>
          <div class="pl-dpad"><button data-d="up" aria-label="Up">${svg('up', 18)}</button><button data-d="left" aria-label="Left">${svg('left', 18)}</button><button data-d="right" aria-label="Right">${svg('right', 18)}</button><button data-d="down" aria-label="Down">${svg('down', 18)}</button></div>
          <div class="pl-hs"><div class="pl-sub">${svg('trophy', 14)}High scores</div><div class="pl-table">${table()}</div></div>
        </div>
      </div>`,
      onKey(k, down) {
        if (!down) return;
        if (DIRS[k] && game && !game.dead) game.next = DIRS[k];
        else if ((k === ' ' || k === 'enter') && !game?.running) start();
      },
      onClose() {
        clearInterval(timer);
        ui = null;
      },
    });
    const cv = h.q('.pl-snake');
    const g2 = cv.getContext('2d');
    const msg = h.q('.pl-msg');
    const say = (t) => { msg.textContent = t || ''; msg.hidden = !t; };

    function draw() {
      const n = game?.n || 20, s = cv.width / n;
      g2.fillStyle = '#15161b';
      g2.fillRect(0, 0, cv.width, cv.height);
      g2.fillStyle = '#1d1f26';
      for (let y = 0; y < n; y++) for (let x = (y & 1); x < n; x += 2) g2.fillRect(x * s, y * s, s, s);
      if (!game) return;
      if (game.food) {
        g2.fillStyle = '#ef4444';
        g2.beginPath();
        g2.arc((game.food[0] + 0.5) * s, (game.food[1] + 0.5) * s, s * 0.36, 0, Math.PI * 2);
        g2.fill();
      }
      game.snake.forEach(([x, y], i) => {
        g2.fillStyle = i ? '#6f78ff' : '#b4b9ff';
        g2.fillRect(x * s + 1, y * s + 1, s - 2, s - 2);
      });
    }

    function start() {
      if (waiting) return;
      waiting = true;
      say('Starting…');
      api.send({ t: 'play-arcade-start' });
    }

    function go() {
      waiting = false;
      game = newSnake();
      game.running = true;
      h.q('.pl-score').textContent = '0';
      h.q('.pl-start').disabled = true;
      say('');
      clearInterval(timer);
      timer = setInterval(() => {
        snakeStep(game);
        h.q('.pl-score').textContent = game.score;
        draw();
        if (game.dead) over();
      }, SNAKE_STEP_MS);
      draw();
    }

    function over() {
      clearInterval(timer);
      game.running = false;
      h.q('.pl-start').disabled = false;
      h.q('.pl-start').textContent = 'Play again';
      say(`Game over: ${game.score}`);
      api.send({ t: 'play-arcade-score', score: game.score });
    }

    h.q('.pl-start').onclick = start;
    h.q('.pl-dpad').addEventListener('pointerdown', (e) => {
      const b = e.target.closest('button[data-d]');
      if (!b) return;
      e.preventDefault();
      if (game && !game.dead) game.next = PAD[b.dataset.d];
      else if (!game?.running) start();
    });
    let sw = null;
    cv.addEventListener('pointerdown', (e) => { sw = { x: e.clientX, y: e.clientY }; e.preventDefault(); });
    cv.addEventListener('pointerup', (e) => {
      if (!sw) return;
      const dx = e.clientX - sw.x, dy = e.clientY - sw.y;
      sw = null;
      if (Math.hypot(dx, dy) < 18) { if (!game?.running) start(); return; }
      if (game && !game.dead) game.next = Math.abs(dx) > Math.abs(dy) ? [Math.sign(dx), 0] : [0, Math.sign(dy)];
    });
    say('Press Start, Space, or tap the screen');
    draw();
    ui = { h, go, fail: (r) => { if (waiting) { waiting = false; say(r); } }, refresh: () => { setHtml(h.q('.pl-table'), table()); }, result(m) {
      if (!m.ok) say(`${m.reason}`);
      else if (m.top) say(`New office high score: ${m.score}`);
      else if (m.rank) say(`Game over: ${m.score}, rank #${m.rank}`);
    } };
  }

  api.onObject('A', open);
  api.onInit((m) => { scores = m.play?.scores || []; ui?.refresh(); });
  api.onMessage('play-scores', (m) => { scores = m.list || []; ui?.refresh(); });
  api.onMessage('play-arcade-go', () => ui?.go());
  api.onMessage('play-err', (m) => ui?.fail(m.reason));
  api.onMessage('play-arcade-result', (m) => ui?.result(m));
  api.onMessage('play-hi', (m) => {
    api.hudNote({ key: 'arcade', title: 'New arcade high score', body: `${m.id === api.me?.id ? 'You' : m.name} scored ${m.score} at Snake` });
  });
  api.paletteItems(() => [{ ic: svg('gamepad', 14), label: 'Play arcade', sub: 'Snake', run: () => goUse(api, arrive, 'A', open) }]);
}
