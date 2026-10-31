import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

import { b64ToBytes, b64UrlToBytes, bytesToB64, bytesToB64Url } from "../public/chat-crypto.js";
import {
  Keccak,
  mlkemDecapsulate,
  mlkemEncapsulate,
  mlkemKeyPair,
  sha3_256,
  shake256,
} from "../public/chat-mlkem.js";
import {
  createRoomSealStore,
  deriveKek,
  isSealKeyFrame,
  isSealedFrame,
  prekeyFromSecrets,
  safetyNumber,
  sealFrame,
  sealKeyFrame,
  unsealFrame,
  acceptSealKey,
  verifyBundle,
  x25519Shared,
} from "../public/chat-seal.js";

const ROOT = new URL("../../", import.meta.url);
const VECTORS = JSON.parse(readFileSync(new URL("conformance/chat/v3/vectors.json", ROOT), "utf8"));
const KAT = Object.fromEntries(
  readFileSync(new URL("native/android/core/src/test/resources/crypto/mlkem768-openssl-kat.txt", ROOT), "utf8")
    .split("\n")
    .filter((line) => line.includes("=") && !line.startsWith("#"))
    .map((line) => [line.slice(0, line.indexOf("=")), line.slice(line.indexOf("=") + 1)]),
);
const ROOM = VECTORS.room;
const SCOPE = VECTORS.scope;
const NOW = VECTORS.now;
const PARTICIPANTS = VECTORS.participants;
const NODES = VECTORS.nodes;
const DM = { participants: PARTICIPANTS, version: 3 };
const PREFIX = "forkmesh.chat/e2ee/";

const hex = (text) => Uint8Array.from(Buffer.from(text, "hex"));
const toHex = (bytes) => Buffer.from(bytes).toString("hex");
const sha256Hex = (bytes) => createHash("sha256").update(bytes).digest("hex");

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

function deviceStore(name, acct, time = clock(), node = NODES[name]) {
  const secrets = VECTORS.devices[name];
  const storage = memoryStorage();
  storage.setItem(PREFIX + "ed25519IdentityV3", secrets.identitySeed);
  storage.setItem(PREFIX + "prekeysV3", JSON.stringify(secrets.prekeys));
  const store = createRoomSealStore({ storage, acct, node, now: time.now, wrappingKey: null });
  return { store, storage, time, secrets };
}

async function bobWithTrust() {
  const bob = deviceStore("bob", "bob");
  const seal = await bob.store.resolve(SCOPE, ROOM, "", DM);
  const bundles = VECTORS.bundles;
  assert.equal((await acceptSealKey(bundles.alice, seal)).state, "approved");
  assert.equal((await acceptSealKey(bundles.bob, seal)).state, "approved");
  assert.equal((await acceptSealKey(bundles.alice2, seal)).state, "pending");
  return { ...bob, seal };
}

test("Keccak and ML-KEM-768 match the OpenSSL known answers", () => {
  assert.equal(toHex(sha3_256(new Uint8Array())), "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
  const pair = mlkemKeyPair(hex(KAT.seed));
  assert.equal(sha256Hex(pair.publicKey), KAT.ek_sha256);
  assert.equal(sha256Hex(pair.secretKey), KAT.dk_sha256);
  assert.equal(toHex(mlkemDecapsulate(pair.secretKey, hex(KAT.ciphertext))), KAT.shared_secret);
  assert.equal(toHex(mlkemDecapsulate(pair.secretKey, hex(KAT.tampered_ciphertext))), KAT.rejection_secret);
});

test("ML-KEM-768 accumulated known answer over 100 iterations", () => {
  const source = Keccak.shake128();
  const output = Keccak.shake128();
  for (let i = 0; i < 100; i += 1) {
    const seed = source.squeeze(new Uint8Array(64));
    const pair = mlkemKeyPair(seed);
    output.update(pair.publicKey);
    const message = source.squeeze(new Uint8Array(32));
    const enc = mlkemEncapsulate(pair.publicKey, message);
    output.update(enc.ciphertext);
    output.update(enc.sharedSecret);
    assert.deepEqual(mlkemDecapsulate(pair.secretKey, enc.ciphertext), enc.sharedSecret);
    const random = source.squeeze(new Uint8Array(1088));
    output.update(mlkemDecapsulate(pair.secretKey, random));
  }
  assert.equal(toHex(output.digest(32)), "1114b1b6699ed191734fa339376afa7e285c9e6acf6ff0177d346696ce564415");
});

test("ML-KEM-768 rejects malformed input and implicitly rejects tampered ciphertexts", () => {
  const pair = mlkemKeyPair(new Uint8Array(64).fill(3));
  const bad = pair.publicKey.slice();
  bad[0] = 0xff;
  bad[1] |= 0x0f;
  assert.throws(() => mlkemEncapsulate(bad));
  assert.throws(() => mlkemEncapsulate(pair.publicKey.slice(0, 1183)));
  assert.throws(() => mlkemDecapsulate(pair.secretKey, new Uint8Array(1087)));
  assert.throws(() => mlkemKeyPair(new Uint8Array(32)));
  const enc = mlkemEncapsulate(pair.publicKey);
  const tampered = enc.ciphertext.slice();
  tampered[500] ^= 1;
  const z = pair.secretKey.slice(2400 - 32);
  assert.deepEqual(mlkemDecapsulate(pair.secretKey, tampered), shake256(32, z, tampered));
});

test("devices rebuild from their secrets", async () => {
  for (const [name, secrets] of Object.entries(VECTORS.devices)) {
    const { store } = deviceStore(name, name.replace(/2$/, ""));
    const identity = await store.identity();
    assert.equal(identity.idk, secrets.idk, name);
    assert.equal(identity.kid, secrets.kid, name);
    for (const spk of secrets.prekeys) {
      const made = await prekeyFromSecrets(b64ToBytes(spk.xPriv), b64ToBytes(spk.pqSeed), spk.ts);
      assert.equal(made.id, spk.id);
      assert.equal(bytesToB64Url(made.x), spk.x);
      assert.equal(bytesToB64Url(made.pq), spk.pq);
      assert.equal(made.exp, spk.exp);
    }
  }
});

test("bundles verify and invalid bundles do not", async () => {
  const bundles = VECTORS.bundles;
  for (const name of ["alice", "alice2", "bob", "bobOld", "injectedOutsider", "injectedAsBob"]) {
    assert.ok(isSealKeyFrame(bundles[name]), name);
    assert.ok(await verifyBundle(bundles[name], ROOM, NOW), name);
  }
  for (const [name, bundle] of Object.entries(bundles.invalid)) {
    assert.equal(await verifyBundle(bundle, ROOM, NOW), false, name);
  }
  assert.equal(await verifyBundle(bundles.alice, ROOM, VECTORS.bundleChecks.expiredAt), false);
  assert.equal(VECTORS.bundleChecks.pendingStateForAlice2AtBob, "pending");
});

test("a bundle for a known kid cannot change its account", async () => {
  const { seal } = await bobWithTrust();
  const changed = { ...VECTORS.bundles.alice, acct: "mallory" };
  assert.equal(await acceptSealKey(changed, seal), null);
  assert.equal(await acceptSealKey(VECTORS.bundles.invalid.wrongNode, seal), null);
  const alice = seal.devices().find((record) => record.kid === VECTORS.devices.alice.kid);
  assert.equal(alice.acct, "alice");
});

test("server-injected devices are refused in a DM or held when they claim a participant", async () => {
  const { seal, store } = await bobWithTrust();
  const bundles = VECTORS.bundles;
  assert.equal(await acceptSealKey(bundles.injectedOutsider, seal), null);
  assert.ok(!seal.devices().some((record) => record.acct === "mallory"));
  const asBob = await acceptSealKey(bundles.injectedAsBob, seal);
  assert.equal(asBob.state, "pending");
  assert.equal(asBob.node, "node-bob-new");
  assert.ok(!seal.recipientKeyIds().includes(asBob.kid));
  const open = await store.resolve("private:open", ROOM);
  assert.equal(open.policy, "open");
  assert.equal(await acceptSealKey(bundles.injectedOutsider, open), null);
});

test("a bundle the device made itself rebuilds the vector bundle", async () => {
  const { store } = deviceStore("alice", "alice");
  const seal = await store.resolve(SCOPE, ROOM, "", DM);
  const bundle = await sealKeyFrame(seal);
  assert.deepEqual(bundle, VECTORS.bundles.alice);
});

test("the relay sequence opens, rejects replays, tampering and forgeries, and holds a pending device", async () => {
  const { seal, time } = await bobWithTrust();
  const sealed = VECTORS.sealed;
  assert.ok(isSealedFrame(sealed.first.frame));
  const first = await unsealFrame(sealed.first.frame, seal);
  assert.equal(first.status, "opened");
  assert.deepEqual(first.plain, sealed.first.plain);
  assert.equal(first.kid, sealed.first.senderKid);
  assert.equal(first.acct, "alice");
  assert.equal(first.node, NODES.alice);
  assert.equal(first.plain.senderId, NODES.alice);
  assert.equal((await unsealFrame(sealed.replay.frame, seal)).status, "replay");
  time.value = NOW + 1000;
  const second = await unsealFrame(sealed.second.frame, seal);
  assert.equal(second.status, "opened");
  assert.deepEqual(second.plain, sealed.second.plain);
  time.value = NOW;
  assert.equal((await unsealFrame(sealed.tampered.frame, seal)).status, "invalid");
  time.value = NOW + 2000;
  assert.equal((await unsealFrame(sealed.forgedSenderId.frame, seal)).status, "invalid");
  const pending = sealed.fromPendingDevice;
  assert.deepEqual(pending.frame.keys.map((entry) => entry.kid), pending.keys);
  time.value = NOW + 3000;
  const held = await unsealFrame(pending.frame, seal);
  assert.equal(held.status, "held");
  assert.equal(held.kid, VECTORS.devices.alice2.kid);
  assert.equal(seal.pendingDevices().length, 1);
  const released = await seal.approve(VECTORS.devices.alice2.kid);
  assert.equal(released.length, 1);
  assert.equal(released[0].status, pending.afterApproval);
  assert.equal(released[0].plain.text, "from the second phone");
  assert.equal((await unsealFrame(pending.frame, seal)).status, "replay");
});

test("held frames survive a restart and open once the device is approved", async () => {
  const { seal, storage, time } = await bobWithTrust();
  time.value = NOW + 3000;
  assert.equal((await unsealFrame(VECTORS.sealed.fromPendingDevice.frame, seal)).status, "held");
  const revived = createRoomSealStore({ storage, acct: "bob", node: NODES.bob, now: time.now, wrappingKey: null });
  const again = await revived.resolve(SCOPE, ROOM, "", DM);
  assert.equal(again.pendingDevices()[0].kid, VECTORS.devices.alice2.kid);
  const released = await again.approve(VECTORS.devices.alice2.kid);
  assert.equal(released[0]?.status, "opened");
});

test("a forged frame does not burn a counter", async () => {
  const { seal } = await bobWithTrust();
  assert.equal((await unsealFrame(VECTORS.sealed.tampered.frame, seal)).status, "invalid");
  assert.equal((await unsealFrame(VECTORS.sealed.first.frame, seal)).status, "opened");
});

test("replay state persists across a restart", async () => {
  const { seal, storage, time } = await bobWithTrust();
  assert.equal((await unsealFrame(VECTORS.sealed.first.frame, seal)).status, "opened");
  const revived = createRoomSealStore({ storage, acct: "bob", node: NODES.bob, now: time.now, wrappingKey: null });
  const again = await revived.resolve(SCOPE, ROOM, "", DM);
  assert.equal((await unsealFrame(VECTORS.sealed.first.frame, again)).status, "replay");
  const history = await unsealFrame(VECTORS.sealed.first.frame, again, ROOM, { history: true });
  assert.equal(history.status, "opened");
  assert.equal(history.replayed, true);
});

test("a blocked sender is held, not opened", async () => {
  const { seal } = await bobWithTrust();
  await seal.block(VECTORS.devices.alice.kid);
  const result = await unsealFrame(VECTORS.sealed.first.frame, seal);
  assert.equal(result.status, "held");
  assert.equal(result.blocked, true);
});

test("a wiped prekey cannot open", async () => {
  const { seal, store, time } = await bobWithTrust();
  time.value = VECTORS.afterBobWipesOldPrekey.now;
  await store.wipeExpired();
  time.value = NOW;
  assert.equal((await unsealFrame(VECTORS.sealed.first.frame, seal)).status, VECTORS.afterBobWipesOldPrekey.expect);
});

test("another room cannot open the frame", async () => {
  const { store } = await bobWithTrust();
  const other = await store.resolve(SCOPE, "another-room", "", DM);
  assert.equal((await unsealFrame(VECTORS.sealed.first.frame, other)).status, "invalid");
});

test("KEK and ML-KEM agree with the vector", async () => {
  const k = VECTORS.kek;
  const ct = b64ToBytes(k.ct);
  const spk = VECTORS.devices.bob.prekeys.find((entry) => entry.id === k.spkId);
  const pair = mlkemKeyPair(b64ToBytes(spk.pqSeed));
  assert.equal(bytesToB64Url(pair.publicKey), spk.pq);
  assert.equal(bytesToB64(mlkemDecapsulate(pair.secretKey, ct)), k.ss2);
  assert.equal(k.ss2, k.ss2FromDecaps);
  assert.equal(bytesToB64(await x25519Shared(b64ToBytes(k.ephPriv), b64UrlToBytes(spk.x))), k.ss1);
  assert.equal(bytesToB64(await x25519Shared(b64ToBytes(spk.xPriv), b64UrlToBytes(k.ephPub))), k.ss1);
  const derived = await deriveKek(k.room, k.sender, k.recipient, k.spkId, b64UrlToBytes(k.ephPub), ct,
    b64ToBytes(k.ss1), b64ToBytes(k.ss2));
  assert.equal(bytesToB64(derived), k.kek);
});

test("safety numbers match the vectors in any order", async () => {
  for (const name of ["twoDevices", "threeDevices"]) {
    const s = VECTORS.safety[name];
    const idks = s.idks.map((idk) => b64UrlToBytes(idk));
    const number = await safetyNumber(s.scope, idks);
    assert.equal(number.digits, s.digits);
    assert.equal(number.digits.length, 60);
    assert.equal(number.groups.length, 12);
    assert.equal(number.qr, s.qr);
    assert.equal((await safetyNumber(s.scope, [...idks].reverse())).digits, s.digits);
  }
  assert.notEqual(VECTORS.safety.twoDevices.digits, VECTORS.safety.threeDevices.digits);
});

test("the seal's safety number follows the approved set and reports the change", async () => {
  const { seal, store } = await bobWithTrust();
  const events = [];
  store.subscribe((event) => events.push(event.type));
  const before = await seal.safetyNumber();
  assert.equal(before.digits, VECTORS.safety.twoDevices.digits);
  assert.equal(before.qr, VECTORS.safety.twoDevices.qr);
  assert.equal(before.verified, false);
  await seal.markVerified();
  assert.equal((await seal.safetyNumber()).verified, true);
  await seal.approve(VECTORS.devices.alice2.kid);
  const after = await seal.safetyNumber();
  assert.equal(after.digits, VECTORS.safety.threeDevices.digits);
  assert.equal(after.verified, false);
  assert.ok(events.includes("safety-changed"));
});

test("two web devices seal and open each other's frames", async () => {
  const time = clock();
  const make = (acct, node) => createRoomSealStore({ storage: memoryStorage(), acct, node, now: time.now, wrappingKey: null });
  const aliceStore = make("Alice", "a-1");
  const bobStore = make("bob", "b-1");
  const malloryStore = make("alice", "a-2");
  const alice = await aliceStore.resolve(SCOPE, ROOM, "", DM);
  const bob = await bobStore.resolve(SCOPE, ROOM, "", DM);
  const mallory = await malloryStore.resolve(SCOPE, ROOM, "", DM);
  assert.equal(alice.acct, "alice");
  const outsider = await make("eve", "e-1").resolve(SCOPE, ROOM, "", DM);
  assert.equal(await acceptSealKey(await sealKeyFrame(outsider), bob), null);
  assert.equal((await acceptSealKey(await sealKeyFrame(bob), alice)).state, "approved");
  assert.equal((await acceptSealKey(await sealKeyFrame(alice), bob)).state, "approved");
  const pending = [];
  bobStore.subscribe((event) => { if (event.type === "pending") pending.push(event.kid); });
  assert.equal((await acceptSealKey(await sealKeyFrame(mallory), bob)).state, "pending");
  assert.deepEqual(pending, [mallory.keyId]);
  assert.equal((await acceptSealKey(await sealKeyFrame(bob), mallory)).state, "approved");

  const plain = { type: "chat", id: "r1", senderId: "a-1", text: "round trip" };
  const frame = await sealFrame(plain, alice);
  assert.deepEqual(frame.keys.map((entry) => entry.kid), [alice.keyId, bob.keyId].sort());
  assert.equal(frame.ctr, 1);
  const opened = await unsealFrame(frame, bob);
  assert.equal(opened.status, "opened");
  assert.deepEqual(opened.plain, plain);
  assert.equal(opened.acct, "alice");
  assert.equal(opened.node, "a-1");
  const own = await unsealFrame(frame, alice);
  assert.equal(own.status, "opened");
  assert.equal(own.self, true);
  assert.equal((await unsealFrame(frame, bob)).status, "replay");

  const reply = await sealFrame({ type: "chat", id: "r2", senderId: "b-1", text: "back" }, bob);
  assert.ok(!reply.keys.some((entry) => entry.kid === mallory.keyId));
  assert.equal((await unsealFrame(reply, alice)).plain.text, "back");
  assert.equal((await unsealFrame(reply, mallory)).status, "notForMe");

  const forged = await sealFrame({ type: "chat", id: "r3", senderId: "b-1", text: "not bob" }, alice);
  assert.equal((await unsealFrame(forged, bob)).status, "invalid");
  const fromMallory = await sealFrame({ type: "chat", id: "r4", senderId: "a-2", text: "hi" }, mallory);
  assert.equal((await unsealFrame(fromMallory, bob)).status, "held");
  await bob.block(mallory.keyId);
  assert.equal((await unsealFrame(fromMallory, bob)).status, "held");
  assert.equal(bob.heldFrames().length, 0);

  const next = await sealFrame({ type: "chat", id: "r5", senderId: "a-1", text: "again" }, alice);
  assert.equal(next.ctr, 3);
  const swapped = { ...next, keys: [...next.keys].reverse() };
  assert.equal((await unsealFrame(swapped, bob)).status, "invalid");
});

test("open rooms approve every valid bundle and prekeys rotate after a day", async () => {
  const time = clock();
  const room = "chat-room-0123:v1";
  const aliceStore = createRoomSealStore({ storage: memoryStorage(), acct: "anna", now: time.now, wrappingKey: null });
  const bobStore = createRoomSealStore({ storage: memoryStorage(), acct: "ben", now: time.now, wrappingKey: null });
  const alice = await aliceStore.resolve("private:0123", room);
  const bob = await bobStore.resolve("private:0123", room);
  assert.equal(alice.policy, "open");
  const first = await sealKeyFrame(alice);
  assert.equal((await acceptSealKey(first, bob)).state, "approved");
  const rotation = alice.rotation;
  time.value = NOW + 25 * 60 * 60 * 1000;
  const second = await sealKeyFrame(alice);
  assert.notEqual(second.spk.id, first.spk.id);
  assert.ok(alice.rotation > rotation);
  const refreshed = await acceptSealKey(second, bob);
  assert.equal(refreshed.refreshed, true);
  const frame = await sealFrame({ type: "chat", id: "o1", senderId: bobStore.node, text: "open" }, bob);
  assert.equal(frame.keys.find((entry) => entry.kid === alice.keyId).spk, second.spk.id);
  assert.equal((await unsealFrame(frame, alice)).status, "held");
  const learned = await acceptSealKey(await sealKeyFrame(bob), alice);
  assert.equal(learned.state, "approved");
  assert.equal(learned.released.length, 1);
  assert.equal(learned.released[0].plain.text, "open");
});

test("relay @peer frames are sealed only to the recipient's devices and the sender", async () => {
  const time = clock();
  const room = "relay-room";
  const stores = ["me", "peer", "other"].map((name) =>
    createRoomSealStore({ storage: memoryStorage(), acct: name, now: time.now, wrappingKey: null }));
  const [me, peer, other] = await Promise.all(stores.map((store) => store.resolve("#mesh-room", room)));
  for (const seal of [peer, other]) await acceptSealKey(await sealKeyFrame(seal), me);
  const frame = await sealFrame(
    { type: "hello", id: "d1", senderId: stores[0].node, to: stores[1].node }, me, room, { to: stores[1].node });
  assert.deepEqual(frame.keys.map((entry) => entry.kid), [me.keyId, peer.keyId].sort());
  assert.equal((await unsealFrame(frame, other)).status, "held");
});

test("v2 key material is removed and v2 frames are not v3 frames", async () => {
  const storage = memoryStorage();
  storage.setItem("forkmesh.chat.e2ee.x25519.v2", "{}");
  storage.setItem("forkmesh.chat.e2ee.peers.v2.x.y", "[]");
  createRoomSealStore({ storage, acct: "a", node: "n", wrappingKey: null });
  assert.equal(storage.getItem("forkmesh.chat.e2ee.x25519.v2"), null);
  assert.equal(storage.getItem("forkmesh.chat.e2ee.peers.v2.x.y"), null);
  assert.equal(isSealedFrame({ type: "sealed", v: 2, kid: "SQAm1SbhWWBxNiSt", keys: [{}] }), false);
  assert.equal(isSealKeyFrame({ type: "e2ee-key", v: 2, kid: "SQAm1SbhWWBxNiSt", pub: "x" }), false);
});

test("secrets are wrapped when a wrapping key is available", async () => {
  const storage = memoryStorage();
  const wrap = await globalThis.crypto.subtle.generateKey({ name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"]);
  const store = createRoomSealStore({ storage, acct: "a", node: "n", wrappingKey: async () => wrap });
  const seal = await store.resolve("direct:00", "room");
  const stored = JSON.parse(storage.getItem(PREFIX + "ed25519IdentityV3"));
  assert.equal(stored.wrapped, 1);
  assert.equal(JSON.parse(storage.getItem(PREFIX + "prekeysV3")).wrapped, 1);
  const again = await createRoomSealStore({ storage, acct: "a", node: "n", wrappingKey: async () => wrap }).resolve("direct:00", "room");
  assert.equal(again.keyId, seal.keyId);
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

test("the DM transport holds a new device of a known account until it is approved", async () => {
  const { createChatRoomTransport } = await import("../public/chat-room-transport.js");
  const { decryptObject, derivePassphraseKey, encryptObject } = await import("../public/chat-crypto.js");
  const roomName = "chat-direct:alpha:v1";
  const scope = "direct:0123456789abcdef0123456789abcdef";
  const dm = { participants: ["me", "peer"] };
  const me = createRoomSealStore({ storage: memoryStorage(), acct: "me", node: "node-me", wrappingKey: null });
  const peer = await createRoomSealStore({ storage: memoryStorage(), acct: "peer", node: "node-peer", wrappingKey: null })
    .resolve(scope, roomName, "", dm);
  const peerPhone = await createRoomSealStore({ storage: memoryStorage(), acct: "peer", node: "node-peer-2", wrappingKey: null })
    .resolve(scope, roomName, "", dm);
  const trust = [];
  const live = [];
  const history = [];
  const transport = createChatRoomTransport({
    authorize: async () => ({ scope, passphrase: "room secret", roomName, webSocketUrl: "/ws" }),
    isDirectRoom: () => true,
    resolveSeal: (room, name) => me.resolve(room.scope, name, "", dm),
    onTrust: (event) => trust.push(event.type),
    onPlain: (plain) => live.push(plain),
    onHistory: (plains) => history.push(...plains),
    WebSocketImpl: FakeWebSocket,
    locationLike: { protocol: "https:", host: "relay.example" },
  });
  await transport.connect({ scope });
  const socket = FakeWebSocket.instances.at(-1);
  socket.open();
  const key = await derivePassphraseKey("room secret", roomName);
  const settle = () => new Promise((resolve) => setTimeout(resolve, 60));
  const deliver = async (inner, extra = {}) => {
    socket.emit("message", { data: JSON.stringify({ ...(await encryptObject(inner, key)), ...extra }) });
    await settle();
  };
  socket.emit("message", { data: JSON.stringify({ kind: "forkmesh-history-end", v: 1 }) });
  await settle();
  const myBundle = await Promise.all(socket.sent.map((raw) => decryptObject(JSON.parse(raw), key)))
    .then((frames) => frames.find(isSealKeyFrame));
  assert.ok(myBundle);
  assert.equal(myBundle.acct, "me");
  assert.equal(myBundle.node, "node-me");
  await acceptSealKey(myBundle, peer);
  await acceptSealKey(myBundle, peerPhone);

  await deliver(await sealKeyFrame(peer));
  const hello = await sealFrame({ type: "chat", id: "p1", text: "hi" }, peer);
  await deliver(hello);
  assert.equal(live.at(-1).senderId, "node-peer");
  await deliver(hello);
  assert.equal(live.filter((plain) => plain.id === "p1").length, 1);

  const forgedReaction = await sealFrame(
    { type: "reaction", id: "r1", senderId: "node-peer", reactorId: "node-me", target: "p1", emoji: "x" }, peer);
  await deliver(forgedReaction);
  assert.ok(!live.some((plain) => plain.id === "r1"));

  await deliver(await sealKeyFrame(peerPhone));
  assert.ok(trust.includes("pending"));
  assert.equal(transport.pendingDevices().length, 1);
  const fromPhone = await sealFrame({ type: "chat", id: "p2", senderId: "node-peer-2", text: "new phone" }, peerPhone);
  await deliver(fromPhone);
  assert.ok(!live.some((plain) => plain.id === "p2"));
  const sentBefore = socket.sent.length;
  await transport.send({ type: "chat", id: "m1", senderId: "node-me", text: "only to the approved phone" }, { persist: true });
  const mine = await decryptObject(JSON.parse(socket.sent.at(-1)), key);
  assert.ok(socket.sent.length > sentBefore);
  assert.ok(!mine.keys.some((entry) => entry.kid === peerPhone.keyId));

  const before = await transport.safetyNumber();
  assert.equal(before.digits.length, 60);
  assert.equal(await transport.approveDevice(peerPhone.keyId), true);
  await settle();
  assert.ok(history.some((plain) => plain.id === "p2"));
  const after = await transport.safetyNumber();
  assert.notEqual(after.digits, before.digits);
  const resent = socket.sent.map((raw) => JSON.parse(raw)).filter((envelope) => envelope.resend === true);
  assert.equal(resent.length, 1);
  const resealed = await decryptObject(resent[0], key);
  assert.ok(resealed.keys.some((entry) => entry.kid === peerPhone.keyId));
  assert.equal((await unsealFrame(resealed, peerPhone)).plain.text, "only to the approved phone");

  socket.emit("close", { code: 1000 });
  transport.dispose();
});

test("a reconnect replays history this device has already opened", async () => {
  const { createChatRoomTransport } = await import("../public/chat-room-transport.js");
  const { derivePassphraseKey, encryptObject } = await import("../public/chat-crypto.js");
  const roomName = "chat-channel:beta:v1";
  const storage = memoryStorage();
  const peerStore = createRoomSealStore({ storage: memoryStorage(), acct: "peer", wrappingKey: null });
  const peer = await peerStore.resolve("private:beta", roomName);
  const key = await derivePassphraseKey("room secret", roomName);
  const bundle = await encryptObject(await sealKeyFrame(peer), key);
  const run = async (store, frames) => {
    const history = [];
    const live = [];
    const transport = createChatRoomTransport({
      authorize: async () => ({ scope: "private:beta", passphrase: "room secret", roomName, webSocketUrl: "/ws" }),
      resolveSeal: (room, name) => store.resolve(room.scope, name),
      onHistory: (plains) => history.push(...plains),
      onPlain: (plain) => live.push(plain),
      WebSocketImpl: FakeWebSocket,
      locationLike: { protocol: "https:", host: "relay.example" },
    });
    await transport.connect({ scope: "private:beta" });
    const socket = FakeWebSocket.instances.at(-1);
    socket.open();
    for (const envelope of [bundle, ...frames]) {
      socket.emit("message", { data: JSON.stringify({ ...envelope, historyReplay: true }) });
    }
    socket.emit("message", { data: JSON.stringify({ kind: "forkmesh-history-end", v: 1 }) });
    await new Promise((resolve) => setTimeout(resolve, 80));
    for (const envelope of frames) socket.emit("message", { data: JSON.stringify(envelope) });
    await new Promise((resolve) => setTimeout(resolve, 80));
    transport.dispose();
    return { history, live };
  };
  const me = createRoomSealStore({ storage, acct: "me", wrappingKey: null });
  const myBundle = await sealKeyFrame(await me.resolve("private:beta", roomName));
  await acceptSealKey(myBundle, peer);
  const frame = await encryptObject(await sealFrame({ type: "chat", id: "h1", senderId: peerStore.node, text: "kept" }, peer), key);
  const first = await run(me, [frame]);
  assert.deepEqual(first.history.map((plain) => plain.id), ["h1"]);
  assert.deepEqual(first.live, []);
  const reloaded = createRoomSealStore({ storage, acct: "me", wrappingKey: null });
  const second = await run(reloaded, [frame]);
  assert.deepEqual(second.history.map((plain) => plain.id), ["h1"]);
  assert.deepEqual(second.live, []);
});
