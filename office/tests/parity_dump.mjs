import { floor, obj, W, H, isWalkable, zoneAt } from '../public/map.js';
import { cleanStatus } from '../public/features/presence/logic.js';
import { findDesks, claimDesk, cleanTask, cleanText as workText } from '../public/features/work/logic.js';
import { TABLES, tableAtSeat } from '../public/features/meetings/tables.js';
import { findBoards, sanitizeItem, applyOp, newBoard, thumbOf, chunkItems } from '../public/features/whiteboard/model.js';
import * as P from '../public/features/play/logic.js';
import { cleanRelay } from '../public/features/agents/model.js';
import { cleanName, cleanTz, cleanPic } from '../public/features/profile/logic.js';
import { cleanAvatar, AV_CUSTOM, AV_COMBOS, AV_MAX, FIELDS } from '../public/avatar.js';
import { cleanFocus, joinFocus } from '../public/features/focus/logic.js';
import { isCouch, COUCHES } from '../public/map.js';
import { cleanDeco, decoEmpty, DESK_COLORS, CHAIR_COLORS, SLOTS, ITEMS } from '../public/features/work/deco.js';
import { deskList } from '../public/features/work/logic.js';

const seq = (seed) => () => {
  seed = (seed * 1103515245 + 12345) % 2147483648;
  return seed / 2147483648;
};

const statuses = [
  { s: 'focus', emoji: '🎧', text: '  deep   work\n now ' },
  { s: 'away', auto: true },
  { s: 'break', emoji: '👍🏽x' },
  { s: 'focus', emoji: 'a' },
  { s: 'nope' },
  { s: 'meeting', text: 'x'.repeat(90) },
  { s: 'available', emoji: '👨‍👩‍👧' },
  null,
  [],
];

const items = [
  { id: 'a1', k: 's', c: '#AABBCC', pts: [1.4, 2.6, 3, 5000, -900, 12, 7, 8], w: 99, hl: 1 },
  { id: 'b2', k: 't', c: 'red', x: 10.5, y: 20, text: ' hi\u0001 there ', sz: 5 },
  { id: 'c3', k: 'n', x: 1700, y: -4, text: 'note' },
  { id: 'd4', k: 'r', x: 1, y: 2, x2: 3000, y2: 4, w: 0 },
  { id: 'bad id', k: 's', pts: [1, 2, 3] },
  { id: 'e5', k: 'a', x: 1, y: 2 },
];
const board = newBoard();
const ops = [
  { o: 'add', items },
  { o: 'mv', ids: ['a1', 'b2', 'zz'], dx: 12.5, dy: -3 },
  { o: 'upd', id: 'b2', text: 'changed' },
  { o: 'upd', id: 'b2', text: '   ' },
  { o: 'del', ids: ['c3'] },
  { o: 'nope' },
];
const opResults = ops.map((op) => {
  const r = applyOp(board, op, 'user-123456789');
  return { ok: r.ok, err: r.err, op: r.op, put: r.put.map((i) => i.id), del: r.del };
});

const names = ['  Kaif   Ansari \n', '<b>x</b>', 'x'.repeat(40), null, '\u0000\u0007', 'Zoë Ünïcode', 42];
const zones = ['Asia/Calcutta', ' America/Argentina/Buenos_Aires ', 'Etc/GMT+5', 'UTC', 'a/b/c/d', 'Asia/Cal cutta', '<script>', 'x'.repeat(80), null, '9/x'];
const avatars = [0, 9999, 10000, AV_CUSTOM - 1, AV_CUSTOM, AV_CUSTOM + AV_COMBOS - 1, AV_CUSTOM + AV_COMBOS, -1, 1.5, 7.0, '7', null, true];
const focusCases = [
  { mode: 'focused', mins: 60, topic: '  ship <the> thing ' },
  { mode: 'focused', mins: 61 },
  { mode: 'focused', mins: '60' },
  { mode: 'focused' },
  { mode: 'pomodoro', rounds: 2, topic: 'x'.repeat(90) },
  { mode: 'pomodoro', rounds: 0 },
  { mode: 'pomodoro', rounds: 7 },
  { mode: 'pomodoro', rounds: 1.5 },
  { mode: 'freeform', mins: 15, rounds: 3 },
  { mode: 'nap' },
  null,
  [],
  { mode: 'freeform', emoji: '🚀' },
  { mode: 'focused', mins: 25, emoji: 'ab', topic: 'x' },
  { mode: 'freeform', emoji: '👨‍💻 code' },
  { mode: 'freeform', emoji: 7 },
  { mode: 'freeform', emoji: '' },
];
const focusHost = cleanFocus({ mode: 'pomodoro', rounds: 2, topic: 'deep work' }, true, 1000);
const decoCases = [
  { desk: 1, chair: 7, slots: [1, 8, 11, 14] },
  { slots: [0, 0, 0, 0] },
  { desk: null, chair: null, slots: [3, 0, 0, 0] },
  { desk: 6, slots: [0, 0, 0, 0] },
  { chair: 8, slots: [0, 0, 0, 0] },
  { slots: [9, 0, 0, 0] },
  { slots: [0, 0, 1, 0] },
  { slots: [0, 0, 0, 15] },
  { slots: [0, 0, 0] },
  { slots: [1.5, 0, 0, 0] },
  { desk: '1', slots: [0, 0, 0, 0] },
  { slots: '0000' },
  null,
  [],
];
const pics = ['0123456789abcdef', '0123456789ABCDEF', 'short', null, 1234567890123456];

const pongRng = seq(42);
const g = P.newPong(P.findTables(obj, 'G')[0]);
P.startPong(g, 1000, pongRng);
P.setPad(g, 'L', 0.2);
P.setPad(g, 'R', 'x');
const pongTrace = [];
for (let i = 0; i < 400 && g.status === 'play'; i++) {
  const pt = P.stepPong(g, P.TICK_MS / 1000, 2300 + i * P.TICK_MS, pongRng);
  if (i % 25 === 0 || pt) pongTrace.push({ i, pt, p: P.packPong(g, 2300 + i * P.TICK_MS) });
}

console.log(JSON.stringify({
  W, H,
  floor: floor.map((r) => r.join('')),
  obj: obj.map((r) => r.map((c) => c || '_').join('')),
  walk: floor.map((r, y) => r.map((_, x) => (isWalkable(x, y) ? 1 : 0)).join('')),
  zoneOut: [zoneAt(-1, 0), zoneAt(0, 99)],
  desks: findDesks(obj),
  boards: findBoards(obj),
  tables: TABLES.map((t) => ({ id: t.id, chairs: t.chairs })),
  seatTable: [tableAtSeat(TABLES[0].chairs[0][0], TABLES[0].chairs[0][1]), tableAtSeat(0, 0)],
  pongTables: P.findTables(obj, 'G'),
  statuses: statuses.map((s) => cleanStatus(s) ?? null),
  items: items.map((i) => sanitizeItem(i)),
  opResults,
  boardBytes: board.bytes,
  thumb: thumbOf(board),
  chunks: chunkItems([...board.items.values()], 200).map((c) => c.length),
  workText: workText(' <b>x</b>\n  y ', 5),
  task: [cleanTask({ id: 'T-1', title: ' <hi> ' }), cleanTask({ id: 'bad id', title: 'x' }), cleanTask({ id: 'x', title: '  ' })],
  claim: claimDesk({}, 'a:1', 'Ann', findDesks(obj)[0].x, findDesks(obj)[0].y, findDesks(obj)),
  plausible: [[P.plausibleScore(4, 1000), P.plausibleScore(5, 1000), P.plausibleScore(1.5, 1000), P.plausibleScore(1, -1)]],
  insert: P.insertScore([{ k: 'a', name: 'A', score: 5, ts: 1 }, { k: 'b', name: 'B', score: 3, ts: 2 }], { k: 'c', name: 'C', score: 9, ts: 3 }),
  pongSide: [P.pongSide(P.findTables(obj, 'G')[0], 22, 11), P.pongSide(P.findTables(obj, 'G')[0], 29, 12), P.pongSide(P.findTables(obj, 'G')[0], 25, 11)],
  nearTile: [P.nearTile(obj, 22, 10, 'Q', 2), P.nearTile(obj, 1, 1, 'Q', 2), P.nearTile(obj, 0, 0, 'E', 3)],
  dayKey: P.dayKey(1759190400000),
  pongTrace,
  profile: { names: names.map(cleanName), zones: zones.map(cleanTz), pics: pics.map(cleanPic) },
  deco: {
    desks: DESK_COLORS.length, chairs: CHAIR_COLORS.length, slots: SLOTS, kinds: ITEMS.map((i) => i?.kind ?? null),
    clean: decoCases.map((v) => cleanDeco(v) ?? null),
    empty: decoCases.map((v) => decoEmpty(cleanDeco(v))),
    list: deskList({ 'a:1': { x: 1, y: 2, name: 'Ann', deco: cleanDeco(decoCases[0]) }, 'a:2': { x: 3, y: 4, name: 'Bo' } }),
  },
  couches: { chars: [...COUCHES].sort(), spots: [[36, 33], [36, 37], [34, 34], [17, 24], [15, 24], [-1, 0]].map(([x, y]) => isCouch(x, y)) },
  focus: {
    clean: focusCases.map((v) => cleanFocus(v, true, 5000) ?? null),
    standing: cleanFocus(focusCases[0], false, 5000) ?? null,
    join: [joinFocus(focusHost, 'host1', true, 2000) ?? null, joinFocus(focusHost, 'host1', false, 2000) ?? null, joinFocus(focusHost, 'host1', true, focusHost.end) ?? null,
      joinFocus({ ...focusHost, host: 'root9' }, 'host1', true, 2000) ?? null, joinFocus(null, 'host1', true, 2000) ?? null, joinFocus({ mode: 'freeform', at: 5 }, 'h', true, 10 ** 12) ?? null],
  },
  avatar: { max: AV_MAX, custom: AV_CUSTOM, combos: AV_COMBOS, fields: FIELDS.map((f) => f[1]), clean: avatars.map((v) => cleanAvatar(v) ?? null) },
  relay: cleanRelay([{ id: 'sess-1', st: 'running', at: 1000 }, { id: 'x', st: 'bogus', at: 1 }, { id: 'y', st: 'failed', at: 1.5 }, { id: 'z', st: 'failed', at: 99999999999999 }], 5000),
}));
