import { test } from "node:test";
import assert from "node:assert/strict";
import { createFmmE2ee, FMM_E2EE_TYPES } from "../public/fmm/fmm-e2ee.js";
import { signerFromSeed, keyId } from "../public/chat-call-e2ee.js";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const peerFor = (n) => n.repeat(32).slice(0, 32);
const hex = (bytes) => Buffer.from(bytes).toString("hex");

async function party(name, relay) {
  const seed = new Uint8Array(32).fill(name.charCodeAt(0));
  const signer = await signerFromSeed(seed);
  const kid = await keyId(signer.idk);
  const me = { name, peer: peerFor(name[0] === "a" ? "a" : name[0] === "b" ? "b" : "c"), kid, sendKey: null, peerKeys: new Map(), forgotten: [], view: null };
  me.e2ee = createFmmE2ee({
    call: "chat:fmm-test",
    peer: me.peer,
    acct: "acct-" + name,
    signer,
    send: async (type, body) => relay.post(me, type, body),
    announce: () => void me.e2ee.presence().then((p) => p && relay.post(me, FMM_E2EE_TYPES.presence, { peer: me.peer, call: "in", e2ee: p })),
    onSendKey: (key) => { me.sendKey = key; },
    onPeerKey: (key) => { me.peerKeys.set(key.peer, key); },
    onForget: (who) => { me.forgotten.push(who); },
    onChange: (view) => { me.view = view; },
  });
  await me.e2ee.start();
  return me;
}

function createRelay() {
  const members = [];
  return {
    members,
    async post(from, type, body) {
      for (const m of members) {
        if (m === from) continue;
        setTimeout(() => void m.e2ee.handle(type, { type, ...body }, { kid: from.kid, acct: "acct-" + from.name }), 5);
      }
    },
  };
}

test("three participants exchange keys, agree on one code, and rekey when one leaves", async () => {
  const relay = createRelay();
  const all = [];
  for (const name of ["ana", "ben", "cai"]) {
    const p = await party(name, relay);
    relay.members.push(p);
    all.push(p);
    const presence = await p.e2ee.presence();
    await relay.post(p, FMM_E2EE_TYPES.presence, { peer: p.peer, call: "in", e2ee: presence });
    await sleep(100);
  }
  for (let i = 0; i < 60; i += 1) {
    await sleep(100);
    const keyed = all.every((p) => p.sendKey && all.every((q) => q === p || hex(q.peerKeys.get(p.peer)?.raw || []) === hex(p.sendKey.raw)));
    const codes = new Set(all.map((p) => p.view?.code?.digits || ""));
    if (keyed && codes.size === 1 && !codes.has("")) break;
  }
  for (const p of all) {
    for (const q of all) {
      if (p === q) continue;
      assert.equal(hex(q.peerKeys.get(p.peer).raw), hex(p.sendKey.raw), `${q.name} holds ${p.name}'s current key`);
      assert.deepEqual([...q.peerKeys.get(p.peer).auth], [...p.sendKey.auth]);
    }
  }
  const codes = new Set(all.map((p) => p.view.code.emojiText + " " + p.view.code.digits));
  assert.equal(codes.size, 1);
  assert.equal(all[0].view.code.emoji.length, 4);
  assert.ok(await all[0].e2ee.confirm());
  assert.equal(all[0].view.verified, true);
  const before = hex(all[0].sendKey.raw);
  await relay.post(all[2], FMM_E2EE_TYPES.presence, { peer: all[2].peer, call: "out" });
  relay.members.splice(2, 1);
  all[2].e2ee.close();
  for (let i = 0; i < 40 && hex(all[0].sendKey.raw) === before; i += 1) await sleep(100);
  assert.notEqual(hex(all[0].sendKey.raw), before, "ana rekeys after cai leaves");
  assert.ok(all[0].forgotten.includes(all[2].peer));
  await sleep(1500);
  assert.equal(hex(all[1].peerKeys.get(all[0].peer).raw), hex(all[0].sendKey.raw));
  for (const p of all.slice(0, 2)) p.e2ee.close();
});

test("a presence whose identity is not the sealed sender's is ignored", async () => {
  const relay = createRelay();
  const ana = await party("ana", relay);
  const ben = await party("ben", relay);
  const presence = await ben.e2ee.presence();
  await ana.e2ee.handle(FMM_E2EE_TYPES.presence, { type: FMM_E2EE_TYPES.presence, peer: ben.peer, call: "in", e2ee: presence }, { kid: ana.kid, acct: "acct-ben" });
  assert.deepEqual(ana.e2ee.session.peers(), []);
  await ana.e2ee.handle(FMM_E2EE_TYPES.presence, { type: FMM_E2EE_TYPES.presence, peer: ben.peer, call: "in", e2ee: presence }, { kid: ben.kid, acct: "acct-ben" });
  assert.deepEqual(ana.e2ee.session.peers(), [ben.peer]);
  ana.e2ee.close();
  ben.e2ee.close();
});
