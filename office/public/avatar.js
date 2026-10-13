export const AV_MAX = 10000;
export const AV_CUSTOM = 1 << 24;

export const SKIN = ['#f7d3b0', '#eab48c', '#cf915f', '#94603c', '#ffe3c9', '#6b4226'];
export const HAIR_COLOR = ['#2e1d12', '#6e4122', '#e0ad4c', '#1a1a1f', '#a65a32', '#dcdcdc', '#b8453a', '#f08fb0', '#8f6bd9', '#3aa7e0', '#3f9f5a', '#7a7f8c'];
export const TOP = ['#ef5a4c', '#4a90e2', '#3cc47c', '#9b6bdb', '#f5a623', '#26b5a5', '#48566d', '#ec5f9b', '#f2f2f2', '#22252e', '#8a5a3a', '#c9d34a'];
export const BOTTOM = ['#2f3446', '#5b4636', '#3f6b4f', '#7a2f3b', '#8d8f98', '#c9b48a', '#22252e', '#4a67a8'];
export const SHOES = ['#1c1f2b', '#f1f1f1', '#8a4b2a', '#d94848'];
export const HAIR = ['Short', 'Long', 'Quiff', 'Bald', 'Curly', 'Ponytail', 'Mohawk', 'Bun'];
export const FACE = ['None', 'Beard', 'Moustache', 'Stubble'];
export const EXTRA = ['None', 'Glasses', 'Sunglasses', 'Cap', 'Beanie', 'Headphones', 'Party hat', 'Crown'];

export const FIELDS = [
  ['skin', SKIN.length], ['hair', HAIR.length], ['hairColor', HAIR_COLOR.length], ['top', TOP.length],
  ['bottom', BOTTOM.length], ['face', FACE.length], ['extra', EXTRA.length], ['shoes', SHOES.length],
];
export const AV_COMBOS = FIELDS.reduce((n, f) => n * f[1], 1);

export function cleanAvatar(v) {
  if (!Number.isInteger(v)) return undefined;
  if (v >= 0 && v < AV_MAX) return v;
  if (v >= AV_CUSTOM && v < AV_CUSTOM + AV_COMBOS) return v;
  return undefined;
}

export function unpackAvatar(av) {
  let n = Math.max(0, Math.floor(av) - AV_CUSTOM);
  const out = {};
  for (const [key, size] of FIELDS) {
    out[key] = n % size;
    n = Math.floor(n / size);
  }
  return out;
}

export function packAvatar(look) {
  let n = 0;
  for (let i = FIELDS.length - 1; i >= 0; i--) {
    const [key, size] = FIELDS[i];
    const v = Math.floor(Number(look?.[key]) || 0);
    n = n * size + (((v % size) + size) % size);
  }
  return AV_CUSTOM + n;
}

export function lookOf(av) {
  const n = Number.isFinite(av) ? Math.abs(Math.floor(av)) : 0;
  if (n >= AV_CUSTOM) return unpackAvatar(n);
  return { skin: n % 5, hairColor: ((n / 5) | 0) % 7, top: ((n / 35) | 0) % 8, hair: ((n / 280) | 0) % 3, bottom: 0, face: 0, extra: 0, shoes: 0 };
}

export function randomLook(rng = Math.random) {
  const out = {};
  for (const [key, size] of FIELDS) out[key] = Math.floor(rng() * size) % size;
  if (rng() < 0.55) out.face = 0;
  if (rng() < 0.5) out.extra = 0;
  return out;
}
