import touch from './play/touch.js';
import arcade from './play/arcade.js';
import pong from './play/pong.js';
import breaks from './play/breaks.js';
import { makeArrival } from './play/ui.js';

export default function setup(api) {
  const arrive = makeArrival(api);
  for (const part of [touch, arcade, pong, breaks]) {
    try { part(api, arrive); } catch (err) { console.error('office play part failed', err); }
  }
  api.onMessage('play-err', (m) => api.noticeError(m.reason || 'Not now'));
}
