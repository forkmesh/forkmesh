import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import vm from "node:vm";

import { Keccak, mlkemDecapsulate, mlkemEncapsulate, mlkemKeyPair } from "../public/chat-mlkem.js";
import {
  CLOCK_SKEW_MS,
  MAX_PARTICIPANTS,
  PRESENCE_GRACE_MS,
  PRESENCE_TIMEOUT_MS,
  b64uDecode,
  b64uEncode,
  createCallSession,
  deriveFrameKey,
  errorCode,
  kekFrom,
  keyId,
  marshalKeyMessage,
  marshalPresence,
  parseKeyMessage,
  parsePresence,
  presenceMessage,
  keyMessageBytes,
  signerFromSeed,
  unwrapFrameSecret,
  verificationCode,
  verifyKeyMessage,
  verifyPresence,
  wrapFrameSecret,
  wrapInfo,
  x25519FromPrivate,
} from "../public/chat-call-e2ee.js";

const ROOT = new URL("../../", import.meta.url);
const TESTDATA = new URL("server/rtc/e2ee/testdata/", ROOT);
const WORKER = new URL("app/public/chat-group-call-crypto.js", ROOT);
const FIPS = JSON.parse(readFileSync(new URL("fips203-mlkem768.json", TESTDATA), "utf8"));
const VECTOR_CALL = "chat:private-room-demo";

const hex = (text) => Uint8Array.from(Buffer.from(text, "hex"));
const toHex = (bytes) => Buffer.from(bytes).toString("hex");
const sha256Hex = (bytes) => createHash("sha256").update(bytes).digest("hex");
const random = (n) => globalThis.crypto.getRandomValues(new Uint8Array(n));

async function rejects(promise, code) {
  try {
    await promise;
  } catch (error) {
    assert.equal(errorCode(error), code, String(error?.stack || error));
    return;
  }
  assert.fail("expected " + code);
}

test("ML-KEM-768 key generation matches the FIPS 203 ACVP vectors", () => {
  assert.equal(FIPS.keyGen.length, 25);
  for (const tc of FIPS.keyGen) {
    const pair = mlkemKeyPair(new Uint8Array([...hex(tc.d), ...hex(tc.z)]));
    assert.equal(sha256Hex(pair.publicKey), tc.ekSha256.toLowerCase(), "tcId " + tc.tcId);
    assert.equal(sha256Hex(pair.secretKey), tc.dkSha256.toLowerCase(), "tcId " + tc.tcId);
  }
});

test("ML-KEM-768 encapsulation matches the FIPS 203 ACVP vectors", () => {
  assert.equal(FIPS.encapsulation.length, 25);
  for (const tc of FIPS.encapsulation) {
    const out = mlkemEncapsulate(hex(tc.ek), hex(tc.m));
    assert.equal(sha256Hex(out.ciphertext), tc.cSha256.toLowerCase(), "tcId " + tc.tcId);
    assert.equal(toHex(out.sharedSecret), tc.k.toLowerCase(), "tcId " + tc.tcId);
  }
});

test("ML-KEM-768 decapsulation matches the FIPS 203 ACVP vectors, implicit rejection included", () => {
  assert.equal(FIPS.decapsulation.length, 10);
  assert.ok(FIPS.decapsulation.some((tc) => tc.reason === "modified ciphertext"));
  for (const tc of FIPS.decapsulation) {
    assert.equal(toHex(mlkemDecapsulate(hex(tc.dk), hex(tc.c))), tc.k.toLowerCase(), "tcId " + tc.tcId);
  }
});

test("ML-KEM-768 input checks match the FIPS 203 ACVP key checks", () => {
  for (const tc of FIPS.encapsulationKeyCheck) {
    let ok = true;
    try { mlkemEncapsulate(hex(tc.ek)); } catch (_) { ok = false; }
    assert.equal(ok, tc.testPassed, "ek tcId " + tc.tcId);
  }
  for (const tc of FIPS.decapsulationKeyCheck) {
    let ok = true;
    try { mlkemDecapsulate(hex(tc.dk), new Uint8Array(1088)); } catch (_) { ok = false; }
    assert.equal(ok, tc.testPassed, "dk tcId " + tc.tcId);
  }
});

test("ML-KEM-768 matches Go crypto/mlkem's accumulated vectors", () => {
  const runs = Number(process.env.CALL_E2EE_ACCUMULATED || 100);
  const expected = {
    100: "1114b1b6699ed191734fa339376afa7e285c9e6acf6ff0177d346696ce564415",
    10000: "8a518cc63da366322a8e7a818c7a0d63483cb3528d34a4cf42f35d5ad73f22fc",
  }[runs];
  assert.ok(expected, "CALL_E2EE_ACCUMULATED must be 100 or 10000");
  const s = Keccak.shake128();
  const o = Keccak.shake128();
  for (let i = 0; i < runs; i += 1) {
    const pair = mlkemKeyPair(s.squeeze(new Uint8Array(64)));
    o.update(pair.publicKey);
    const enc = mlkemEncapsulate(pair.publicKey, s.squeeze(new Uint8Array(32)));
    o.update(enc.ciphertext);
    o.update(enc.sharedSecret);
    assert.deepEqual(mlkemDecapsulate(pair.secretKey, enc.ciphertext), enc.sharedSecret);
    o.update(mlkemDecapsulate(pair.secretKey, s.squeeze(new Uint8Array(1088))));
  }
  assert.equal(toHex(o.digest(32)), expected);
});

test("WebCrypto provides X25519, Ed25519 and HKDF with RFC vectors", async () => {
  const alice = await x25519FromPrivate(hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"));
  assert.equal(toHex(alice.publicBytes), "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
  const bob = await x25519FromPrivate(hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb"));
  const { kek } = await kekFrom("x", "y", new Uint8Array(32), new Uint8Array(32));
  assert.equal(kek.length, 32);
  const shared = new Uint8Array(await crypto.subtle.deriveBits(
    { name: "X25519", public: await crypto.subtle.importKey("raw", bob.publicBytes, { name: "X25519" }, false, []) },
    alice.privateKey, 256));
  assert.equal(toHex(shared), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
  const signer = await signerFromSeed(hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"));
  assert.equal(toHex(signer.idk), "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  await rejects(signer.sign(new TextEncoder().encode("forkmesh-chat-e2ee-bundle-v3\n")), "invalid");
});

function loadWorker() {
  const context = {
    crypto: globalThis.crypto, TransformStream, ReadableStream, WritableStream,
    Uint8Array, DataView, BigInt, Map, Math, Number, Promise, console,
  };
  context.self = context;
  vm.createContext(context);
  vm.runInContext(readFileSync(WORKER, "utf8"), context);
  return {
    post(message) {
      return context.onmessage({ data: message });
    },
    async run(direction, kind, peer, frames) {
      const out = [];
      const readable = new ReadableStream({
        start(controller) {
          for (const frame of frames) controller.enqueue({ data: frame.data.slice().buffer, type: frame.type });
          controller.close();
        },
      });
      let done;
      const finished = new Promise((resolve) => { done = resolve; });
      const writable = new WritableStream({
        write(frame) { out.push(new Uint8Array(frame.data)); },
        close() { done(); },
      });
      context.onmessage({ data: { op: "stream", direction, kind, peer, readable, writable } });
      await finished;
      return out;
    },
  };
}

function frameKind(clear) {
  if (clear === 1) return { kind: "audio", type: undefined };
  if (clear === 10) return { kind: "video", type: "key" };
  if (clear === 3) return { kind: "video", type: "delta" };
  throw new Error("no codec has " + clear + " clear bytes");
}

function queueRandom(bytes) {
  let offset = 0;
  return (n) => {
    if (offset + n > bytes.length) throw new Error("random queue exhausted");
    const out = bytes.slice(offset, offset + n);
    offset += n;
    return out;
  };
}

async function sessionFor(v, name) {
  const id = v.identities[name];
  const stream = [];
  let epochs = 0;
  for (const p of v.presences) {
    if (p.name !== name) continue;
    stream.push(...b64uDecode(p.x25519), ...b64uDecode(p.mlkemSeed));
    epochs += 1;
  }
  const session = await createCallSession({
    call: v.call, peer: id.peer, acct: id.acct, signer: await signerFromSeed(b64uDecode(id.seed)),
    now: () => v.now, random: queueRandom(Uint8Array.from(stream)),
  });
  for (let i = 1; i < epochs; i += 1) await session.rotate();
  return session;
}

async function checkVectors(v) {
  assert.ok(v.call && Object.keys(v.identities).length >= 3 && v.keys.length && v.codes.length && v.frames.length && v.rejects.length);
  const byPeer = {};
  for (const [name, id] of Object.entries(v.identities)) {
    const signer = await signerFromSeed(b64uDecode(id.seed));
    assert.equal(b64uEncode(signer.idk), id.idk, name);
    byPeer[id.peer] = name;
  }
  const held = {};
  for (const vp of v.presences) {
    const p = await parsePresence(vp.message);
    await verifyPresence(p);
    assert.equal(JSON.stringify(marshalPresence(p)), JSON.stringify(vp.message));
    const x = await x25519FromPrivate(b64uDecode(vp.x25519));
    const pq = mlkemKeyPair(b64uDecode(vp.mlkemSeed));
    assert.deepEqual(x.publicBytes, p.x);
    assert.deepEqual(pq.publicKey, p.pq);
    held[vp.name] = held[vp.name] || {};
    held[vp.name][p.epoch] = { x, pq, p };
  }
  for (const vk of v.keys) {
    const m = await parseKeyMessage(vk.message);
    await verifyKeyMessage(m);
    assert.equal(JSON.stringify(marshalKeyMessage(m)), JSON.stringify(vk.message));
    const eph = await x25519FromPrivate(b64uDecode(vk.ephPrivate));
    assert.deepEqual(eph.publicBytes, m.eph);
    const sender = { peer: m.peer, idk: m.idk, epoch: m.epoch };
    const fk = await deriveFrameKey(v.call, sender, b64uDecode(vk.secret));
    assert.equal(b64uEncode(fk.raw), vk.frameKey);
    assert.equal(fk.keyId, vk.keyId);
    assert.equal(vk.wraps.length, m.keys.length);
    for (let i = 0; i < m.keys.length; i += 1) {
      const w = m.keys[i];
      const tr = vk.wraps[i];
      const sec = held[tr.recipient][w.epoch];
      assert.ok(sec && byPeer[w.peer] === tr.recipient && tr.presenceEpoch === w.epoch);
      const ss1 = new Uint8Array(await crypto.subtle.deriveBits(
        { name: "X25519", public: await crypto.subtle.importKey("raw", m.eph, { name: "X25519" }, false, []) },
        sec.x.privateKey, 256));
      assert.equal(b64uEncode(ss1), tr.ss1);
      const ss2 = mlkemDecapsulate(sec.pq.secretKey, w.ct);
      assert.equal(b64uEncode(ss2), tr.ss2);
      const info = await wrapInfo(v.call, sender, sec.p, m.eph, w.ct);
      assert.equal(info, tr.info);
      const { kek, nonce } = await kekFrom(v.call, info, ss1, ss2);
      assert.equal(b64uEncode(kek), tr.kek);
      assert.equal(b64uEncode(nonce), tr.nonce);
      const secret = await unwrapFrameSecret({
        call: v.call, sender, eph: m.eph, entry: w, recipient: sec.p, xPrivate: sec.x.privateKey, pqSecret: sec.pq.secretKey,
      });
      assert.equal(b64uEncode(secret), vk.secret);
      const session = await sessionFor(v, tr.recipient);
      const senderEpochs = Object.values(held[vk.sender]).map((entry) => entry.p);
      const newest = senderEpochs.reduce((a, b) => (b.epoch > a.epoch ? b : a));
      await session.acceptPresence(marshalPresence(newest));
      const got = await session.openKeys(vk.message);
      assert.equal(b64uEncode(got.raw), vk.frameKey);
      assert.equal(got.peer, m.peer);
      assert.equal(got.epoch, m.epoch);
      await rejects(session.openKeys(vk.message), "duplicate");
    }
  }
  for (const c of v.codes) {
    const members = c.members.map((name) => ({ idk: b64uDecode(v.identities[name].idk), acct: v.identities[name].acct }));
    assert.equal(await verificationCode(v.call, members), c.code, c.members.join(","));
  }
  const worker = loadWorker();
  const key = b64uDecode(v.frames[0].key);
  await worker.post({ op: "key", peer: "p", keyId: v.frames[0].keyId, raw: key.slice().buffer });
  await worker.post({ op: "send-key", keyId: v.frames[0].keyId, raw: key.slice().buffer });
  for (const f of v.frames) {
    assert.equal(f.key, v.frames[0].key);
    const { kind, type } = frameKind(f.clear);
    const opened = await worker.run("decrypt", kind, "p", [{ data: hex(f.sealed), type }]);
    assert.equal(opened.length, 1, "frame did not open in the worker");
    assert.equal(toHex(opened[0]), f.plain);
    const sealed = await worker.run("encrypt", kind, "p", [{ data: hex(f.plain), type }]);
    assert.equal(toHex(sealed[0]), f.sealed);
    assert.equal((await worker.run("decrypt", kind, "p", [{ data: hex(f.sealed), type }])).length, 0, "replay accepted");
  }
  for (const r of v.rejects) {
    const session = await sessionFor(v, r.as);
    for (const before of r.before || []) await session.acceptPresence(before);
    const work = r.what === "presence" ? session.acceptPresence(r.message) : session.openKeys(r.message);
    await rejects(work, r.error);
  }
}

test("JS opens the vectors Go wrote", async () => {
  const path = process.env.CALL_E2EE_GO_VECTORS || new URL("go-vectors.json", TESTDATA);
  const v = JSON.parse(readFileSync(path, "utf8"));
  assert.equal(v.generator, "go");
  await checkVectors(v);
});

async function signed(signer, object, messageOf, marshal) {
  object.sig = await signer.sign(await messageOf(object));
  return marshal(object);
}

async function generateVectors() {
  const now = Date.now();
  const v = { generator: "js", call: VECTOR_CALL, now, identities: {}, presences: [], keys: [], codes: [], frames: [], rejects: [] };
  const ids = {};
  for (const [name, acct, peer] of [
    ["alice", "alice", "0a1b2c3d4e5f60718293a4b5c6d7e8f9"],
    ["bob", "bob", "1f2e3d4c5b6a79880716253443526170"],
    ["carol", "carol ü", "2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c2c"],
  ]) {
    const seed = random(32);
    const signer = await signerFromSeed(seed);
    ids[name] = { name, seed, signer, acct, peer, kid: await keyId(signer.idk) };
    v.identities[name] = { seed: b64uEncode(seed), idk: b64uEncode(signer.idk), acct, peer };
  }
  const pres = {};
  for (const [name, epoch] of [["alice", 1], ["bob", 1], ["carol", 1], ["carol", 2]]) {
    const who = ids[name];
    const xs = random(32);
    const seed = random(64);
    const x = await x25519FromPrivate(xs);
    const pq = mlkemKeyPair(seed);
    const p = { call: VECTOR_CALL, peer: who.peer, acct: who.acct, kid: who.kid, idk: who.signer.idk, epoch, ts: now, x: x.publicBytes, pq: pq.publicKey, sig: null };
    const message = await signed(who.signer, p, presenceMessage, marshalPresence);
    (pres[name] = pres[name] || []).push(p);
    v.presences.push({ name, epoch, x25519: b64uEncode(xs), mlkemSeed: b64uEncode(seed), message });
  }
  const latest = (name) => pres[name][pres[name].length - 1];
  const nameOf = (peer) => Object.values(ids).find((id) => id.peer === peer).name;
  const parsedKeys = [];
  for (const [sender, epoch, to] of [
    ["alice", 1, [latest("bob"), latest("carol")]],
    ["bob", 1, [latest("alice"), pres.carol[0]]],
    ["alice", 257, [latest("bob")]],
  ]) {
    const who = ids[sender];
    const secret = random(32);
    const ephPrivate = random(32);
    const eph = await x25519FromPrivate(ephPrivate);
    const s = { peer: who.peer, idk: who.signer.idk, epoch };
    const m = { call: VECTOR_CALL, peer: who.peer, kid: who.kid, idk: who.signer.idk, epoch, ts: now, eph: eph.publicBytes, keys: [], sig: null };
    const wraps = [];
    for (const recipient of [...to].sort((a, b) => (a.peer < b.peer ? -1 : 1))) {
      const { entry, trace } = await wrapFrameSecret({ call: VECTOR_CALL, sender: s, recipient, eph, secret, mlkemMessage: random(32) });
      m.keys.push(entry);
      wraps.push({
        recipient: nameOf(recipient.peer), presenceEpoch: recipient.epoch, ss1: b64uEncode(trace.ss1), ss2: b64uEncode(trace.ss2),
        kek: b64uEncode(trace.kek), nonce: b64uEncode(trace.nonce), info: trace.info,
      });
    }
    const message = await signed(who.signer, m, keyMessageBytes, marshalKeyMessage);
    const fk = await deriveFrameKey(VECTOR_CALL, s, secret);
    parsedKeys.push(m);
    v.keys.push({ sender, secret: b64uEncode(secret), frameKey: b64uEncode(fk.raw), keyId: fk.keyId, ephPrivate: b64uEncode(ephPrivate), message, wraps });
  }
  for (const set of [["alice", "bob", "carol"], ["alice", "bob"], ["bob", "carol"], ["carol", "alice", "bob"]]) {
    const code = await verificationCode(VECTOR_CALL, set.map((name) => ({ idk: ids[name].signer.idk, acct: ids[name].acct })));
    v.codes.push({ members: set, code });
  }
  const worker = loadWorker();
  const frameKey = b64uDecode(v.keys[0].frameKey);
  await worker.post({ op: "send-key", keyId: v.keys[0].keyId, raw: frameKey.slice().buffer });
  for (const [clear, size] of [[1, 60], [10, 400], [3, 120], [1, 1]]) {
    const { kind, type } = frameKind(clear);
    const plain = random(size);
    const [sealed] = await worker.run("encrypt", kind, "p", [{ data: plain, type }]);
    v.frames.push({ key: v.keys[0].frameKey, keyId: v.keys[0].keyId, clear, plain: toHex(plain), sealed: toHex(sealed) });
  }
  v.rejects = await generateRejects(now, ids, pres, parsedKeys[0]);
  return v;
}

function cloneP(p) {
  return { ...p, x: p.x.slice(), pq: p.pq.slice(), sig: p.sig.slice() };
}

function cloneM(m) {
  return { ...m, keys: m.keys.map((k) => ({ ...k, ct: k.ct.slice(), key: k.key.slice() })), sig: m.sig.slice() };
}

async function generateRejects(now, ids, pres, first) {
  const { alice, bob, carol } = ids;
  const ap = pres.alice[0];
  const bp = pres.bob[0];
  const out = [];
  const add = (what, name, as, before, message, error) => out.push({ what, case: name, as, before, message, error });
  const signP = (p, who) => signed(who.signer, p, presenceMessage, marshalPresence);
  const signM = (m, who) => signed(who.signer, m, keyMessageBytes, marshalKeyMessage);
  const apJSON = marshalPresence(ap);
  let p = cloneP(ap);
  p.sig[5] ^= 1;
  add("presence", "bad-signature", "bob", null, marshalPresence(p), "signature");
  p = cloneP(ap);
  p.call = "chat:another-room";
  add("presence", "wrong-call", "bob", null, await signP(p, alice), "call");
  const { pq: _drop, ...classical } = apJSON;
  add("presence", "classical-only", "bob", null, classical, "invalid");
  add("presence", "short-pq", "bob", null, { ...apJSON, pq: b64uEncode(ap.pq.subarray(0, 1183)) }, "invalid");
  add("presence", "kid-mismatch", "bob", null, { ...apJSON, kid: bob.kid }, "invalid");
  add("presence", "quoted-epoch", "bob", null, { ...apJSON, epoch: "1" }, "invalid");
  add("presence", "version-2", "bob", null, { ...apJSON, v: 2 }, "invalid");
  p = cloneP(ap);
  p.ts = now - 6 * 60 * 1000;
  add("presence", "old-timestamp", "bob", null, await signP(p, alice), "clock");
  const newer = cloneP(ap);
  newer.epoch = 2;
  add("presence", "stale-epoch", "bob", [await signP(newer, alice)], apJSON, "stale");
  p = cloneP(ap);
  p.x = bp.x.slice();
  add("presence", "equivocation", "bob", [apJSON], await signP(p, alice), "equivocation");
  p = cloneP(bp);
  p.peer = alice.peer;
  add("presence", "peer-taken", "carol", [apJSON], await signP(p, bob), "peer-bound");
  p = cloneP(ap);
  p.peer = bob.peer;
  add("presence", "own-peer", "bob", null, await signP(p, alice), "peer-bound");

  let m = cloneM(first);
  m.sig[0] ^= 1;
  add("keys", "bad-signature", "bob", [apJSON], marshalKeyMessage(m), "signature");
  add("keys", "unknown-sender", "bob", null, marshalKeyMessage(first), "unknown-peer");
  m = cloneM(first);
  m.keys = m.keys.slice(1);
  add("keys", "not-for-me", "bob", [apJSON], await signM(m, alice), "not-for-me");
  m = cloneM(first);
  m.keys[0].key[3] ^= 1;
  add("keys", "tampered-wrap", "bob", [apJSON], await signM(m, alice), "invalid");
  m = cloneM(first);
  m.keys[0].ct = m.keys[1].ct.slice();
  add("keys", "swapped-ciphertext", "bob", [apJSON], await signM(m, alice), "invalid");
  m = cloneM(first);
  m.keys[0].epoch = 9;
  add("keys", "retired-recipient-epoch", "bob", [apJSON], await signM(m, alice), "recipient-epoch");
  m = cloneM(first);
  m.keys.reverse();
  add("keys", "unsorted", "bob", [apJSON], await signM(m, alice), "invalid");
  m = cloneM(first);
  m.keys[1].peer = alice.peer;
  add("keys", "sender-as-recipient", "bob", [apJSON], await signM(m, alice), "invalid");
  m = cloneM(first);
  m.idk = carol.signer.idk;
  m.kid = carol.kid;
  add("keys", "other-identity", "bob", [apJSON], await signM(m, carol), "peer-bound");
  m = cloneM(first);
  m.peer = bob.peer;
  add("keys", "own-peer", "bob", [apJSON], await signM(m, alice), "invalid");
  return out.map((r) => ({ ...r, before: r.before || [] }));
}

test("JS writes vectors that it can check itself", async () => {
  const v = await generateVectors();
  await checkVectors(v);
  if (process.env.CALL_E2EE_WRITE_JS_VECTORS) {
    writeFileSync(process.env.CALL_E2EE_WRITE_JS_VECTORS, JSON.stringify(v, null, 1) + "\n");
  }
});

function clockAt(start = 1_790_000_000_000) {
  const state = { t: start };
  state.now = () => state.t;
  return state;
}

let peerCounter = 0;

async function participant(clock, call, acct) {
  peerCounter += 1;
  return createCallSession({
    call, peer: peerCounter.toString(16).padStart(32, "0"), acct, signer: await signerFromSeed(random(32)), now: clock.now,
  });
}

async function meet(...all) {
  for (const a of all) for (const b of all) if (a !== b) await a.acceptPresence(await b.presence());
}

async function distribute(from, ...to) {
  const { message, key } = await from.rekey();
  for (const r of to) {
    const got = await r.openKeys(message);
    assert.deepEqual(got.raw, key.raw);
    assert.equal(got.keyId, key.keyId);
    assert.equal(got.peer, from.peer);
  }
  return key;
}

test("three participants agree on every sender's key and on the code", async () => {
  const clock = clockAt();
  const [a, b, d] = [await participant(clock, "call-1", "alice"), await participant(clock, "call-1", "bob"), await participant(clock, "call-1", "dave")];
  await meet(a, b, d);
  const ka = await distribute(a, b, d);
  const kb = await distribute(b, a, d);
  assert.notDeepEqual(ka.raw, kb.raw);
  const code = await a.code();
  assert.match(code, /^\d{5}( \d{5}){5}$/);
  assert.equal(await b.code(), code);
  assert.equal(await d.code(), code);
  a.leave(d.peer);
  assert.notEqual(await a.code(), code);
  const { message } = await a.rekey();
  await rejects(d.openKeys(message), "not-for-me");
  await b.openKeys(message);
});

test("a split view gives different codes", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  await a.acceptPresence(await (await participant(clock, "call-1", "bob")).presence());
  await b.acceptPresence(await (await participant(clock, "call-1", "alice")).presence());
  assert.notEqual(await a.code(), await b.code());
});

test("a joiner cannot open earlier epochs and a rotated key keeps the grace", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  await meet(a, b);
  const old = (await a.rekey()).message;
  const d = await participant(clock, "call-1", "dave");
  await meet(a, b, d);
  await rejects(d.openKeys(old), "not-for-me");
  const late = (await a.rekey()).message;
  assert.equal((await a.acceptPresence(await b.rotate())).kind, "updated");
  await b.openKeys(late);
  const next = (await a.rekey()).message;
  clock.t += PRESENCE_GRACE_MS + 1000;
  b.prune();
  await b.openKeys(next);
  const stale = await b.presence();
  await b.rotate();
  await b.rotate();
  const e = await participant(clock, "call-1", "erin");
  await e.acceptPresence(stale);
  await b.acceptPresence(await e.presence());
  await rejects(b.openKeys((await e.rekey()).message), "recipient-epoch");
});

test("presence replay, stolen peer ids, clocks and calls are refused", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  const first = await a.presence();
  assert.equal((await b.acceptPresence(first)).kind, "joined");
  clock.t += 20000;
  assert.equal((await b.acceptPresence(await a.presence())).kind, "same");
  await rejects(b.acceptPresence(first), "stale");
  await b.acceptPresence(await a.rotate());
  await rejects(b.acceptPresence(first), "stale");
  const mallory = await participant(clock, "call-1", "mallory");
  await rejects(b.acceptPresence({ ...(await mallory.presence()), peer: a.peer }), "peer-bound");
  await rejects(b.acceptPresence(await b.presence()), "own");
  clock.t += CLOCK_SKEW_MS + 60000;
  const later = await participant(clock, "call-1", "zed");
  await rejects(later.acceptPresence(first), "clock");
  await rejects((await participant(clock, "call-2", "zed")).acceptPresence(await a.presence()), "call");
  clock.t += PRESENCE_TIMEOUT_MS + 1000;
  assert.deepEqual(b.expire(), [a.peer]);
});

test("key messages are monotonic per sender", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  await meet(a, b);
  const one = (await a.rekey()).message;
  const two = (await a.rekey()).message;
  await b.openKeys(two);
  await rejects(b.openKeys(one), "stale");
  await rejects(b.openKeys(two), "duplicate");
  await rejects(a.openKeys(two), "own");
});

test("a call holds sixteen people and refuses the seventeenth", async () => {
  const clock = clockAt();
  const host = await participant(clock, "call-1", "host");
  for (let i = 0; i < MAX_PARTICIPANTS - 1; i += 1) {
    await host.acceptPresence(await (await participant(clock, "call-1", "p" + i)).presence());
  }
  await rejects(host.acceptPresence(await (await participant(clock, "call-1", "extra")).presence()), "full");
  const { message } = await host.rekey();
  assert.equal(message.keys.length, MAX_PARTICIPANTS - 1);
  assert.ok(JSON.stringify(message).length < 32 * 1024);
});

test("interleaved calls on one session stay consistent", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  await meet(a, b);
  const rekeys = await Promise.all([a.rekey(), a.rekey(), a.rekey(), a.presence(), a.rotate()]);
  const epochs = rekeys.slice(0, 3).map((r) => r.key.epoch);
  assert.deepEqual(epochs, [1, 2, 3]);
  await b.acceptPresence(rekeys[4]);
  const { message } = await a.rekey();
  await b.openKeys(message);
});

test("strict parsing refuses lenient base64 and odd shapes", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const p = await a.presence();
  await parsePresence(p);
  await rejects(parsePresence({ ...p, idk: p.idk + "=" }), "invalid");
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  const loose = p.x.slice(0, -1) + alphabet[alphabet.indexOf(p.x.slice(-1)) | 1];
  await rejects(parsePresence({ ...p, x: loose }), "invalid");
  await rejects(parsePresence({ ...p, epoch: 1.5 }), "invalid");
  await rejects(parsePresence({ ...p, epoch: 2 ** 32 }), "invalid");
  await rejects(parsePresence({ ...p, acct: "a\u0000b" }), "invalid");
  await rejects(parsePresence({ ...p, peer: "a|b" }), "invalid");
  await rejects(parsePresence([p]), "invalid");
  await rejects(parsePresence("{"), "invalid");
  const parsed = await parsePresence(JSON.stringify(p));
  assert.equal(parsed.peer, a.peer);
});
