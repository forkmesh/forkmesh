import { placeKey, encodePlace, readPlace } from './place/logic.js';

const SAVE_EVERY_MS = 2000;

export default function setup(api) {
  let last = '';
  let lastAt = 0;
  const key = () => placeKey(api.account?.org);

  const save = (force) => {
    const me = api.me;
    if (!me) return;
    const now = Date.now();
    const spot = `${me.x},${me.y},${me.d}`;
    if (spot === last || (!force && now - lastAt < SAVE_EVERY_MS)) return;
    last = spot;
    lastAt = now;
    api.store.set(key(), encodePlace(me, now));
  };

  api.joinExtras(() => {
    if (api.me) return {};
    const p = readPlace(api.store.get(key()), Date.now());
    return p ? { x: p.x, y: p.y } : {};
  });
  api.onTick(() => save(false));
  addEventListener('pagehide', () => save(true));
  document.addEventListener('visibilitychange', () => { if (document.hidden) save(true); });
}
