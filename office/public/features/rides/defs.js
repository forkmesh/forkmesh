import { PARK_X0 as K, RIDES } from '../../map.js';
import { Path } from './timeline.js';
import { ROAD, ANCHORS as A } from '../../park/art.js';
import { buildTrack, pointAt, SPEED } from '../coaster/track.js';

const mix = (a, b) => {
  let h = Math.imul((a | 0) ^ 0x9e3779b9, 0x85ebca6b) ^ Math.imul(b | 0, 0xc2b2ae35);
  h = Math.imul(h ^ (h >>> 15), 0x2c1b3c6d);
  return ((h ^ (h >>> 13)) >>> 0) / 4294967296;
};

export const RACE_EVERY_MS = 3000;
export const SUMMIT_LIFT = 6;
const SUMMIT = [A.crater.x, A.crater.y + SUMMIT_LIFT];

function climb() {
  return new Path(A.crater.x, 26.5).go(A.crater.x, 25, { speed: 2, lift: 1.5 }).go(SUMMIT[0], SUMMIT[1], { speed: 1.8, lift: SUMMIT_LIFT });
}

function coaster() {
  const track = buildTrack();
  const s = [];
  const total = track.length / SPEED;
  for (let t = 0; t <= total * 1000; t += 50) {
    const p = pointAt(track, (t / 1000) * SPEED);
    s.push({ x: p.x, y: p.y, lift: p.lift, wild: p.loop, phase: '' });
  }
  return { kind: 'cart', s, duration: s.length * 50, sample: (t) => s[Math.max(0, Math.min(s.length - 1, Math.floor(t / 50)))] };
}

const BUILD = {
  coaster,
  'drive-park': () => {
    const p = new Path(ROAD[0][0], ROAD[0][1]).hold(700);
    p.go(...ROAD[1], { speed: 3 }).go(...ROAD[2], { speed: 7 }).go(...ROAD[3], { speed: 7 }).go(...ROAD[4], { speed: 7 });
    return p.go(K + 6.5, 21.5, { speed: 3 }).hold(400).done('car');
  },
  'drive-office': () => {
    const p = new Path(K + 3.5, 20.5).hold(700);
    p.go(K + 1.5, 21.5, { speed: 3 }).go(...ROAD[3], { speed: 7 }).go(...ROAD[2], { speed: 7 }).go(...ROAD[1], { speed: 7 });
    return p.go(60.5, 18.5, { speed: 3 }).hold(400).done('car');
  },
  lazy: () => {
    const p = new Path(K + 42.5, 21.5).go(K + 41, 21.5, { speed: 1.5 });
    for (const [x, y] of [[41, 10], [62, 10], [62, 29], [41, 29], [41, 21.5]]) p.go(K + x, y, { speed: 1.6 });
    return p.go(K + 42.5, 21.5, { speed: 1.5 }).done('raft');
  },
  drop: () => {
    const p = climb().hold(3000, { phase: 'capsule' });
    p.go(SUMMIT[0], SUMMIT[1] + 0.6, { speed: 30, lift: 2.4, wild: true });
    p.go(K + 44, 19.5, { speed: 13, lift: 1.4, wild: true }).go(K + 38, 19.8, { speed: 10, lift: 0.6, wild: true });
    return p.go(K + 35.5, 20.5, { speed: 5, lift: 0 }).hold(500).done('body');
  },
  funnel: () => {
    const p = climb().hold(700);
    const f = A.funnel;
    p.go(K + 58, 16, { speed: 9, lift: 4.5, wild: true }).go(f.x - f.r, f.y, { speed: 10, lift: 0.3, wild: true });
    p.spiral(f.x, f.y, f.r, 0.5, 2.5, { speed: 8, lift: 0.1 });
    p.go(K + 77, 19.5, { speed: 10, lift: 0, wild: true });
    return p.go(K + 75.5, 20.5, { speed: 4, lift: 0 }).hold(500).done('tube');
  },
  blaster: () => {
    const p = climb().hold(500);
    p.go(K + 58.5, 22.5, { speed: 8, lift: 4.2, wild: true }).go(K + 64, 25, { speed: 8, lift: 3.4 });
    p.go(K + 68, 31.5, { speed: 10, lift: 1.4, wild: true });
    p.go(K + 96, 31.5, { speed: 8, lift: 1.4, bump: 2.6, bumps: 5, wild: true });
    p.go(K + 96, 41.5, { speed: 8, lift: 1.2, bump: 2, bumps: 2, wild: true });
    p.go(K + 87.5, 41.5, { speed: 7, lift: 0.8 });
    return p.go(K + 83.5, 38.5, { speed: 5, lift: 0 }).hold(500).done('raft');
  },
  cliff: () => {
    const peakLift = 2.3;
    const p = new Path(K + 74.5, 14.5).go(A.skullPeak.x, A.skullPeak.y + peakLift, { speed: 1.4, lift: peakLift }).hold(1600);
    return p.arc(K + 71.5, 20.5, 1100, 3.4).hold(700).done('body');
  },
  flush: () => {
    const b = A.flush, d = A.flushDeck;
    const p = new Path(K + 8.5, 30.5).go(d.x, d.y + 0.6, { speed: 1.6, lift: 0.6 });
    p.go(b.x + b.r, b.y, { speed: 3, lift: 0.15, wild: true }).hold(900, { phase: 'rim' });
    p.spiral(b.x, b.y, b.r, 0.25, 7, { speed: 9, lift: 0.1 });
    p.go(b.x, b.y + 0.1, { speed: 2, lift: 0, phase: 'flush' }).hold(500, { phase: 'flush' });
    p.go(b.x, 39.5, { speed: 6, lift: 0, wild: true, phase: 'pipe' }).go(K + 5.5, 39.5, { speed: 2 });
    return p.hold(600).done('body');
  },
  surf: () => {
    const p = new Path(K + 76.5, 37.5).go(K + 70.5, 37.5, { speed: 3 });
    p.wobble(22000, { cx: K + 70.5, cy: 37.5, ax: 2.6, ay: 1.1, lift: 0.35 });
    return p.go(K + 76.5, 38.5, { speed: 3, lift: 0 }).done('surf');
  },
};

for (let lane = 0; lane < 4; lane++) {
  BUILD[`race${lane + 1}`] = (at) => {
    const x = K + 92.5 + lane;
    const v = 8 + mix(at, lane) * 4;
    const p = new Path(K + 89.5, 4.5).go(x, A.racerPads + 0.5, { speed: 2.5, lift: 0.5 });
    const climbed = p.s.length * 50;
    const release = Math.ceil((at + climbed + 1000) / RACE_EVERY_MS) * RACE_EVERY_MS;
    p.hold(release - at - climbed, { phase: 'gate' });
    p.go(x, 24.5, { speed: v, lift: 0.2, bump: 0.8, bumps: 4, wild: true });
    return p.go(x, 26.5, { speed: 4, lift: 0 }).hold(400).done('mat');
  };
}

export const INFO = {
  coaster: { name: 'Rollercoaster', sub: 'One lap of the office with three loops' },
  'drive-park': { name: 'Drive to the water park', sub: 'Hop in the car' },
  'drive-office': { name: 'Drive back to the office', sub: 'Hop in the car' },
  lazy: { name: 'Jungle River', sub: 'A slow raft lap around the volcano, under a waterfall' },
  drop: { name: 'Leap of Doom', sub: 'Trapdoor capsule, a near-vertical drop into the lagoon' },
  funnel: { name: 'Typhoon Twister', sub: 'Tube slide into a giant spinning funnel' },
  blaster: { name: 'Krakatoa Blaster', sub: 'Uphill water coaster with five water-jet launches' },
  race1: { name: 'Riptide Racers', sub: 'Four-lane mat race, live results' },
  race2: { name: 'Riptide Racers', sub: 'Four-lane mat race, live results' },
  race3: { name: 'Riptide Racers', sub: 'Four-lane mat race, live results' },
  race4: { name: 'Riptide Racers', sub: 'Four-lane mat race, live results' },
  cliff: { name: 'Skull Rock', sub: 'Climb the rock and dive into the reef pool' },
  flush: { name: 'Royal Flush', sub: 'Spin round and round the giant bowl, then get flushed out the bottom' },
  surf: { name: 'Endless Wave', sub: 'Ride the surf machine for 22 seconds' },
};

export const IDS = Object.keys(BUILD);
export const BOARDS = { ...RIDES };

const cache = new Map();
export function timeline(id, at = 0) {
  const build = BUILD[id];
  if (!build) return null;
  const key = id.startsWith('race') ? `${id}:${at}` : id;
  let tl = cache.get(key);
  if (!tl) {
    tl = build(at);
    cache.set(key, tl);
    if (cache.size > 40) cache.delete(cache.keys().next().value);
  }
  return tl;
}
