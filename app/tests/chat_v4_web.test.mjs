import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { b64ToBytes, b64UrlToBytes, bytesToB64 } from "../public/chat-crypto.js";
import {
  E2EE_RESET_TYPE,
  acceptSealKey,
  createRoomSealStore,
  isSealedFrame,
  kdfCk,
  kdfInit,
  kdfRk,
  sealFrame,
  sealKeyFrame,
  sessionId,
  unsealFrame,
} from "../public/chat-seal.js";

const ROOT = new URL("../../", import.meta.url);
const VECTORS = JSON.parse(readFileSync(new URL("conformance/chat/v4/vectors.json", ROOT), "utf8"));
const ROOM = VECTORS.room;
const SCOPE = VECTORS.scope;
const NOW = VECTORS.now;
const DM = { participants: VECTORS.participants };
const PREFIX = "forkmesh.chat/e2ee/";
const DAY = 24 * 60 * 60 * 1000;
const roomKey = (kind, controller = "") => PREFIX + kind + "/" + Buffer.from(controller + "\n" + ROOM).toString("base64url");

function memoryStorage() {
  const items = new Map();
  return {
    items,
    get length() { return items.size; },
    key: (index) => [...items.keys()][index] ?? null,
    getItem: (key) => (items.has(key) ? items.get(key) : null),
    setItem: (key, value) => items.set(key, String(value)),
    removeItem: (key) => items.delete(key),
  };
}

function clock(start = NOW) {
  const state = { value: start };
  state.now = () => state.value;
  return state;
}

async function vectorDevice(name, time) {
  const secrets = VECTORS.devices[name];
  const storage = memoryStorage();
  storage.setItem(PREFIX + "ed25519IdentityV3", secrets.identitySeed);
  storage.setItem(PREFIX + "prekeysV3", JSON.stringify(secrets.prekeys));
  const store = createRoomSealStore({ storage, acct: name, node: VECTORS.nodes[name], now: time.now, wrappingKey: null });
  const seal = await store.resolve(SCOPE, ROOM, "", DM);
  assert.equal(seal.keyId, secrets.kid);
  for (const bundle of Object.values(VECTORS.bundles)) {
    assert.equal((await acceptSealKey(bundle, seal)).state, "approved");
  }
  return { seal, storage, store };
}

async function pair(time = clock()) {
  const make = (acct) => createRoomSealStore({
    storage: memoryStorage(), acct, node: "node-" + acct, now: time.now, wrappingKey: null,
  });
  const alice = await make("alice").resolve(SCOPE, ROOM, "", DM);
  const bob = await make("bob").resolve(SCOPE, ROOM, "", DM);
  assert.equal((await acceptSealKey(await sealKeyFrame(bob), alice)).state, "approved");
  assert.equal((await acceptSealKey(await sealKeyFrame(alice), bob)).state, "approved");
  return { alice, bob, time };
}

const chat = (text) => ({ type: "chat", id: text, text });

async function waitFor(check, timeoutMs = 10000) {
  const until = Date.now() + timeoutMs;
  for (;;) {
    if (await check()) return;
    if (Date.now() > until) return;
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
}

function shuffled(list, seed) {
  const out = [...list];
  let state = seed;
  for (let i = out.length - 1; i > 0; i -= 1) {
    state = (state * 1103515245 + 12345) % 2147483648;
    const j = state % (i + 1);
    [out[i], out[j]] = [out[j], out[i]];
  }
  return out;
}

function installLocks() {
  let chain = Promise.resolve();
  const requests = [];
  const locks = {
    request(name, task) {
      requests.push(name);
      const run = chain.then(() => task());
      chain = run.catch(() => {});
      return run;
    },
  };
  Object.defineProperty(globalThis, "navigator", { value: { locks }, configurable: true, writable: true });
  return { requests, remove: () => delete globalThis.navigator };
}

test("KDF vectors", async () => {
  const k = VECTORS.kdf;
  assert.equal(await sessionId(b64UrlToBytes(k.sessionId.dh), b64ToBytes(k.sessionId.ct)), k.sessionId.sid);
  const i = k.init;
  const [rk, ck] = await kdfInit(i.room, i.sender, i.recipient, i.spk, i.sid, b64ToBytes(i.ss1), b64ToBytes(i.ss2));
  assert.deepEqual([bytesToB64(rk), bytesToB64(ck)], [i.rk, i.ck]);
  const r = k.rootStep;
  const [nextRk, chain] = await kdfRk(b64ToBytes(r.rk), b64ToBytes(r.dhOut), b64ToBytes(r.ss));
  assert.deepEqual([bytesToB64(nextRk), bytesToB64(chain)], [r.nextRk, r.ck]);
  const c = k.chainStep;
  const [mk, nextCk] = await kdfCk(b64ToBytes(c.ck));
  assert.deepEqual([bytesToB64(mk), bytesToB64(nextCk)], [c.mk, c.nextCk]);
});

test("the vector sequence replays exactly", async () => {
  const devices = {};
  const time = clock();
  for (const step of VECTORS.sequence) {
    const name = step.device;
    if ("state" in step) {
      devices[name] = await vectorDevice(name, time);
      if (step.state) await devices[name].seal.importRatchets(VECTORS.states[step.state]);
    }
    const device = devices[name];
    if (step.clearSeen) device.storage.removeItem(roomKey("seenV3"));
    time.value = step.at;
    const frame = VECTORS.frames[step.frame];
    assert.ok(isSealedFrame(frame), step.frame);
    const result = await unsealFrame(frame, device.seal);
    assert.equal(result.status, step.expect, JSON.stringify(step));
    if (step.expect === "opened") {
      assert.deepEqual(result.plain, VECTORS.plain[step.plain]);
      assert.equal(result.kid, VECTORS.devices[name === "alice" ? "bob" : "alice"].kid);
    }
    if (step.expect === "noSession") assert.equal(result.kid, VECTORS.devices.alice.kid);
    if (step.frame === "m1" && step.expect === "opened") {
      const exported = await device.seal.exportRatchets();
      const session = exported.peers[VECTORS.devices.alice.kid].sessions[0];
      const want = VECTORS.bobAfterM1;
      assert.deepEqual([session.rk, session.ckr, session.nr, session.skipped.length],
        [want.rk, want.ckr, want.nr, want.skipped]);
    }
  }
});

test("exported state has the canonical shape", async () => {
  const time = clock(NOW + 2000);
  const bob = await vectorDevice("bob", time);
  assert.equal((await unsealFrame(VECTORS.frames.m1, bob.seal)).status, "opened");
  const stored = JSON.parse(bob.storage.getItem(roomKey("ratchetV4")));
  assert.equal(stored.v, 4);
  const session = stored.peers[VECTORS.devices.alice.kid].sessions[0];
  assert.deepEqual(Object.keys(session).sort(),
    ["ckr", "cks", "created", "dhr", "dhs", "nr", "ns", "pn", "rk", "sid", "skipped", "sq", "used"]);
  assert.equal(stored.peers[VECTORS.devices.alice.kid].current, session.sid);
  assert.equal(session.dhr, VECTORS.headers.m1.dh);
});

test("a conversation with shuffled delivery opens every frame", async () => {
  const { alice, bob, time } = await pair();
  const roles = [[alice, bob], [bob, alice]];
  for (let turn = 0; turn < 6; turn += 1) {
    const [sender, receiver] = roles[turn % 2];
    const frames = [];
    for (let i = 0; i < 5; i += 1) frames.push(await sealFrame(chat(`${turn}-${i}`), sender));
    assert.ok(frames.every((frame) => frame.v === 4 && frame.keys.length === 1 && frame.keys[0].kid === receiver.keyId));
    for (const frame of shuffled(frames, turn + 7)) {
      const result = await unsealFrame(frame, receiver);
      assert.equal(result.status, "opened", `${turn}`);
    }
    time.value += 100;
  }
});

test("a late message from an old chain still opens once", async () => {
  const { alice, bob } = await pair();
  const late = await sealFrame(chat("late"), alice);
  assert.equal((await unsealFrame(await sealFrame(chat("a2"), alice), bob)).status, "opened");
  assert.equal((await unsealFrame(await sealFrame(chat("b1"), bob), alice)).status, "opened");
  const a3 = await sealFrame(chat("a3"), alice);
  assert.equal(a3.keys[0].pn, 2);
  assert.equal((await unsealFrame(a3, bob)).status, "opened");
  assert.equal((await unsealFrame(late, bob)).plain.text, "late");
  assert.equal((await unsealFrame(late, bob)).status, "replay");
});

test("simultaneous first messages converge on one session", async () => {
  const { alice, bob } = await pair();
  const fromAlice = await sealFrame(chat("a"), alice);
  const fromBob = await sealFrame(chat("b"), bob);
  assert.equal((await unsealFrame(fromAlice, bob)).status, "opened");
  assert.equal((await unsealFrame(fromBob, alice)).status, "opened");
  for (let i = 0; i < 3; i += 1) {
    assert.equal((await unsealFrame(await sealFrame(chat(`a${i}`), alice), bob)).status, "opened");
    assert.equal((await unsealFrame(await sealFrame(chat(`b${i}`), bob), alice)).status, "opened");
  }
  const aliceState = await alice.exportRatchets();
  const bobState = await bob.exportRatchets();
  assert.equal(aliceState.peers[bob.keyId].current, bobState.peers[alice.keyId].current);
});

test("a reset recovers a lost session", async () => {
  const { alice, bob } = await pair();
  assert.equal((await unsealFrame(await sealFrame(chat("a1"), alice), bob)).status, "opened");
  assert.equal((await unsealFrame(await sealFrame(chat("b1"), bob), alice)).status, "opened");
  await bob.importRatchets({ v: 4, peers: {} });
  const broken = await sealFrame(chat("a2"), alice);
  assert.deepEqual(await unsealFrame(broken, bob), { status: "noSession", kid: alice.keyId, later: false });
  await bob.resetPeer(alice.keyId);
  const reset = await sealFrame({ type: E2EE_RESET_TYPE, senderId: "node-bob" }, bob, ROOM, { only: [alice.keyId] });
  assert.deepEqual(reset.keys.map((entry) => entry.kid), [alice.keyId]);
  assert.equal(reset.keys[0].pn, 0);
  const opened = await unsealFrame(reset, alice);
  assert.equal(opened.status, "opened");
  assert.equal(opened.plain.type, E2EE_RESET_TYPE);
  assert.equal((await unsealFrame(await sealFrame(chat("a2 again"), alice), bob)).status, "opened");
});

test("a peek does not change any state", async () => {
  const { alice, bob } = await pair();
  const frame = await sealFrame(chat("a1"), alice);
  const before = JSON.stringify(await bob.exportRatchets());
  const seenBefore = bob.ctx.storage.getItem(roomKey("seenV3"));
  assert.equal((await unsealFrame(frame, bob, ROOM, { commit: false })).status, "opened");
  assert.equal(JSON.stringify(await bob.exportRatchets()), before);
  assert.equal(bob.ctx.storage.getItem(roomKey("seenV3")), seenBefore);
  assert.equal((await unsealFrame(frame, bob)).status, "opened");
  assert.equal((await unsealFrame(frame, bob)).status, "replay");
});

test("more than MAX_SKIP missing messages need a reset", async () => {
  const { alice, bob } = await pair();
  for (let i = 0; i < 257; i += 1) await sealFrame(chat("lost"), alice);
  const far = await sealFrame(chat("far"), alice);
  assert.equal(far.keys[0].n, 257);
  assert.equal((await unsealFrame(far, bob)).status, "noSession");
});

test("sessions and skipped keys are bounded", async () => {
  const { alice, bob, time } = await pair();
  for (let i = 0; i < 7; i += 1) {
    await alice.importRatchets({ v: 4, peers: {} });
    time.value += 1;
    assert.equal((await unsealFrame(await sealFrame(chat(String(i)), alice), bob)).status, "opened");
  }
  const peer = (await bob.exportRatchets()).peers[alice.keyId];
  assert.equal(peer.sessions.length, 4);
  assert.equal(peer.sessions[0].sid, peer.current);
  for (let i = 0; i < 3; i += 1) await sealFrame(chat("skipped"), alice);
  assert.equal((await unsealFrame(await sealFrame(chat("next"), alice), bob)).status, "opened");
  assert.equal((await bob.exportRatchets()).peers[alice.keyId].sessions[0].skipped.length, 3);
  time.value += 8 * DAY + 1000;
  assert.equal((await acceptSealKey(await sealKeyFrame(bob), alice)).refreshed, true);
  await alice.importRatchets({ v: 4, peers: {} });
  assert.equal((await unsealFrame(await sealFrame(chat("much later"), alice), bob)).status, "opened");
  const later = (await bob.exportRatchets()).peers[alice.keyId];
  assert.equal(later.sessions.length, 4);
  assert.ok(later.sessions.every((session) => session.skipped.length === 0));
});

test("strict rooms carry only v4, open rooms only v3, and empty frames are legal", async () => {
  const time = clock();
  const make = (acct) => createRoomSealStore({
    storage: memoryStorage(), acct, node: "node-" + acct, now: time.now, wrappingKey: null,
  });
  const aliceStore = make("alice");
  const alice = await aliceStore.resolve(SCOPE, ROOM, "", DM);
  const bob = await make("bob").resolve(SCOPE, ROOM, "", DM);
  const empty = await sealFrame(chat("nobody yet"), alice);
  assert.equal(empty.v, 4);
  assert.deepEqual(empty.keys, []);
  assert.ok(isSealedFrame(empty));
  assert.equal((await acceptSealKey(await sealKeyFrame(alice), bob)).state, "approved");
  assert.equal((await unsealFrame(empty, bob)).status, "notForMe");
  assert.equal((await unsealFrame(empty, alice)).status, "own");
  const aliceV3 = await aliceStore.resolve(SCOPE, ROOM, "", { ...DM, version: 3 });
  assert.equal((await acceptSealKey(await sealKeyFrame(bob), aliceV3)).state, "approved");
  const v3 = await sealFrame(chat("old protocol"), aliceV3);
  assert.equal(v3.v, 3);
  assert.equal((await unsealFrame(v3, bob)).status, "invalid");
  const open = await aliceStore.resolve("private:room", "chat-room:v1");
  const openBob = await make("bob").resolve("private:room", "chat-room:v1");
  await acceptSealKey(await sealKeyFrame(open), openBob);
  await acceptSealKey(await sealKeyFrame(openBob), open);
  const openFrame = await sealFrame(chat("channel"), open);
  assert.equal(openFrame.v, 3);
  assert.equal(open.version, 3);
  const v4InOpen = { ...VECTORS.frames.m1 };
  assert.equal((await unsealFrame(v4InOpen, openBob, "chat-room:v1")).status, "invalid");
  const withSelf = JSON.parse(JSON.stringify(await sealFrame(chat("x"), bob)));
  withSelf.keys.push({ ...withSelf.keys[0], kid: bob.keyId });
  withSelf.keys.sort((a, b) => (a.kid < b.kid ? -1 : 1));
  assert.equal((await unsealFrame(withSelf, alice)).status, "invalid");
});

test("the ratchet is wrapped when a wrapping key is available", async () => {
  const wrap = await globalThis.crypto.subtle.generateKey({ name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"]);
  const storage = memoryStorage();
  const time = clock();
  const alice = await createRoomSealStore({ storage, acct: "alice", node: "node-alice", now: time.now, wrappingKey: async () => wrap })
    .resolve(SCOPE, ROOM, "", DM);
  const bob = await createRoomSealStore({ storage: memoryStorage(), acct: "bob", node: "node-bob", now: time.now, wrappingKey: null })
    .resolve(SCOPE, ROOM, "", DM);
  await acceptSealKey(await sealKeyFrame(bob), alice);
  await acceptSealKey(await sealKeyFrame(alice), bob);
  const frame = await sealFrame(chat("wrapped"), alice);
  assert.equal(JSON.parse(storage.getItem(roomKey("ratchetV4"))).wrapped, 1);
  assert.equal(JSON.parse(storage.getItem(roomKey("openedV4"))).wrapped, 1);
  assert.equal((await unsealFrame(frame, bob)).status, "opened");
  assert.equal((await alice.exportRatchets()).peers[bob.keyId].sessions[0].ns, 1);
});

test("tabs share one ratchet under the Web Lock and show what another tab opened", async () => {
  const locks = installLocks();
  try {
    const time = clock();
    const shared = memoryStorage();
    const tab = () => createRoomSealStore({ storage: shared, acct: "bob", node: "node-bob", now: time.now, wrappingKey: null })
      .resolve(SCOPE, ROOM, "", DM);
    const tabA = await tab();
    const tabB = await tab();
    assert.equal(tabA.keyId, tabB.keyId);
    const alice = await createRoomSealStore({ storage: memoryStorage(), acct: "alice", node: "node-alice", now: time.now, wrappingKey: null })
      .resolve(SCOPE, ROOM, "", DM);
    await acceptSealKey(await sealKeyFrame(tabA), alice);
    await acceptSealKey(await sealKeyFrame(alice), tabA);

    const frames = [];
    for (let i = 0; i < 4; i += 1) frames.push(await sealFrame(chat(`a${i}`), alice));
    const firstPass = await Promise.all(frames.map((frame, i) => unsealFrame(frame, i % 2 ? tabA : tabB)));
    assert.deepEqual(firstPass.map((result) => result.status), ["opened", "opened", "opened", "opened"]);
    const other = await unsealFrame(frames[0], tabA);
    assert.equal(other.status, "opened");
    assert.equal(other.plain.text, "a0");
    assert.equal((await unsealFrame(frames[0], tabA)).status, "replay");
    assert.equal((await unsealFrame(frames[0], tabB)).status, "replay");

    const sent = await Promise.all([
      sealFrame(chat("b-from-A"), tabA),
      sealFrame(chat("b-from-B"), tabB),
      sealFrame(chat("b-from-A-2"), tabA),
    ]);
    assert.deepEqual(sent.map((frame) => frame.keys[0].n), [0, 1, 2]);
    assert.deepEqual(sent.map((frame) => frame.ctr), [1, 2, 3]);
    for (const frame of shuffled(sent, 3)) assert.equal((await unsealFrame(frame, alice)).status, "opened");

    const ownElsewhere = await unsealFrame(sent[0], tabB);
    assert.equal(ownElsewhere.status, "own");
    assert.equal(ownElsewhere.plain.text, "b-from-A");
    assert.deepEqual(await unsealFrame(sent[0], tabA), { status: "own" });
    assert.ok(locks.requests.length > 0);
    assert.ok(locks.requests.every((name) => name === "forkmesh-chat-ratchet"));

    time.value += 11 * 60 * 1000;
    const late = await createRoomSealStore({ storage: shared, acct: "bob", node: "node-bob", now: time.now, wrappingKey: null })
      .resolve(SCOPE, ROOM, "", DM);
    assert.equal((await unsealFrame(frames[1], late)).status, "replay");
  } finally {
    locks.remove();
  }
});

class FakeWebSocket {
  static OPEN = 1;
  static instances = [];
  constructor() {
    this.readyState = 0;
    this.sent = [];
    this.listeners = new Map();
    FakeWebSocket.instances.push(this);
  }
  addEventListener(type, callback) {
    const entries = this.listeners.get(type) || [];
    entries.push(callback);
    this.listeners.set(type, entries);
  }
  emit(type, event = {}) {
    for (const callback of this.listeners.get(type) || []) callback(event);
  }
  open() {
    this.readyState = FakeWebSocket.OPEN;
    this.emit("open");
  }
  send(value) { this.sent.push(value); }
  close() { this.readyState = 3; }
}

test("the DM transport ignores own frames, resets lost sessions and reseals to a reset device", async () => {
  const { createChatRoomTransport } = await import("../public/chat-room-transport.js");
  const { decryptObject, derivePassphraseKey, encryptObject } = await import("../public/chat-crypto.js");
  const roomName = "chat-direct:gamma:v1";
  const scope = "direct:0123456789abcdef0123456789abcdef";
  const dm = { participants: ["me", "peer"] };
  const me = createRoomSealStore({ storage: memoryStorage(), acct: "me", node: "node-me", wrappingKey: null });
  const peerStorage = memoryStorage();
  const peerStore = createRoomSealStore({ storage: peerStorage, acct: "peer", node: "node-peer", wrappingKey: null });
  const peer = await peerStore.resolve(scope, roomName, "", dm);
  const live = [];
  const transport = createChatRoomTransport({
    authorize: async () => ({ scope, passphrase: "room secret", roomName, webSocketUrl: "/ws" }),
    isDirectRoom: () => true,
    resolveSeal: (room, name) => me.resolve(room.scope, name, "", dm),
    onPlain: (plain) => live.push(plain),
    WebSocketImpl: FakeWebSocket,
    locationLike: { protocol: "https:", host: "relay.example" },
  });
  await transport.connect({ scope });
  const socket = FakeWebSocket.instances.at(-1);
  socket.open();
  const key = await derivePassphraseKey("room secret", roomName);
  const settle = () => new Promise((resolve) => setTimeout(resolve, 200));
  const deliver = async (inner, extra = {}) => {
    socket.emit("message", { data: JSON.stringify({ ...(await encryptObject(inner, key)), ...extra }) });
    await settle();
  };
  const sentSince = async (start) => {
    const out = [];
    for (const raw of socket.sent.slice(start)) {
      const envelope = JSON.parse(raw);
      out.push({ envelope, inner: envelope.kind === "cipher" ? await decryptObject(envelope, key) : null });
    }
    return out;
  };
  socket.emit("message", { data: JSON.stringify({ kind: "forkmesh-history-end", v: 1 }) });
  await settle();
  const mySeal = await me.resolve(scope, roomName, "", dm);
  await acceptSealKey(await sealKeyFrame(mySeal), peer);
  await deliver(await sealKeyFrame(peer));

  let mark = socket.sent.length;
  await transport.send({ type: "chat", id: "m1", senderId: "node-me", text: "first", ts: Date.now() }, { persist: true });
  await transport.send({ type: "chat", id: "m2", senderId: "node-me", text: "second", ts: Date.now() }, { persist: true });
  const mine = (await sentSince(mark)).filter((item) => item.inner?.type === "sealed");
  assert.equal(mine.length, 2);
  assert.deepEqual(mine[0].inner.keys.map((entry) => entry.kid), [peer.keyId]);
  for (const item of mine) assert.equal((await unsealFrame(item.inner, peer)).status, "opened");

  mark = socket.sent.length;
  await deliver(mine[0].inner, { persist: true, ref: "e".repeat(40) });
  const afterOwn = await sentSince(mark);
  assert.equal(afterOwn.length, 0);
  assert.ok(!live.some((plain) => plain.id === "m1"));

  await deliver(await sealFrame({ type: "chat", id: "p1", senderId: "node-peer", text: "hello" }, peer), {
    persist: true, ref: "a".repeat(40),
  });
  assert.equal(live.at(-1).id, "p1");

  await mySeal.importRatchets({ v: 4, peers: {} });
  const p4 = await sealFrame({ type: "chat", id: "p4", senderId: "node-peer", text: "lost" }, peer);
  const p5 = await sealFrame({ type: "chat", id: "p5", senderId: "node-peer", text: "lost too" }, peer);
  mark = socket.sent.length;
  await deliver(p4, { persist: true, ref: "b".repeat(40) });
  await waitFor(async () => (await sentSince(mark)).filter((item) => item.inner?.type === "e2ee-key").length > 0);
  await settle();
  assert.ok(!live.some((plain) => plain.id === "p4"));
  const afterLoss = await sentSince(mark);
  const resets = afterLoss.filter((item) => item.inner?.type === "sealed");
  const adverts = afterLoss.filter((item) => item.inner?.type === "e2ee-key");
  const receipts = afterLoss.filter((item) => item.envelope.kind === "forkmesh-received");
  assert.equal(resets.length, 1);
  assert.equal(resets[0].envelope.persist, true);
  assert.equal(resets[0].envelope.notify, undefined);
  assert.deepEqual(resets[0].inner.keys.map((entry) => entry.kid), [peer.keyId]);
  assert.equal(adverts.length, 1);
  assert.equal(adverts[0].envelope.persist, true);
  assert.equal(receipts.length, 0);

  mark = socket.sent.length;
  await deliver(p5, { persist: true, ref: "c".repeat(40) });
  await settle();
  assert.equal((await sentSince(mark)).length, 0);

  const resetOpened = await unsealFrame(resets[0].inner, peer);
  assert.equal(resetOpened.status, "opened");
  assert.equal(resetOpened.plain.type, E2EE_RESET_TYPE);
  assert.equal(resetOpened.plain.senderId, "node-me");
  await deliver(await sealFrame({ type: "chat", id: "p6", senderId: "node-peer", text: "recovered" }, peer), {
    persist: true, ref: "f".repeat(40),
  });
  assert.equal(live.at(-1).id, "p6");
  const recovered = JSON.stringify(await mySeal.exportRatchets());
  const realNow = Date.now;
  Date.now = () => realNow() + 120000;
  try {
    mark = socket.sent.length;
    await deliver(p4, { persist: true, ref: "b".repeat(40) });
    await settle();
    assert.equal((await sentSince(mark)).filter((item) => item.envelope.kind === "cipher").length, 0);
    assert.equal(JSON.stringify(await mySeal.exportRatchets()), recovered);
  } finally {
    Date.now = realNow;
  }
  assert.equal((await unsealFrame(p5, mySeal)).later, true);

  const phone = await createRoomSealStore({ storage: memoryStorage(), acct: "peer", node: "node-peer-2", wrappingKey: null })
    .resolve(scope, roomName, "", dm);
  await deliver(await sealKeyFrame(phone));
  const beforeApproval = socket.sent.length;
  await transport.approveDevice(phone.keyId);
  await waitFor(async () => (await sentSince(beforeApproval)).some((item) => item.envelope.kind === "forkmesh-superseded"));
  await settle();

  mark = socket.sent.length;
  const peerReset = await sealFrame({ type: E2EE_RESET_TYPE, senderId: "node-peer" }, peer, roomName, { only: [mySeal.keyId] });
  await deliver(peerReset, { persist: true, ref: "d".repeat(40) });
  await waitFor(async () => (await sentSince(mark)).filter((item) => item.envelope.resend === true).length >= 2 &&
    (await sentSince(mark)).some((item) => item.envelope.kind === "forkmesh-received"));
  await settle();
  const afterReset = await sentSince(mark);
  assert.ok(!live.some((plain) => plain.type === E2EE_RESET_TYPE));
  const resent = afterReset.filter((item) => item.envelope.resend === true);
  assert.deepEqual(resent.map((item) => item.inner.keys.map((entry) => entry.kid)), [[peer.keyId], [peer.keyId]]);
  assert.ok(resent.every((item) => item.envelope.persist === true && item.envelope.notify === true));
  assert.ok(!afterReset.some((item) => item.envelope.kind === "forkmesh-superseded"));
  assert.ok(afterReset.some((item) => item.envelope.kind === "forkmesh-received" && item.envelope.refs.includes("d".repeat(40))));
  const reopened = [];
  for (const item of resent) reopened.push(await unsealFrame(item.inner, peer));
  assert.deepEqual(reopened.map((result) => result.status), ["opened", "opened"]);
  assert.deepEqual(reopened.map((result) => result.plain.id).sort(), ["m1", "m2"]);
  assert.ok(peerStorage.getItem(PREFIX + "ratchetV4/" + Buffer.from("\n" + roomName).toString("base64url")));

  socket.emit("close", { code: 1000 });
  transport.dispose();
});

test("a frame with no recipients is still sent and resealed once a peer appears", async () => {
  const { createChatRoomTransport } = await import("../public/chat-room-transport.js");
  const { decryptObject, derivePassphraseKey, encryptObject } = await import("../public/chat-crypto.js");
  const roomName = "chat-direct:delta:v1";
  const scope = "direct:delta";
  const me = createRoomSealStore({ storage: memoryStorage(), acct: "me", node: "node-me", wrappingKey: null });
  const peer = await createRoomSealStore({ storage: memoryStorage(), acct: "peer", node: "node-peer", wrappingKey: null })
    .resolve(scope, roomName);
  const transport = createChatRoomTransport({
    authorize: async () => ({ scope, passphrase: "room secret", roomName, webSocketUrl: "/ws" }),
    isDirectRoom: () => true,
    resolveSeal: (room, name) => me.resolve(room.scope, name),
    WebSocketImpl: FakeWebSocket,
    locationLike: { protocol: "https:", host: "relay.example" },
  });
  await transport.connect({ scope });
  const socket = FakeWebSocket.instances.at(-1);
  socket.open();
  const key = await derivePassphraseKey("room secret", roomName);
  const settle = () => new Promise((resolve) => setTimeout(resolve, 200));
  socket.emit("message", { data: JSON.stringify({ kind: "forkmesh-history-end", v: 1 }) });
  await settle();
  assert.equal(await transport.send({ type: "chat", id: "z1", senderId: "node-me", text: "anyone?", ts: Date.now() }, { persist: true }), true);
  const first = await decryptObject(JSON.parse(socket.sent.at(-1)), key);
  assert.equal(first.v, 4);
  assert.deepEqual(first.keys, []);
  const mark = socket.sent.length;
  socket.emit("message", { data: JSON.stringify({ ...(await encryptObject(await sealKeyFrame(peer), key)), persist: true }) });
  await waitFor(() => socket.sent.slice(mark).some((raw) => JSON.parse(raw).kind === "forkmesh-superseded"));
  const after = socket.sent.slice(mark).map((raw) => JSON.parse(raw));
  const resent = after.find((envelope) => envelope.resend === true);
  assert.ok(resent);
  assert.ok(after.some((envelope) => envelope.kind === "forkmesh-superseded"));
  for (const raw of socket.sent) {
    const inner = await decryptObject(JSON.parse(raw), key);
    if (inner?.type === "e2ee-key") await acceptSealKey(inner, peer);
  }
  const opened = await unsealFrame(await decryptObject(resent, key), peer);
  assert.equal(opened.status, "opened");
  assert.equal(opened.plain.text, "anyone?");
  transport.dispose();
});
