import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { ROOM_ANCHORS, isWalkable, zoneAt } from '../public/map.js';
import {
  TABLES, TABLE_BY_ID, tableAtSeat, cleanMeeting, volumeFor, smooth, fullVolume, meetingVerdict,
  knockAllowed, meetingSince, fmtDuration, MIN_VOLUME, KNOCK_GAP_MS,
} from '../public/features/meetings/tables.js';
import { Mesh, SLOTS, FALLBACK_ICE, iceConfig } from '../public/rtc.js';

const LOBBY = 'l-36-26';
const COFFEE = 'c-14-27';
const LIBRARY = ['k-47-12', 'k-55-12'];
const MEDIA = 'm-50-26';

describe('tables and pure helpers', () => {
  test('finds every table cluster with its tiles and chairs', () => {
    const sizes = { [COFFEE]: [1, 4], [LOBBY]: [4, 4], [LIBRARY[0]]: [1, 4], [LIBRARY[1]]: [1, 4], [MEDIA]: [6, 10] };
    assert.deepEqual(TABLES.map((t) => t.id).sort(), Object.keys(sizes).sort());
    for (const t of TABLES) {
      assert.deepEqual([t.tiles.length, t.chairs.length], sizes[t.id]);
      for (const [x, y] of t.chairs) {
        assert.ok(isWalkable(x, y));
        assert.equal(tableAtSeat(x, y), t.id);
      }
    }
    assert.equal(TABLE_BY_ID.get(LOBBY).zone, 'l');
    assert.equal(TABLE_BY_ID.get(COFFEE).zone, 'c');
    assert.equal(TABLE_BY_ID.get(MEDIA).name, 'Media Room table');
    for (const id of LIBRARY) assert.equal(TABLE_BY_ID.get(id).name, 'Library table');
  });

  test('tableAtSeat is null away from table chairs, including garden benches', () => {
    assert.equal(tableAtSeat(36, 24), null);
    assert.equal(tableAtSeat(22, 3), null);
    assert.equal(tableAtSeat(-1, 3), null);
    assert.equal(tableAtSeat(1.5, 3), null);
  });

  test('cleanMeeting only accepts the table you are seated at', () => {
    const now = 1000;
    assert.deepEqual(cleanMeeting({ table: LOBBY }, 36, 25, now), { table: LOBBY, at: now });
    assert.equal(cleanMeeting({ table: LOBBY }, 14, 26, now), undefined);
    assert.equal(cleanMeeting({ table: 'nope' }, 36, 25, now), undefined);
    assert.equal(cleanMeeting('l-36-26', 36, 25, now), undefined);
    assert.equal(cleanMeeting({ table: LOBBY }, 36, 24, now), undefined);
  });

  test('volumeFor is full up close, fades monotonically and floors at the drop distance', () => {
    assert.equal(volumeFor(0), 1);
    assert.equal(volumeFor(1), 1);
    assert.equal(volumeFor(5), MIN_VOLUME);
    assert.equal(volumeFor(12), MIN_VOLUME);
    let prev = 1;
    for (let d = 1; d <= 5; d += 0.25) {
      const v = volumeFor(d);
      assert.ok(v <= prev + 1e-9, `not monotonic at ${d}`);
      assert.ok(v >= MIN_VOLUME && v <= 1);
      prev = v;
    }
  });

  test('smooth converges toward the target without overshoot', () => {
    assert.equal(smooth(undefined, 0.4, 100), 0.4);
    let v = 1;
    for (let i = 0; i < 40; i++) {
      const n = smooth(v, 0.15, 80);
      assert.ok(n <= v && n >= 0.15);
      v = n;
    }
    assert.equal(v, 0.15);
  });

  test('fullVolume inside private rooms and shared meetings, distance-based in the open', () => {
    const at = (x, y, ex = {}) => ({ x, y, ex });
    assert.equal(fullVolume(at(36, 29), at(38, 29), zoneAt), false);
    const a = ROOM_ANCHORS.a;
    assert.equal(fullVolume(at(a.x, a.y), at(a.x + 1, a.y), zoneAt), true);
    const m = { meeting: { table: LOBBY } };
    assert.equal(fullVolume(at(36, 25, m), at(37, 28, m), zoneAt), true);
  });

  test('meetingVerdict gives tables a bubble', () => {
    const m = (t) => ({ ex: t ? { meeting: { table: t } } : {} });
    assert.equal(meetingVerdict(m(LOBBY), m(LOBBY)), true);
    assert.equal(meetingVerdict(m(LOBBY), m(null)), false);
    assert.equal(meetingVerdict(m(LOBBY), m(COFFEE)), false);
    assert.equal(meetingVerdict(m(null), m(LOBBY)), false);
    assert.equal(meetingVerdict(m(null), m(null)), undefined);
  });

  test('knockAllowed, meetingSince and fmtDuration', () => {
    assert.equal(knockAllowed(undefined, 5), true);
    assert.equal(knockAllowed(1000, 1000 + KNOCK_GAP_MS - 1), false);
    assert.equal(knockAllowed(1000, 1000 + KNOCK_GAP_MS), true);
    const p = (t, at) => ({ ex: { meeting: { table: t, at } } });
    assert.equal(meetingSince([p(LOBBY, 50), p(LOBBY, 20), p(COFFEE, 5)], LOBBY), 20);
    assert.equal(meetingSince([{ ex: {} }], LOBBY), null);
    assert.equal(fmtDuration(0), '0:00');
    assert.equal(fmtDuration(65000), '1:05');
    assert.equal(fmtDuration(3723000), '1:02:03');
  });
});

describe('rtc.js screen slot (mocked RTCPeerConnection)', () => {
  class FakeTrack { constructor(kind, id) { this.kind = kind; this.id = id; } }
  class FakeSender { constructor(track) { this.track = track; this.replaced = []; } async replaceTrack(t) { this.track = t; this.replaced.push(t); } getParameters() { return { encodings: [{}] }; } async setParameters(p) { this.params = p; } }
  class FakeTransceiver {
    constructor(trackOrKind) {
      const kind = typeof trackOrKind === 'string' ? trackOrKind : trackOrKind.kind;
      this.sender = new FakeSender(typeof trackOrKind === 'string' ? null : trackOrKind);
      this.receiver = { track: new FakeTrack(kind, 'remote-' + Math.random()) };
      this.direction = 'sendrecv';
    }
  }
  class FakePC {
    constructor(config) { this.config = config; this.list = []; this.localDescription = null; this.remoteDescription = null; }
    addTransceiver(t) { const tr = new FakeTransceiver(t); this.list.push(tr); return tr; }
    getTransceivers() { return this.list; }
    async setLocalDescription() { this.localDescription = { type: this.remoteDescription ? 'answer' : 'offer', sdp: `m=${this.list.length}` }; }
    async setRemoteDescription(d) { this.remoteDescription = d; if (d.type === 'offer') { const n = Number(d.sdp.slice(2)); for (let i = 0; i < n; i++) this.list.push(new FakeTransceiver(i === 0 ? 'audio' : 'video')); } }
    async addIceCandidate() {}
    close() {}
  }
  class FakeStream { constructor() { this.tracks = []; } addTrack(t) { this.tracks.push(t); } getTracks() { return this.tracks; } }

  before(() => { globalThis.RTCPeerConnection = FakePC; globalThis.MediaStream = FakeStream; });
  after(() => { delete globalThis.RTCPeerConnection; delete globalThis.MediaStream; });

  test('peer connections use the entry ICE servers, or the public STUN fallback when there are none', async () => {
    const turn = { urls: ['turn:app.example:3478?transport=udp'], username: '1:a', credential: 'c' };
    const withTurn = new Mesh('a', () => {}, { onStream() {}, onClose() {}, iceServers: [turn] });
    await withTurn.offer('b');
    assert.deepEqual(withTurn.peers.get('b').pc.config.iceServers, [turn]);
    const bare = new Mesh('a', () => {}, { onStream() {}, onClose() {} });
    await bare.offer('b');
    assert.deepEqual(bare.peers.get('b').pc.config.iceServers, FALLBACK_ICE);
    assert.deepEqual(iceConfig([null, { urls: 7 }]), FALLBACK_ICE);
  });

  test('want() asks for fresh TURN credentials only while someone is nearby', () => {
    let asked = 0;
    const mesh = new Mesh('z', () => {}, { onStream() {}, onClose() {}, refreshIce: () => { asked++; } });
    mesh.want(new Set());
    assert.equal(asked, 0);
    mesh.want(new Set(['a']));
    assert.equal(asked, 1);
  });

  test('offers audio, camera and screen transceivers in that order', async () => {
    const sent = [];
    const mesh = new Mesh('a', (to, d) => sent.push([to, d]), { onStream() {}, onClose() {} });
    const screen = new FakeTrack('video', 'screen');
    mesh.local = { audio: null, video: null, screen };
    await mesh.offer('b');
    const tr = mesh.peers.get('b').pc.getTransceivers();
    assert.deepEqual(SLOTS, ['audio', 'video', 'screen']);
    assert.equal(tr.length, 3);
    assert.equal(tr[2].sender.track, screen);
    assert.equal(tr[1].sender.track, null);
    assert.equal(sent[0][1].sdp.type, 'offer');
  });

  test('answerer maps camera and screen by slot, and setTrack(screen) touches only slot 2', async () => {
    const mesh = new Mesh('b', () => {}, { onStream() {}, onClose() {} });
    const cam = new FakeTrack('video', 'cam');
    mesh.local = { audio: null, video: cam };
    await mesh.handle('a', { sdp: { type: 'offer', sdp: 'm=3' } });
    const tr = mesh.peers.get('a').pc.getTransceivers();
    assert.equal(tr[1].sender.track, cam);
    assert.equal(tr[2].sender.track, null);
    const scr = new FakeTrack('video', 'scr');
    await mesh.setTrack('screen', scr);
    assert.equal(tr[2].sender.track, scr);
    assert.equal(tr[1].sender.track, cam);
    assert.equal(tr[2].sender.params?.degradationPreference, 'maintain-resolution');
    await mesh.setTrack('screen', null);
    assert.equal(tr[2].sender.track, null);
  });

  test('answering a two-slot offer from an older client still works', async () => {
    const mesh = new Mesh('b', () => {}, { onStream() {}, onClose() {} });
    mesh.local = { audio: new FakeTrack('audio', 'mic'), video: null, screen: new FakeTrack('video', 's') };
    await mesh.handle('a', { sdp: { type: 'offer', sdp: 'm=2' } });
    const tr = mesh.peers.get('a').pc.getTransceivers();
    assert.equal(tr.length, 2);
    assert.equal(tr[0].sender.track.id, 'mic');
    await mesh.setTrack('screen', null);
  });

  test('ontrack on the screen slot goes to onScreen, others to onStream', async () => {
    const got = { stream: 0, screen: 0 };
    const mesh = new Mesh('a', () => {}, { onStream() { got.stream++; }, onClose() {}, onScreen() { got.screen++; } });
    await mesh.offer('b');
    const pc = mesh.peers.get('b').pc;
    const tr = pc.getTransceivers();
    for (const t of tr) pc.ontrack({ transceiver: t, track: t.receiver.track });
    assert.deepEqual(got, { stream: 2, screen: 1 });
    assert.equal(mesh.screenOf('b').getTracks().length, 1);
  });
});
