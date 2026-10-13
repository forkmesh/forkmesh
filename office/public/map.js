
import { PARK_X0, PARK_W, PARK_FLOOR, PARK_OBJ, OFFICE_PARKING, OFFICE_CAR } from './park/layout.js';

export { RIDES, PARK_X0, PARK_W } from './park/layout.js';
export const TILE = 32;
export const OFFICE_W = 66;
export const W = PARK_X0 + PARK_W;
export const H = 44;
export const SCENES = [
  { id: 'office', x0: 0, x1: OFFICE_W },
  { id: 'park', x0: PARK_X0, x1: PARK_X0 + PARK_W },
];
export function sceneAt(x) {
  return SCENES.find((s) => x >= s.x0 - 0.5 && x < s.x1 + 0.5) || null;
}
const OX = 6, OY = 7;

export const ZONES = {
  '.': { name: 'Hallway', private: false },
  l: { name: 'Lobby', private: false },
  p: { name: 'Garden', private: false },
  c: { name: 'Coffee Huddle', private: true },
  a: { name: 'Engineering', private: true },
  b: { name: 'Design', private: true },
  k: { name: 'Library', private: true },
  m: { name: 'Media Room', private: true },
  s: { name: 'Lobby Couches', private: true },
  w: { name: 'Workspace', private: false },
  f: { name: 'Focus Lounge', private: false },
  h: { name: 'Campfire', private: true },
  q: { name: 'Agent Room', private: false },
  D: { name: 'Parking', private: false },
  Z: { name: 'Water Park', private: false },
  B: { name: 'Surf Lagoon', private: false },
  K: { name: 'Splash Harbor', private: false },
  A: { name: 'Shark Reef Tunnel', private: false },
  V: { name: 'Volcano Plaza', private: false },
};

export const CHAIRS = { v: 0, '^': 1, '<': 2, '>': 3, n: 0, u: 1, d: 2, c: 3, o: 0, S: 0, s: 1, j: 0, h: 1 };
export const COUCHES = new Set(['S', 's', 'j']);
export const WATER = 'W';
export const FIRE = 'I';
export const FIRE_ZONE = 'h';
export const AGENT_ZONE = 'q';
export const CONSOLE = 'J';
export const GATE = 'x';
export const RIDE_GATE = 'y';
export const CAR = 'e';
const WALKABLE_OBJ = new Set(['', 'r', WATER, ...Object.keys(CHAIRS)]);

export const floor = Array.from({ length: H }, () => Array(W).fill('g'));
export const obj = Array.from({ length: H }, () => Array(W).fill(''));
export const ROOMS = {};

function rect(x0, y0, x1, y1, c, layer = floor) {
  for (let y = y0 + OY; y <= y1 + OY; y++) for (let x = x0 + OX; x <= x1 + OX; x++) layer[y][x] = c;
}
function room(x0, y0, x1, y1, c, doors = []) {
  rect(x0, y0, x1, y1, '#');
  rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, c);
  for (const [x, y] of doors) floor[y + OY][x + OX] = c;
  ROOMS[c] = { x0: x0 + OX, y0: y0 + OY, x1: x1 + OX, y1: y1 + OY };
}
function area(x0, y0, x1, y1, c) {
  rect(x0, y0, x1, y1, c);
  ROOMS[c] = { x0: x0 + OX, y0: y0 + OY, x1: x1 + OX, y1: y1 + OY };
}
function put(list) {
  for (const [c, x, y] of list) obj[y + OY][x + OX] = c;
}

room(1, 1, 52, 24, '.', [[19, 1], [20, 1]]);
room(1, 1, 14, 9, 'a', [[7, 9], [8, 9]]);
room(25, 1, 38, 9, 'b', [[31, 9], [32, 9]]);
room(1, 15, 15, 24, 'c', [[15, 20], [15, 21]]);
rect(2, 16, 14, 16, '#');
room(24, 15, 38, 24, 'l', [[30, 15], [31, 15]]);
room(38, 1, 52, 9, 'k', [[45, 9], [46, 9]]);
room(38, 15, 52, 24, 'm', [[45, 15], [46, 15], [38, 21], [38, 22]]);
area(33, 17, 37, 20, 's');
rect(31, -5, 40, 0, 'p');
area(41, -5, 49, 0, FIRE_ZONE);
room(-5, -6, 30, 0, AGENT_ZONE, [[30, -3], [30, -2], [3, 0], [4, 0], [19, 0], [20, 0]]);
floor[1 + OY][3 + OX] = 'a';
floor[1 + OY][4 + OX] = 'a';
rect(31, -6, 40, -6, 'p');
for (let y = OFFICE_PARKING[1]; y <= OFFICE_PARKING[3]; y++) for (let x = OFFICE_PARKING[0]; x <= OFFICE_PARKING[2]; x++) floor[y][x] = 'D';
floor[10 + OY][52 + OX] = '.';
floor[11 + OY][52 + OX] = '.';
obj[OFFICE_CAR[1]][OFFICE_CAR[0]] = CAR;
for (let y = 0; y < H; y++) for (let x = 0; x < PARK_W; x++) {
  floor[y][PARK_X0 + x] = PARK_FLOOR[y][x];
  const o = PARK_OBJ[y][x];
  obj[y][PARK_X0 + x] = o === '_' ? '' : o;
}
export const AGENT_STATIONS = [];
for (const y of [-5, -3, -1]) for (let x = -2; x <= 28; x += 2) AGENT_STATIONS.push([x + OX, y + OY]);
export const COASTER_PLATFORM = { y: -6 + OY, x0: 31 + OX, x1: 39 + OX };

put([
  ['P', 2, 2], ['P', 13, 2], ['B', 6, 2], ['B', 7, 2], ['B', 8, 2], ['H', 11, 2],
  ['M', 4, 5], ['D', 5, 5], ['^', 4, 6],
  ['M', 9, 5], ['D', 10, 5], ['^', 9, 6],
]);
put([
  ['P', 26, 2], ['P', 37, 2], ['B', 30, 2], ['B', 31, 2], ['B', 32, 2], ['H', 27, 2],
  ['M', 28, 5], ['D', 29, 5], ['^', 28, 6],
  ['M', 33, 5], ['D', 34, 5], ['^', 33, 6],
]);
put([
  ['R', 2, 17], ['K', 3, 17], ['C', 4, 17], ['K', 5, 17], ['K', 6, 17],
  ['T', 8, 20], ['v', 8, 19], ['^', 8, 21], ['>', 7, 20], ['<', 9, 20],
  ['S', 10, 17], ['S', 11, 17], ['S', 12, 17], ['P', 14, 17], ['P', 2, 23], ['P', 14, 23],
]);
put([
  ['D', 26, 17], ['M', 27, 17], ['D', 28, 17],
  ['T', 30, 19], ['T', 31, 19], ['T', 30, 20], ['T', 31, 20],
  ['v', 30, 18], ['v', 31, 18], ['^', 30, 21], ['^', 31, 21],
  ['P', 25, 16], ['P', 37, 16], ['P', 37, 23], ['S', 34, 23], ['S', 35, 23], ['S', 36, 23],
]);
rect(29, 22, 32, 23, 'r', obj);
put([
  ['N', 33, 17], ['n', 34, 17], ['n', 35, 17],
  ['c', 33, 18], ['F', 34, 18], ['F', 35, 18], ['d', 37, 18],
  ['c', 33, 19], ['u', 35, 19], ['o', 37, 20],
]);
put([
  ['P', 39, 2], ['B', 40, 2], ['B', 41, 2], ['B', 42, 2], ['B', 43, 2],
  ['L', 47, 2], ['B', 48, 2], ['B', 49, 2], ['B', 50, 2], ['P', 51, 2],
  ['v', 41, 4], ['>', 40, 5], ['T', 41, 5], ['<', 42, 5], ['^', 41, 6],
  ['v', 49, 4], ['>', 48, 5], ['T', 49, 5], ['<', 50, 5], ['^', 49, 6],
  ['P', 39, 8], ['u', 40, 8], ['u', 50, 8], ['P', 51, 8],
]);
put([
  ['P', 39, 16], ['V', 40, 16], ['V', 41, 16],
  ['X', 48, 16], ['Z', 49, 16], ['Z', 50, 16], ['Q', 51, 16],
  ['v', 44, 18], ['v', 45, 18], ['v', 46, 18],
  ['>', 43, 19], ['T', 44, 19], ['T', 45, 19], ['T', 46, 19], ['<', 47, 19],
  ['>', 43, 20], ['T', 44, 20], ['T', 45, 20], ['T', 46, 20], ['<', 47, 20],
  ['^', 44, 21], ['^', 45, 21], ['^', 46, 21],
  ['P', 39, 23], ['P', 51, 23],
]);
put([
  ['P', 2, 10], ['P', 37, 10], ['P', 2, 14], ['P', 37, 14], ['P', 51, 10], ['P', 51, 14],
  ['G', 18, 4], ['G', 19, 4], ['G', 20, 4], ['G', 21, 4], ['G', 18, 5], ['G', 19, 5], ['G', 20, 5], ['G', 21, 5],
  ['Q', 16, 2], ['A', 22, 2], ['A', 23, 2], ['P', 15, 2], ['P', 24, 2],
  ['P', 16, 23], ['P', 23, 23], ['A', 19, 23], ['A', 20, 23],
]);
put([
  ['W', 34, -4], ['W', 35, -4], ['W', 36, -4], ['W', 37, -4],
  ['W', 34, -3], ['W', 35, -3], ['W', 36, -3], ['W', 37, -3],
  ['v', 32, -4], ['v', 39, -4], ['>', 32, -2], ['<', 39, -2],
  ['Y', 31, -5], ['Y', 40, -5], ['Y', 31, 0], ['Y', 40, 0], ['Y', 33, -5], ['Y', 38, -5],
]);
export const FIRE_SEATS = [[44, -5], [46, -5], [47, -4], [47, -2], [46, -1], [44, -1], [43, -2], [43, -4]].map(([x, y]) => [x + OX, y + OY]);
put([
  [FIRE, 45, -3],
  ['v', 44, -5], ['v', 46, -5], ['<', 47, -4], ['<', 47, -2],
  ['^', 46, -1], ['^', 44, -1], ['>', 43, -2], ['>', 43, -4],
  ['Y', 49, -5], ['Y', 49, 0], ['P', 41, 0],
  [CONSOLE, -4, -5], ['P', -4, -1], ['P', 29, -5],
  [GATE, 40, -6],
]);

room(1, 24, 38, 33, 'w', [[17, 24], [18, 24], [21, 24], [22, 24]]);
rect(27, 25, 37, 32, 'f');
for (const y of [25, 28, 31]) {
  for (const x of y === 25 ? [3, 5, 8, 10, 13, 24] : [3, 5, 8, 10, 13, 15, 18, 20, 23]) put([['M', x, y], ['D', x + 1, y], ['^', x, y + 1]]);
}
put([
  ['P', 2, 25], ['P', 26, 25], ['P', 26, 32],
  ['B', 27, 25], ['B', 28, 25], ['P', 30, 25], ['P', 34, 25], ['B', 36, 25], ['B', 37, 25],
  ['S', 30, 26], ['S', 31, 26], ['S', 32, 26], ['S', 33, 26], ['S', 34, 26],
  ['j', 28, 27], ['j', 28, 28], ['j', 28, 29], ['j', 36, 27], ['j', 36, 28], ['j', 36, 29],
  ['t', 31, 28], ['t', 32, 28], ['t', 33, 28],
  ['s', 30, 30], ['s', 31, 30], ['s', 32, 30], ['s', 33, 30], ['s', 34, 30],
  ['P', 27, 32], ['P', 37, 32],
]);

const TREES = [
  ['E', 2, 4], ['O', 4, 9], ['E', 1, 13], ['U', 3, 17], ['E', 5, 21], ['O', 2, 25], ['E', 4, 30], ['U', 1, 34], ['E', 3, 37],
  ['O', 62, 4], ['E', 64, 8], ['U', 61, 13], ['E', 63, 18], ['O', 62, 23], ['E', 64, 27], ['E', 61, 31], ['O', 63, 36],
  ['E', 8, 3], ['U', 12, 5], ['E', 15, 2], ['O', 18, 5], ['E', 34, 4], ['U', 38, 2], ['O', 41, 5], ['E', 44, 3],
  ['O', 48, 4], ['E', 53, 3], ['U', 57, 5],
  ['E', 9, 35], ['O', 14, 36], ['E', 19, 34], ['U', 24, 36], ['E', 29, 35], ['O', 34, 36], ['E', 39, 34], ['U', 43, 36],
  ['O', 49, 36], ['E', 54, 34], ['U', 58, 36],
  ['O', 3, 41], ['E', 10, 43], ['U', 18, 42], ['E', 27, 43], ['O', 35, 42], ['E', 42, 43], ['U', 48, 41],
];
for (const [c, x, y] of TREES) if (y < H && floor[y][x] === 'g') obj[y][x] = c;

const at = (x, y) => ({ x: x + OX, y: y + OY });
export const LABELS = [
  { ...at(7.5, 1.5), text: 'Engineering' },
  { ...at(31.5, 1.5), text: 'Design' },
  { ...at(8.5, 15.7), text: 'Coffee Huddle' },
  { ...at(28.5, 15.5), text: 'Lobby' },
  { ...at(36, -5.5), text: 'Garden' },
  { ...at(43.5, 9.5), text: 'Library' },
  { ...at(43.5, 15.5), text: 'Media Room' },
  { ...at(35.5, 16.5), text: 'Lobby Couches' },
  { ...at(9.5, 24.5), text: 'Workspace' },
  { ...at(32.5, 24.5), text: 'Focus Lounge' },
  { ...at(45.5, -5.5), text: 'Campfire' },
  { ...at(4.5, -5.5), text: 'Agent Room' },
];
const PARK_LABELS = [
  { x: PARK_X0 + 24.5, y: 0.6, text: 'Surf Lagoon' },
  { x: PARK_X0 + 51.5, y: 8.4, text: 'Volcano · Jungle River' },
  { x: PARK_X0 + 74.5, y: 1.3, text: 'Shark Reef Tunnel' },
  { x: PARK_X0 + 69.5, y: 10.6, text: 'Skull Rock' },
  { x: PARK_X0 + 80.5, y: 10.4, text: 'Typhoon Twister' },
  { x: PARK_X0 + 23, y: 25.4, text: 'Splash Harbor' },
  { x: PARK_X0 + 70.5, y: 33.4, text: 'Endless Wave' },
  { x: PARK_X0 + 83.5, y: 35.4, text: 'Krakatoa Blaster' },
  { x: PARK_X0 + 5, y: 25.6, text: 'Royal Flush' },
  { x: PARK_X0 + 4.5, y: 16.4, text: 'Park Entrance' },
  { x: 61.5, y: 13.4, text: 'Car Park' },
];
LABELS.push(...PARK_LABELS);
export const DECOR = [
  { ...at(45.5, 1), art: 'mondrian' },
  { ...at(41, 15), art: 'tv' },
  { ...at(35.5, 15), art: 'squiggle' },
];
export const ROOM_ANCHORS = {
  a: at(7, 7), b: at(31, 7), c: at(8, 18), l: at(30, 17), p: at(36, -1),
  k: at(45, 7), m: at(45, 17), s: at(36, 19),
  w: at(19, 27), f: at(32, 27), h: at(45, -1), q: at(13, -3),
};

export function inBounds(x, y) {
  return x >= 0 && y >= 0 && x < W && y < H;
}
export function isWalkable(x, y) {
  if (!inBounds(x, y)) return false;
  const f = floor[y][x];
  return f !== 'g' && f !== '#' && WALKABLE_OBJ.has(obj[y][x]);
}
export function zoneAt(x, y) {
  return inBounds(x, y) ? floor[y][x] : 'g';
}
export function chairDir(x, y) {
  return inBounds(x, y) ? CHAIRS[obj[y][x]] : undefined;
}
export function isCouch(x, y) {
  return inBounds(x, y) && COUCHES.has(obj[y][x]);
}
export const SEATS = [];
for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (chairDir(x, y) !== undefined) SEATS.push([x, y]);

const SPAWNS = [];
for (let y = 22 + OY; y <= 23 + OY; y++) for (let x = 27 + OX; x <= 34 + OX; x++) if (isWalkable(x, y)) SPAWNS.push([x, y]);
export function randomSpawn() {
  const [x, y] = SPAWNS[(Math.random() * SPAWNS.length) | 0];
  return { x, y };
}
