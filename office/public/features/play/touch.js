import { html, setHtml, trusted } from '../../html.js';
import { anyOpen } from './ui.js';
import { joystickDir, pinchSteps } from './logic.js';

const RADIUS = 38;
const DIR_KEYS = { up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft', right: 'ArrowRight' };

function key(type, k) {
  dispatchEvent(new KeyboardEvent(type, { key: k, bubbles: true, cancelable: true }));
}

export default function touch(api) {
  const root = document.documentElement;
  const coarse = matchMedia('(pointer: coarse)');
  const narrow = matchMedia('(max-width: 760px)');
  const apply = () => root.classList.toggle('pl-touch', coarse.matches || narrow.matches || 'ontouchstart' in window);
  apply();
  coarse.addEventListener?.('change', apply);
  narrow.addEventListener?.('change', apply);

  const joy = document.createElement('div');
  joy.className = 'pl-joy';
  joy.setAttribute('aria-label', 'Move');
  setHtml(joy, html`<div class="pl-knob"></div>`);
  const knob = joy.firstChild;
  let cur = null, jid = null, cx = 0, cy = 0;
  const release = () => {
    if (cur) key('keyup', DIR_KEYS[cur]);
    cur = null;
    jid = null;
    knob.style.transform = '';
    joy.classList.remove('on');
  };
  joy.addEventListener('pointerdown', (e) => {
    if (!api.me || anyOpen()) return;
    e.preventDefault();
    e.stopPropagation();
    const r = joy.getBoundingClientRect();
    cx = r.left + r.width / 2;
    cy = r.top + r.height / 2;
    jid = e.pointerId;
    joy.setPointerCapture(e.pointerId);
    joy.classList.add('on');
    move(e);
  });
  const move = (e) => {
    if (e.pointerId !== jid) return;
    let dx = e.clientX - cx, dy = e.clientY - cy;
    const d = Math.hypot(dx, dy);
    if (d > RADIUS) { dx = (dx / d) * RADIUS; dy = (dy / d) * RADIUS; }
    knob.style.transform = `translate(${dx}px, ${dy}px)`;
    const dir = joystickDir(dx, dy);
    if (dir === cur) return;
    if (cur) key('keyup', DIR_KEYS[cur]);
    cur = dir;
    if (cur) key('keydown', DIR_KEYS[cur]);
  };
  joy.addEventListener('pointermove', move);
  joy.addEventListener('pointerup', release);
  joy.addEventListener('pointercancel', release);
  joy.addEventListener('lostpointercapture', release);
  document.querySelector('#stage')?.append(joy);

  const cv = document.querySelector('#cv');
  const pts = new Map();
  let pinch = null, swallow = false;
  const dist = () => { const [a, b] = [...pts.values()]; return Math.hypot(a.x - b.x, a.y - b.y); };
  const wheel = (dy) => cv.dispatchEvent(new WheelEvent('wheel', { deltaY: dy, cancelable: true, bubbles: true }));
  cv?.addEventListener('pointerdown', (e) => {
    if (e.pointerType !== 'touch') return;
    pts.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (pts.size === 2) { pinch = { d0: dist(), z0: api.zoom, steps: 0 }; swallow = true; }
    if (pts.size > 1) e.stopImmediatePropagation();
  }, true);
  cv?.addEventListener('pointermove', (e) => {
    if (!pts.has(e.pointerId)) return;
    pts.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (!swallow) return;
    e.stopImmediatePropagation();
    if (!pinch || pts.size < 2) return;
    if (api.setZoom) return api.setZoom(pinch.z0 * dist() / pinch.d0);
    const want = pinchSteps(pinch.d0, dist());
    while (pinch.steps < want) { wheel(-1); pinch.steps++; }
    while (pinch.steps > want) { wheel(1); pinch.steps--; }
  }, true);
  const up = (e) => {
    if (!pts.delete(e.pointerId)) return;
    if (pts.size < 2) pinch = null;
    if (swallow) e.stopImmediatePropagation();
    if (!pts.size) swallow = false;
  };
  cv?.addEventListener('pointerup', up, true);
  cv?.addEventListener('pointercancel', up, true);
  document.addEventListener('gesturestart', (e) => e.preventDefault());

  api.onInit((m, again) => {
    if (!again && narrow.matches) document.querySelector('#app')?.classList.add('collapsed');
  });
  addEventListener('blur', release);
}
