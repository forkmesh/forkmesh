import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import vm from "node:vm";

import { Keccak, mlkemDecapsulate, mlkemEncapsulate, mlkemKeyPair } from "../public/chat-mlkem.js";
import {
  CLOCK_SKEW_MS,
  CallE2eeError,
  AUTH_INTERVAL_MS,
  EMOJI_TABLE,
  MAX_AUTH_FRAMES,
  MAX_GEN_JUMP,
  MAX_PARTICIPANTS,
  PRESENCE_GRACE_MS,
  PRESENCE_TIMEOUT_MS,
  REVEAL_TIMEOUT_MS,
  ROUND_TIMEOUT_MS,
  b64uDecode,
  b64uEncode,
  commitHash,
  commitMessage,
  commitOpens,
  computeSas,
  createCallSession,
  createMediaSigner,
  createMediaVerifier,
  deriveFrameKey,
  errorCode,
  kekFrom,
  keyId,
  keyMessageBytes,
  marshalCommit,
  marshalKeyMessage,
  marshalPresence,
  marshalReveal,
  parseAuthMessage,
  parseCommit,
  parseKeyMessage,
  parsePresence,
  parseQr,
  parseReveal,
  presenceMessage,
  roundId,
  sasFromTranscript,
  signAuthMessage,
  signerFromSeed,
  unwrapFrameSecret,
  verifyCommit,
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

async function sessionFor(v, name, ...extra) {
  const id = v.identities[name];
  const stream = [];
  let epochs = 0;
  for (const p of v.presences) {
    if (p.name !== name) continue;
    stream.push(...b64uDecode(p.x25519), ...b64uDecode(p.mlkemSeed));
    epochs += 1;
  }
  for (const bytes of extra) stream.push(...bytes);
  const session = await createCallSession({
    call: v.call, peer: id.peer, acct: id.acct, signer: await signerFromSeed(b64uDecode(id.seed)),
    now: () => v.now, random: queueRandom(Uint8Array.from(stream)),
  });
  for (let i = 1; i < epochs; i += 1) await session.rotate();
  return session;
}

function membersOf(v, names, nonces) {
  return names.map((name, i) => {
    const id = v.identities[name];
    return { idk: b64uDecode(id.idk), acct: id.acct, peer: id.peer, nonce: nonces ? b64uDecode(nonces[i]) : null };
  });
}

async function checkSas(v, c) {
  const members = membersOf(v, c.members, c.nonces);
  assert.equal(b64uEncode(await roundId(v.call, c.gen, members)), c.round, c.members.join(","));
  const code = await computeSas(v.call, c.gen, members);
  assert.equal(b64uEncode(code.transcript), c.transcript);
  assert.deepEqual(code.emoji, c.emoji);
  assert.equal(code.emojiText, c.emojiText);
  assert.equal(code.digits, c.digits);
  assert.equal(code.qr, c.qr);
  assert.equal(b64uEncode(parseQr(c.qr)), c.transcript);
}

async function checkRound(v) {
  const r = v.round;
  await checkSas(v, r.sas);
  assert.equal(r.commits.length, r.sas.members.length);
  for (let i = 0; i < r.commits.length; i += 1) {
    const vc = r.commits[i];
    const c = await parseCommit(vc.commit);
    await verifyCommit(c);
    assert.equal(JSON.stringify(marshalCommit(c)), JSON.stringify(vc.commit));
    const rv = await parseReveal(vc.reveal);
    assert.equal(JSON.stringify(marshalReveal(rv)), JSON.stringify(vc.reveal));
    assert.equal(b64uEncode(c.round), r.sas.round);
    assert.equal(c.gen, r.sas.gen);
    assert.equal(b64uEncode(rv.nonce), r.sas.nonces[i]);
    assert.ok(await commitOpens(c, rv.nonce), vc.name);
    assert.ok(!await commitOpens(c, b64uDecode(r.sas.round)));
  }
  const session = await sessionFor(v, r.commits[0].name, b64uDecode(r.sas.nonces[0]));
  for (const name of r.sas.members.slice(1)) {
    const newest = v.presences.filter((p) => p.name === name).reduce((a, b) => (b.epoch > a.epoch ? b : a));
    await session.acceptPresence(newest.message);
  }
  assert.equal(JSON.stringify(await session.commit()), JSON.stringify(r.commits[0].commit));
  for (const vc of r.commits.slice(1)) await session.acceptCommit(vc.commit);
  assert.equal(JSON.stringify(await session.reveal()), JSON.stringify(r.commits[0].reveal));
  for (const vc of r.commits.slice(1)) await session.acceptReveal(vc.reveal);
  const st = await session.verification();
  assert.ok(st.code);
  assert.equal(b64uEncode(st.code.transcript), r.sas.transcript);
  assert.equal(st.code.digits, r.sas.digits);
}

async function checkMedia(v) {
  const md = v.media;
  const vk = v.keys[md.key];
  const m = await parseKeyMessage(vk.message);
  const key = { call: v.call, peer: m.peer, epoch: m.epoch, keyId: vk.keyId, auth: m.auth };
  const frames = md.frames.map(hex);
  assert.equal(md.auth.length, 1);
  assert.ok(frames.length >= 3);
  const signer = await createMediaSigner({ ...key, authSeed: b64uDecode(vk.authSeed) });
  assert.deepEqual(signer.auth, m.auth);
  for (const f of frames) await signer.add(f);
  const produced = await signer.flush();
  assert.equal(produced.length, 1);
  assert.equal(toHex(produced[0]), md.auth[0]);
  const t = v.now;
  const verifier = createMediaVerifier({ now: () => t });
  verifier.setKey(key);
  for (const f of frames) assert.equal(await verifier.frame(key.peer, f), "");
  await verifier.accept(key.peer, hex(md.auth[0]));
  const st = verifier.status(key.peer);
  assert.equal(st.state, "verified");
  assert.equal(st.verified, frames.length);
  assert.equal(st.forged, 0);
  for (const rj of md.rejects) await rejects(verifier.accept(key.peer, hex(rj.message)), rj.error);
  const forged = createMediaVerifier({ now: () => t });
  forged.setKey(key);
  assert.equal(await forged.frame(key.peer, hex(md.forged)), "");
  await forged.accept(key.peer, hex(md.auth[0]));
  const fs = forged.status(key.peer);
  assert.equal(fs.state, "forged");
  assert.equal(fs.forged, 1);
  assert.ok(fs.dropping);
  assert.equal(await forged.frame(key.peer, frames[0]), "forged");
}

async function checkVectors(v) {
  assert.ok(v.call && Object.keys(v.identities).length >= 3 && v.keys.length && v.sas.length && v.round.commits.length &&
    v.frames.length && v.media.frames.length && v.rejects.length);
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
    assert.deepEqual((await signerFromSeed(b64uDecode(vk.authSeed))).idk, m.auth);
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
      assert.equal(got.call, v.call);
      assert.deepEqual(got.auth, m.auth);
      await rejects(session.openKeys(vk.message), "duplicate");
    }
  }
  await checkRound(v);
  for (const c of v.sas) await checkSas(v, c);
  assert.equal(v.emoji.length, EMOJI_TABLE.length);
  v.emoji.forEach((e, i) => assert.deepEqual({ text: e.text, name: e.name }, { ...EMOJI_TABLE[i] }, "emoji " + i));
  await checkMedia(v);
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
  const kinds = new Set();
  for (const r of v.rejects) {
    kinds.add(r.what);
    const session = await sessionFor(v, r.as);
    for (const before of r.before || []) await session.acceptPresence(before);
    for (const before of r.beforeCommits || []) await session.acceptCommit(before);
    const work = {
      presence: () => session.acceptPresence(r.message),
      keys: () => session.openKeys(r.message),
      commit: () => session.acceptCommit(r.message),
      reveal: () => session.acceptReveal(r.message),
    }[r.what];
    assert.ok(work, "unknown reject kind " + r.what);
    await rejects(work(), r.error);
  }
  assert.equal(kinds.size, 4);
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

async function makeSas(ids, names, gen, nonces) {
  const list = nonces || names.map(() => random(32));
  const members = names.map((name, i) => ({ idk: ids[name].signer.idk, acct: ids[name].acct, peer: ids[name].peer, nonce: list[i] }));
  const code = await computeSas(VECTOR_CALL, gen, members);
  return {
    members: names, gen, nonces: list.map(b64uEncode), round: b64uEncode(code.round), transcript: b64uEncode(code.transcript),
    emoji: code.emoji, emojiText: code.emojiText, digits: code.digits, qr: code.qr,
  };
}

async function generateRound(now, ids) {
  const names = ["alice", "bob", "carol"];
  const nonces = names.map(() => random(32));
  const sas = await makeSas(ids, names, 1, nonces);
  const round = b64uDecode(sas.round);
  const commits = [];
  for (let i = 0; i < names.length; i += 1) {
    const who = ids[names[i]];
    const c = { call: VECTOR_CALL, peer: who.peer, kid: who.kid, idk: who.signer.idk, round, gen: 1, ts: now, c: null, sig: null };
    c.c = await commitHash(c.call, c.peer, c.idk, c.round, c.gen, nonces[i]);
    commits.push({
      name: names[i],
      commit: await signed(who.signer, c, commitMessage, marshalCommit),
      reveal: marshalReveal({ call: VECTOR_CALL, peer: who.peer, round, gen: 1, nonce: nonces[i] }),
    });
  }
  return { sas, commits };
}

async function generateMedia(v, ids) {
  const vk = v.keys[0];
  const raw = b64uDecode(vk.frameKey);
  const media = { sender: "alice", key: 0, frames: v.frames.map((f) => f.sealed), auth: [], forged: "", rejects: [] };
  const key = { call: VECTOR_CALL, peer: ids.alice.peer, epoch: 1, keyId: vk.keyId };
  const signer = await createMediaSigner({ ...key, authSeed: b64uDecode(vk.authSeed) });
  for (const f of media.frames) await signer.add(hex(f));
  media.auth = (await signer.flush()).map(toHex);
  const forger = loadWorker();
  await forger.post({ op: "send-key", keyId: vk.keyId, raw: raw.slice().buffer });
  const out = await forger.run("encrypt", "audio", "p", [{ data: utf8("\x01not alice") }, { data: utf8("\x01not alice either") }]);
  media.forged = toHex(out[1]);
  const good = hex(media.auth[0]);
  const m = parseAuthMessage(good);
  const add = (name, bytes, error) => media.rejects.push({ case: name, message: toHex(bytes), error });
  const flip = (index) => {
    const b = good.slice();
    b[index] ^= 1;
    return b;
  };
  const resign = async (overrides, peer, seed) => {
    const copy = { ...m, digests: m.digests.slice(), ...overrides };
    return signAuthMessage(VECTOR_CALL, peer, await signerFromSeed(seed), copy);
  };
  const seed = b64uDecode(vk.authSeed);
  add("bad-signature", flip(good.length - 3), "signature");
  add("tampered-digest", flip(15), "signature");
  add("signed-for-another-peer", await resign({}, ids.bob.peer, seed), "signature");
  add("other-auth-key", await resign({}, ids.alice.peer, new Uint8Array(32).fill(3)), "signature");
  add("truncated", good.subarray(0, good.length - 1), "invalid");
  const v1 = good.slice();
  v1[0] = (1 << 4) | 1;
  add("version-1", v1, "invalid");
  const zero = good.slice();
  zero[14] = 0;
  add("zero-count", zero, "invalid");
  add("key-id-not-epoch", flip(1), "invalid");
  add("unknown-key", await resign({ epoch: 2, keyId: 2 }, ids.alice.peer, seed), "unknown-peer");
  add("other-epoch-same-key-id", await resign({ epoch: 257 }, ids.alice.peer, seed), "stale");
  add("duplicate", good, "duplicate");
  return media;
}

async function generateVectors() {
  const now = Date.now();
  const v = {
    generator: "js", call: VECTOR_CALL, now, identities: {}, presences: [], keys: [], round: null, sas: [], frames: [], media: null,
    emoji: EMOJI_TABLE.map((e) => ({ text: e.text, name: e.name })), rejects: [],
  };
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
    const authSeed = random(32);
    const eph = await x25519FromPrivate(ephPrivate);
    const s = { peer: who.peer, idk: who.signer.idk, epoch };
    const m = {
      call: VECTOR_CALL, peer: who.peer, kid: who.kid, idk: who.signer.idk, epoch, ts: now, eph: eph.publicBytes,
      auth: (await signerFromSeed(authSeed)).idk, keys: [], sig: null,
    };
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
    v.keys.push({
      sender, secret: b64uEncode(secret), frameKey: b64uEncode(fk.raw), keyId: fk.keyId, ephPrivate: b64uEncode(ephPrivate),
      authSeed: b64uEncode(authSeed), message, wraps,
    });
  }
  v.round = await generateRound(now, ids);
  for (const [names, gen] of [[["alice", "bob"], 2], [["carol", "bob"], 7], [["bob"], 1]]) v.sas.push(await makeSas(ids, names, gen));
  const worker = loadWorker();
  const frameKey = b64uDecode(v.keys[0].frameKey);
  await worker.post({ op: "send-key", keyId: v.keys[0].keyId, raw: frameKey.slice().buffer });
  for (const [clear, size] of [[1, 60], [10, 400], [3, 120], [1, 1]]) {
    const { kind, type } = frameKind(clear);
    const plain = random(size);
    const [sealed] = await worker.run("encrypt", kind, "p", [{ data: plain, type }]);
    v.frames.push({ key: v.keys[0].frameKey, keyId: v.keys[0].keyId, clear, plain: toHex(plain), sealed: toHex(sealed) });
  }
  v.media = await generateMedia(v, ids);
  v.rejects = await generateRejects(now, ids, pres, parsedKeys[0]);
  return v;
}

const utf8 = (text) => new TextEncoder().encode(text);

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
  const add = (what, name, as, before, message, error, beforeCommits = []) => out.push({ what, case: name, as, before: before || [], beforeCommits, message, error });
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
  add("presence", "version-1", "bob", null, { ...apJSON, v: 1 }, "invalid");
  add("presence", "version-3", "bob", null, { ...apJSON, v: 3 }, "invalid");
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
  const firstJSON = marshalKeyMessage(first);
  const { ak: _ak, ...noAuth } = firstJSON;
  add("keys", "no-auth-key", "bob", [apJSON], noAuth, "invalid");
  add("keys", "version-1", "bob", [apJSON], { ...firstJSON, v: 1 }, "invalid");
  add("keys", "auth-key-unsigned", "bob", [apJSON], { ...firstJSON, ak: b64uEncode(bp.idk) }, "signature");

  const round = new Uint8Array(32).fill(0x42);
  const nonce = new Uint8Array(32).fill(0x24);
  const base = { call: VECTOR_CALL, peer: alice.peer, kid: alice.kid, idk: alice.signer.idk, round, gen: 3, ts: now, c: null, sig: null };
  base.c = await commitHash(base.call, base.peer, base.idk, base.round, base.gen, nonce);
  const signC = (c, who) => signed(who.signer, c, commitMessage, marshalCommit);
  const good = await signC({ ...base }, alice);
  const pa = [apJSON];
  const bad = { ...good, sig: b64uEncode((() => { const s = b64uDecode(good.sig); s[9] ^= 1; return s; })()) };
  add("commit", "bad-signature", "bob", pa, bad, "signature");
  add("commit", "unknown-sender", "bob", null, good, "unknown-peer");
  add("commit", "wrong-call", "bob", pa, await signC({ ...base, call: "chat:another-room" }, alice), "call");
  add("commit", "version-1", "bob", pa, { ...good, v: 1 }, "invalid");
  add("commit", "reveal-type", "bob", pa, { ...good, t: "reveal" }, "invalid");
  add("commit", "quoted-gen", "bob", pa, { ...good, gen: "3" }, "invalid");
  add("commit", "short-round", "bob", pa, { ...good, round: b64uEncode(round.subarray(0, 31)) }, "invalid");
  add("commit", "old-timestamp", "bob", pa, await signC({ ...base, ts: now - 6 * 60 * 1000 }, alice), "clock");
  add("commit", "other-identity", "bob", pa, await signC({ ...base, idk: carol.signer.idk, kid: carol.kid }, carol), "peer-bound");
  const otherC = await commitHash(base.call, base.peer, base.idk, base.round, base.gen, round);
  add("commit", "equivocation", "bob", pa, await signC({ ...base, c: otherC }, alice), "equivocation", [good]);
  add("commit", "duplicate", "bob", pa, good, "duplicate", [good]);
  add("commit", "older-gen", "bob", pa, await signC({ ...base, gen: 2 }, alice), "stale", [good]);
  add("commit", "gen-jump", "bob", pa, await signC({ ...base, gen: MAX_GEN_JUMP + 1 }, alice), "invalid");
  const rv = { call: VECTOR_CALL, peer: alice.peer, round, gen: 3, nonce };
  add("reveal", "unknown-sender", "bob", null, marshalReveal(rv), "unknown-peer");
  add("reveal", "before-commit", "bob", pa, marshalReveal(rv), "waiting");
  add("reveal", "version-1", "bob", pa, { ...marshalReveal(rv), v: 1 }, "invalid", [good]);
  add("reveal", "commit-type", "bob", pa, { ...marshalReveal(rv), t: "commit" }, "invalid", [good]);
  add("reveal", "wrong-nonce", "bob", pa, marshalReveal({ ...rv, nonce: round }), "commitment", [good]);
  add("reveal", "wrong-round", "bob", pa, marshalReveal({ ...rv, round: nonce }), "commitment", [good]);
  add("reveal", "older-gen", "bob", pa, marshalReveal({ ...rv, gen: 2 }), "stale", [good]);
  add("reveal", "own-peer", "bob", pa, marshalReveal({ ...rv, peer: bob.peer }), "own");
  return out;
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
    assert.deepEqual(got.auth, key.auth);
    assert.equal(got.authSeed, undefined);
    assert.equal(got.keyId, key.keyId);
    assert.equal(got.peer, from.peer);
  }
  return key;
}

async function quietly(promise, ...codes) {
  try {
    return await promise;
  } catch (error) {
    if (!codes.includes(errorCode(error))) throw error;
    return null;
  }
}

async function runRound(...all) {
  for (const s of all) {
    const c = await s.commit();
    for (const r of all) if (r !== s) await quietly(r.acceptCommit(c), "duplicate");
  }
  for (const s of all) {
    const rv = await s.reveal();
    for (const r of all) if (r !== s) await quietly(r.acceptReveal(rv), "duplicate");
  }
  let code = null;
  for (const s of all) {
    const st = await s.verification();
    assert.ok(st.code && !st.needCommit && st.waiting.length === 0, JSON.stringify(st));
    if (code) assert.deepEqual(st.code.transcript, code.transcript);
    code = st.code;
  }
  return code;
}

test("three participants agree on every sender's key and on the short code", async () => {
  const clock = clockAt();
  const [a, b, d] = [await participant(clock, "call-1", "alice"), await participant(clock, "call-1", "bob"), await participant(clock, "call-1", "dave")];
  await meet(a, b, d);
  const ka = await distribute(a, b, d);
  const kb = await distribute(b, a, d);
  assert.notDeepEqual(ka.raw, kb.raw);
  assert.notDeepEqual(ka.auth, kb.auth);
  const code = await runRound(a, b, d);
  assert.match(code.digits, /^\d{3} \d{3}$/);
  assert.equal(code.emoji.length, 4);
  assert.equal(code.qr.length, "FMCALL2:".length + 52);
  assert.deepEqual(parseQr(code.qr), code.transcript);
  assert.deepEqual((await runRound(a, b, d)).transcript, code.transcript);
  a.leave(d.peer);
  b.leave(d.peer);
  const after = await runRound(a, b);
  assert.notDeepEqual(after.transcript, code.transcript);
  const { message } = await a.rekey();
  await rejects(d.openKeys(message), "not-for-me");
  await b.openKeys(message);
});

test("reveal waits for every commit and a split view gives different codes", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  const fakeB = await participant(clock, "call-1", "bob");
  await meet(a, fakeB);
  await rejects(a.reveal(), "waiting");
  await a.commit();
  await rejects(a.reveal(), "waiting");
  const st = await a.verification();
  assert.deepEqual(st.waiting, [fakeB.peer]);
  clock.t += REVEAL_TIMEOUT_MS + 1000;
  assert.ok((await a.verification()).late);
  const left = await runRound(a, fakeB);
  const fakeA = await participant(clock, "call-1", "alice");
  await meet(b, fakeA);
  const right = await runRound(b, fakeA);
  assert.notDeepEqual(left.transcript, right.transcript);
});

test("an attacker cannot re-commit after seeing a nonce, and restarts are counted", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const signer = await signerFromSeed(random(32));
  const m = await createCallSession({ call: "call-1", peer: "6d616c6c6f7279", acct: "mallory", signer, now: clock.now });
  await meet(a, m);
  await a.acceptCommit(await m.commit());
  await m.acceptCommit(await a.commit());
  await m.acceptReveal(await a.reveal());
  const mc = await parseCommit(await m.commit());
  const resign = async (c) => signed(signer, { ...c, sig: null }, commitMessage, marshalCommit);
  const nonce = new Uint8Array(32).fill(7);
  const regrind = await resign({ ...mc, c: await commitHash(mc.call, mc.peer, mc.idk, mc.round, mc.gen, nonce) });
  await rejects(a.acceptCommit(regrind), "equivocation");
  await rejects(a.acceptReveal(marshalReveal({ call: "call-1", peer: m.peer, round: mc.round, gen: mc.gen, nonce })), "commitment");
  assert.equal((await a.verification()).rounds, 1);
  const roster = [
    { idk: a.idk, acct: "alice", peer: a.peer },
    { idk: signer.idk, acct: "mallory", peer: m.peer },
  ];
  const round = await roundId("call-1", mc.gen + 1, roster);
  await a.acceptCommit(await resign({ ...mc, gen: mc.gen + 1, round, c: await commitHash(mc.call, mc.peer, mc.idk, round, mc.gen + 1, nonce) }));
  const st = await a.verification();
  assert.ok(st.needCommit && !st.code);
  await a.commit();
  assert.equal((await a.verification()).unfinished, 1);
});

test("a transient roster change converges on one generation", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  const ghost = await participant(clock, "call-1", "ghost");
  await meet(a, b);
  const first = await runRound(a, b);
  await b.acceptPresence(await ghost.presence());
  await b.commit();
  b.leave(ghost.peer);
  await a.acceptCommit(await b.commit());
  assert.ok((await a.verification()).needCommit);
  for (let i = 0; i < 4; i += 1) {
    const ca = await a.commit();
    const cb = await b.commit();
    await quietly(b.acceptCommit(ca), "duplicate");
    await quietly(a.acceptCommit(cb), "duplicate");
    const ra = await quietly(a.reveal(), "waiting");
    if (ra) await quietly(b.acceptReveal(ra), "duplicate");
    const rb = await quietly(b.reveal(), "waiting");
    if (rb) await quietly(a.acceptReveal(rb), "duplicate");
  }
  const [sa, sb] = [await a.verification(), await b.verification()];
  assert.ok(sa.code && sb.code);
  assert.deepEqual(sa.code.transcript, sb.code.transcript);
  assert.notDeepEqual(sa.code.transcript, first.transcript);
  assert.ok(sa.gen > 1);
});

test("verified survives leaves, not newcomers, and the QR form verifies", async () => {
  const clock = clockAt();
  const [a, b, d] = [await participant(clock, "call-1", "alice"), await participant(clock, "call-1", "bob"), await participant(clock, "call-1", "dave")];
  await meet(a, b, d);
  assert.equal((await a.verification()).verified, false);
  const code = await runRound(a, b, d);
  await rejects(a.markVerified(new Uint8Array(32)), "waiting");
  await a.markVerified(code.round);
  assert.ok((await a.verification()).verified);
  a.leave(d.peer);
  b.leave(d.peer);
  assert.ok((await a.verification()).verified);
  const e = await participant(clock, "call-1", "erin");
  await meet(a, b, e);
  const st = await a.verification();
  assert.equal(st.verified, false);
  assert.deepEqual(st.newcomers, [e.peer]);
  const next = await runRound(a, b, e);
  assert.equal(await a.checkQr(next.qr), true);
  assert.ok((await a.verification()).verified);
  assert.equal(await b.checkQr(sasFromTranscript(next.round, new Uint8Array(32).fill(9)).qr), false);
  await rejects(b.checkQr("FMCALL2:abc"), "invalid");
  a.unverify();
  assert.equal((await a.verification()).verified, false);
});

test("a revealed round that never finishes is counted once", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  await meet(a, b);
  await a.acceptCommit(await b.commit());
  await b.acceptCommit(await a.commit());
  await b.acceptReveal(await a.reveal());
  clock.t += ROUND_TIMEOUT_MS + 1000;
  const st = await a.verification();
  assert.equal(st.unfinished, 1);
  assert.equal(st.code, null);
  assert.equal((await a.verification()).unfinished, 1);
});

test("an early reveal is held until its commit", async () => {
  const clock = clockAt();
  const a = await participant(clock, "call-1", "alice");
  const b = await participant(clock, "call-1", "bob");
  await meet(a, b);
  const cb = await b.commit();
  await b.acceptCommit(await a.commit());
  const rb = await b.reveal();
  await rejects(a.acceptReveal(rb), "waiting");
  await a.acceptCommit(cb);
  await b.acceptReveal(await a.reveal());
  assert.ok((await a.verification()).code && (await b.verification()).code);
  await rejects(a.acceptReveal(rb), "duplicate");
});

function mediaKey(peer, epoch, fill) {
  const authSeed = new Uint8Array(32).fill(fill ^ 0x5a);
  return { call: "call-1", peer, epoch, keyId: epoch & 0xff, raw: new Uint8Array(32).fill(fill), authSeed };
}

async function sealFrames(worker, n, prefix = "frame") {
  const frames = [];
  for (let i = 0; i < n; i += 1) frames.push({ data: utf8("\x01" + prefix + " " + i + " of media") });
  return worker.run("encrypt", "audio", "p", frames);
}

async function mediaRig(clock) {
  const key = mediaKey("alice", 1, 4);
  const worker = loadWorker();
  await worker.post({ op: "send-key", keyId: key.keyId, raw: key.raw.slice().buffer });
  const signer = await createMediaSigner(key);
  const states = [];
  const verifier = createMediaVerifier({ now: clock.now, onState: (who, state) => states.push(state) });
  verifier.setKey({ ...key, authSeed: undefined, auth: signer.auth });
  return { key, worker, signer, verifier, states };
}

test("media auth verifies frames and catches a colluding participant's forgery", async () => {
  const clock = clockAt();
  const g = await mediaRig(clock);
  const real = await sealFrames(g.worker, 3);
  for (const f of real) await g.signer.add(f);
  const forger = loadWorker();
  await forger.post({ op: "send-key", keyId: g.key.keyId, raw: g.key.raw.slice().buffer });
  const [fake] = await sealFrames(forger, 1, "words alice never said");
  assert.equal(await g.verifier.frame("alice", fake), "");
  for (const f of real.slice(1)) assert.equal(await g.verifier.frame("alice", f), "");
  for (const m of await g.signer.flush()) await g.verifier.accept("alice", m);
  const st = g.verifier.status("alice");
  assert.equal(st.state, "forged");
  assert.equal(st.forged, 1);
  assert.equal(st.verified, 2);
  assert.ok(st.dropping);
  assert.equal(g.states.at(-1), "forged");
  const [more] = await sealFrames(g.worker, 1);
  assert.equal(await g.verifier.frame("alice", more), "forged");
  const colluder = await createMediaSigner({ ...g.key, authSeed: new Uint8Array(32).fill(0xee) });
  await colluder.add(fake);
  for (const m of await colluder.flush()) await rejects(g.verifier.accept("alice", m), "signature");
});

test("media auth: late frames verify, missing auth turns unverified then drops, gaps recover", async () => {
  const clock = clockAt();
  const g = await mediaRig(clock);
  const frames = await sealFrames(g.worker, 5);
  for (const f of frames) await g.signer.add(f);
  for (const f of frames.slice(0, 2)) await g.verifier.frame("alice", f);
  for (const m of await g.signer.flush()) await g.verifier.accept("alice", m);
  for (const f of frames.slice(2)) assert.equal(await g.verifier.frame("alice", f), "");
  assert.equal(g.verifier.status("alice").verified, 5);
  let dropped = "";
  for (let i = 0; i < 40 && !dropped; i += 1) {
    const [f] = await sealFrames(g.worker, 1);
    await g.signer.add(f);
    await g.signer.flush();
    dropped = await g.verifier.frame("alice", f);
    clock.t += 100;
    if (i === 12) assert.equal(g.verifier.status("alice").state, "missing");
  }
  assert.equal(dropped, "unverified");
  const [f] = await sealFrames(g.worker, 1);
  await g.signer.add(f);
  assert.equal(await g.verifier.frame("alice", f), "unverified");
  for (const m of await g.signer.flush()) await g.verifier.accept("alice", m);
  assert.equal(g.verifier.status("alice").dropping, false);
  clock.t += 1100;
  g.verifier.status("alice");
  clock.t += 3000;
  assert.equal(g.verifier.status("alice").state, "verified");
});

test("media auth windows split at 100 frames and refuse equivocation", async () => {
  const clock = clockAt();
  const g = await mediaRig(clock);
  const frames = await sealFrames(g.worker, 2 * MAX_AUTH_FRAMES + 50);
  for (const f of frames) await g.signer.add(f);
  const msgs = await g.signer.flush();
  assert.deepEqual(msgs.map((m) => parseAuthMessage(m).digests.length / 10), [100, 100, 50]);
  for (const f of frames) await g.verifier.frame("alice", f);
  for (const m of msgs) await g.verifier.accept("alice", m);
  assert.equal(g.verifier.status("alice").verified, frames.length);
  await rejects(g.verifier.accept("alice", msgs[0]), "duplicate");
  const m = parseAuthMessage(msgs[0]);
  const lie = await signAuthMessage("call-1", "alice", await signerFromSeed(g.key.authSeed), { ...m, digests: new Uint8Array(10).fill(2) });
  await rejects(g.verifier.accept("alice", lie), "equivocation");
  assert.equal(g.verifier.status("alice").state, "forged");
  await rejects(g.verifier.accept("bob", msgs[0]), "unknown-peer");
});

test("media auth costs: bytes per second and CPU per frame", async () => {
  const clock = clockAt();
  const rows = [];
  for (const [name, sizes, fps] of [
    ["audio only", [40], [50]],
    ["audio + video 30 fps", [60, 3000], [50, 30]],
    ["audio + 2 simulcast layers", [60, 3000, 900], [50, 30, 30]],
  ]) {
    const g = await mediaRig(clock);
    let bytes = 0;
    let frames = 0;
    let addMs = 0;
    let verifyMs = 0;
    for (let tick = 0; tick < 1000 / AUTH_INTERVAL_MS; tick += 1) {
      const batch = [];
      for (let s = 0; s < sizes.length; s += 1) {
        for (let n = 0; n < (fps[s] * AUTH_INTERVAL_MS) / 1000; n += 1) batch.push({ data: random(sizes[s]) });
      }
      const sealed = await g.worker.run("encrypt", "audio", "p", batch);
      let t0 = performance.now();
      for (const f of sealed) await g.signer.add(f);
      const msgs = await g.signer.flush();
      addMs += performance.now() - t0;
      t0 = performance.now();
      for (const f of sealed) await g.verifier.frame("alice", f);
      for (const m of msgs) await g.verifier.accept("alice", m);
      verifyMs += performance.now() - t0;
      frames += sealed.length;
      for (const m of msgs) bytes += m.length;
    }
    assert.equal(g.verifier.status("alice").verified, frames);
    assert.ok(bytes < (64 * frames) / 3);
    rows.push({ name, frames, bytes, signUsPerFrame: Math.round((addMs * 1000) / frames), verifyUsPerFrame: Math.round((verifyMs * 1000) / frames) });
  }
  if (process.env.CALL_E2EE_BENCH) console.log(JSON.stringify(rows, null, 1));
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
  const rekeys = await Promise.all([a.rekey(), a.rekey(), a.rekey(), a.presence(), a.rotate(), a.commit(), a.verification()]);
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
  const c = await a.commit();
  await rejects(parseCommit({ ...c, gen: 0 }), "invalid");
  await rejects(parseReveal(c), "invalid");
  assert.throws(() => parseAuthMessage(new Uint8Array(10)), CallE2eeError);
  assert.throws(() => parseQr("FMCALL2:" + "a".repeat(52)), CallE2eeError);
});
