import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { b64ToBytes, b64UrlToBytes, bytesToB64Url, decryptObject, derivePassphraseKey, encryptObject } from "../public/chat-crypto.js";
import {
  FRAME_AUTH,
  acceptSealKey,
  authenticatedPlain,
  createRoomSealStore,
  isSealedFrame,
  isSealKeyFrame,
  nodeBindMessage,
  nodeBundleMessage,
  sealFrame,
  sealKeyFrame,
  unsealFrame,
  verifyNodeBinding,
} from "../public/chat-seal.js";
import {
  attributeFrame,
  bindNodeKey,
  botLine,
  botViaName,
  isBotViaName,
  chatProfileFor,
  createNodeOwners,
  forgetLegacySelfId,
  guestLabel,
  historyEntries,
  isPrivateRelayFrame,
  mayChangeMessage,
  serverPlain,
} from "../public/chat-frame-auth.js";
import { createChatRoomTransport } from "../public/chat-room-transport.js";

const ROOT = new URL("../../", import.meta.url);
const BINDING = JSON.parse(readFileSync(new URL("conformance/chat/v3/node-binding.json", ROOT), "utf8"));
const PREFIX = "forkmesh.chat/e2ee/";
const NOW = 1_800_000_000_000;

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

function store(acct, options = {}) {
  return createRoomSealStore({ storage: memoryStorage(), acct, wrappingKey: null, ...options });
}

async function edVerify(publicB64u, sigB64u, message) {
  const key = await crypto.subtle.importKey("raw", b64UrlToBytes(publicB64u), { name: "Ed25519" }, false, ["verify"]);
  return crypto.subtle.verify({ name: "Ed25519" }, key, b64UrlToBytes(sigB64u), message);
}

class FakeWebSocket {
  static OPEN = 1;
  static instances = [];
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.sent = [];
    this.listeners = new Map();
    FakeWebSocket.instances.push(this);
  }
  addEventListener(type, callback) {
    const list = this.listeners.get(type) || [];
    list.push(callback);
    this.listeners.set(type, list);
  }
  emit(type, event = {}) {
    for (const callback of this.listeners.get(type) || []) callback(event);
  }
  open() {
    this.readyState = 1;
    this.emit("open");
  }
  send(value) { this.sent.push(value); }
  close() { this.readyState = 3; }
}

const settle = (ms = 60) => new Promise((resolve) => setTimeout(resolve, ms));

test("a browser's node key signs its bundle exactly as the node-binding vector says", async () => {
  const storage = memoryStorage();
  storage.setItem(PREFIX + "ed25519NodeV1", BINDING.nodeSeed);
  storage.setItem(PREFIX + "ed25519IdentityV3", BINDING.identitySeed);
  const own = createRoomSealStore({ storage, acct: BINDING.acct, wrappingKey: null });
  assert.equal(await own.ready(), BINDING.node);
  const seal = await own.resolve("public-world-general", BINDING.room);
  const bundle = await sealKeyFrame(seal);
  assert.equal(bundle.node, BINDING.node);
  assert.equal(bundle.idk, BINDING.idk);
  assert.equal(bundle.acct, BINDING.acct);
  assert.equal(new TextDecoder().decode(nodeBundleMessage(BINDING.room, bundle.kid, bundle.idk, bundle.acct)), BINDING.message);
  assert.equal(bundle.nsig, BINDING.nsig);
  const kid = BINDING.message.split("\n")[2];
  assert.equal(bundle.kid, kid);
  assert.equal(await verifyNodeBinding({ node: BINDING.node, kid, idk: BINDING.idk, acct: BINDING.acct, nsig: BINDING.nsig }, BINDING.room), true);
  for (const reject of BINDING.rejects) {
    const claim = { node: reject.node, kid, idk: BINDING.idk, acct: BINDING.acct, nsig: reject.nsig };
    assert.equal(await verifyNodeBinding(claim, reject.room), false, reject.why);
  }
});

test("open rooms drop bundles without a valid node signature and strict rooms do not need one", async () => {
  const room = "world-general";
  const me = await store("me").resolve("public-world-general", room);
  const peerStore = store("peer");
  const peer = await peerStore.resolve("public-world-general", room);
  const bundle = await sealKeyFrame(peer);
  assert.ok(bundle.nsig);
  const { nsig: _dropped, ...unsigned } = bundle;
  assert.equal(await acceptSealKey(unsigned, me), null);
  const other = await sealKeyFrame(await store("peer").resolve("public-world-general", room));
  assert.equal(await acceptSealKey({ ...bundle, nsig: other.nsig }, me), null);
  const elsewhere = await sealKeyFrame(await peerStore.resolve("public-general", "general"));
  assert.equal(await acceptSealKey({ ...bundle, nsig: elsewhere.nsig }, me), null);
  assert.equal((await acceptSealKey(bundle, me)).state, "approved");
  const dm = { participants: ["me", "peer"] };
  const strictMe = await store("me", { node: "node-me" }).resolve("direct:00", "chat-direct:00:v1", "", dm);
  const strictPeer = await store("peer", { node: "node-peer" }).resolve("direct:00", "chat-direct:00:v1", "", dm);
  assert.equal((await acceptSealKey(await sealKeyFrame(strictPeer), strictMe)).state, "approved");
});

test("a guest and each signed-in account get unlinkable node keys and identities in one browser", async () => {
  const storage = memoryStorage();
  storage.setItem(PREFIX + "ed25519IdentityV3", BINDING.identitySeed);
  storage.setItem(PREFIX + "trustV3/eA", "{}");
  storage.setItem("forkmesh.chat.selfId", "0b6f6c1e-legacy");
  const make = (profile, acct) => createRoomSealStore({ storage, acct, profile, wrappingKey: null });
  const guest = make(chatProfileFor(""), "");
  const alice = make(chatProfileFor("Alice"), "alice");
  const bob = make(chatProfileFor("bob"), "bob");
  const nodes = [await guest.ready(), await alice.ready(), await bob.ready()];
  assert.equal(new Set(nodes).size, 3);
  for (const node of nodes) assert.equal(b64UrlToBytes(node).length, 32);
  const kids = [(await guest.identity()).kid, (await alice.identity()).kid, (await bob.identity()).kid];
  assert.equal(new Set(kids).size, 3);
  assert.equal(await make(chatProfileFor("alice"), "alice").ready(), nodes[1]);
  const guestSeal = await guest.resolve("public-world-general", "world-general");
  const guestBundle = await sealKeyFrame(guestSeal);
  assert.equal(guestBundle.acct, nodes[0]);
  assert.equal(guestBundle.node, nodes[0]);
  const left = [...storage.items.keys()];
  assert.ok(left.every((key) => key.startsWith(PREFIX + "p/") || key === "forkmesh.chat.selfId"));
  forgetLegacySelfId(storage);
  assert.equal(storage.getItem("forkmesh.chat.selfId"), null);
});

test("the node-bind proof is signed by the node key for the account", async () => {
  const own = store("alice", { now: () => NOW });
  const proof = await own.nodeBindProof("Alice");
  assert.equal(proof.node, await own.ready());
  assert.equal(proof.ts, NOW);
  assert.equal(await edVerify(proof.node, proof.sig, nodeBindMessage("alice", proof.node, NOW)), true);
  assert.equal(new TextDecoder().decode(nodeBindMessage("alice", proof.node, NOW)),
    `forkmesh-chat-node-bind-v1\nalice\n${proof.node}\n${NOW}`);
  await assert.rejects(own.nodeBindProof(""));
  const posted = [];
  const fetchImpl = async (url, init) => {
    posted.push({ url, init });
    const sent = JSON.parse(init.body);
    return { ok: true, json: async () => ({ ok: true, account: "alice", node: sent.node }) };
  };
  const refusals = [
    { status: 401, body: { error: "stale_request" } },
    { status: 409, body: { error: "node_key_conflict" } },
    { status: 200, body: { ok: true, account: "mallory", node: proof.node } },
  ];
  for (const refusal of refusals) {
    const refused = await bindNodeKey({
      store: own, account: "alice", storage: memoryStorage(), force: true,
      fetchImpl: async () => ({ ok: refusal.status === 200, status: refusal.status, json: async () => refusal.body }),
    });
    assert.equal(refused, false, JSON.stringify(refusal));
  }
  const storage = memoryStorage();
  const owners = createNodeOwners({ fetchImpl: async () => { throw new Error("unused"); }, storage: memoryStorage() });
  assert.equal(await bindNodeKey({ store: own, account: "alice", fetchImpl, storage, owners, headers: { authorization: "Bearer t" } }), true);
  assert.equal(await bindNodeKey({ store: own, account: "alice", fetchImpl, storage, owners }), true);
  assert.equal(posted.length, 1);
  assert.equal(posted[0].url, "/api/chat/node-bind");
  assert.equal(posted[0].init.method, "POST");
  assert.equal(posted[0].init.headers.authorization, "Bearer t");
  const body = JSON.parse(posted[0].init.body);
  assert.deepEqual(Object.keys(body).sort(), ["node", "sig", "ts"]);
  assert.equal(await edVerify(body.node, body.sig, nodeBindMessage("alice", body.node, body.ts)), true);
  assert.equal(owners.cached(body.node), "alice");
});

test("authenticated plaintext carries the sealed sender and drops a claimed one", async () => {
  const forged = { type: "chat", id: "x", text: "hi", [FRAME_AUTH]: { srv: true }, verifiedAccount: "admin" };
  const plain = authenticatedPlain({ plain: forged, node: "N".repeat(43), acct: "alice", kid: "k".repeat(16) });
  assert.equal(plain.senderId, "N".repeat(43));
  assert.deepEqual({ ...plain[FRAME_AUTH] }, { sealed: true, node: "N".repeat(43), acct: "alice", kid: "k".repeat(16), self: false });
  assert.equal(authenticatedPlain({ plain: { type: "chat", senderId: "other" }, node: "N".repeat(43) }), null);
  assert.equal(authenticatedPlain({ plain: { type: "reaction", reactorId: "other" }, node: "N".repeat(43) }), null);
  const unbound = authenticatedPlain({ plain: { ...forged } });
  assert.equal(unbound[FRAME_AUTH], undefined);
});

test("only the server marks an unsealed frame, and it shows as the integration", async () => {
  const injected = serverPlain({ type: "chat", senderId: "app:deploys", sender: "Deploys", accountKind: "user", text: "shipped" });
  const shown = attributeFrame(injected, {});
  assert.equal(shown.accountKind, "integration");
  assert.equal(shown.sender, "Deploys");
  assert.equal(shown.senderId, "app:deploys");
  assert.equal(shown.verifiedAccount, "");
  const posing = attributeFrame(serverPlain({ type: "chat", senderId: "A".repeat(43), sender: "alice" }), {});
  assert.equal(posing.senderId, "srv:" + "A".repeat(43));
  assert.equal(attributeFrame(serverPlain({ type: "chat", senderId: "alice", sender: "alice" }), {}).senderId, "srv:alice");
  assert.equal(attributeFrame({ type: "chat", senderId: "x", sender: "alice", accountKind: "user" }, {}), null);
});

test("an account name shows only once the server confirms the node, and unconfirmed claims are guests", async () => {
  const node = bytesToB64Url(new Uint8Array(32).fill(7));
  const calls = [];
  let answer = { account: "alice" };
  let clock = NOW;
  const owners = createNodeOwners({
    storage: memoryStorage(),
    now: () => clock,
    fetchImpl: async (url) => {
      calls.push(url);
      return { ok: true, json: async () => answer };
    },
  });
  const sealed = (acct, extra = {}) => authenticatedPlain({
    plain: { type: "chat", id: "m", sender: "alice", accountKind: "user", text: "hi", ...extra }, node, acct, kid: "k".repeat(16),
  });
  const before = attributeFrame(sealed("alice"), { owners });
  assert.equal(before.accountKind, "guest");
  assert.equal(before.sender, guestLabel(node));
  await owners.prefetch([sealed("alice"), sealed("alice")]);
  assert.deepEqual(calls, [`/api/chat/node-owner?node=${node}`]);
  const after = attributeFrame(sealed("alice"), { owners });
  assert.equal(after.accountKind, "user");
  assert.equal(after.sender, "alice");
  assert.equal(after.verifiedAccount, "alice");
  assert.equal(attributeFrame(sealed("mallory"), { owners }).accountKind, "guest");
  clock += 59 * 60 * 1000;
  await owners.prefetch([sealed("alice")]);
  assert.equal(calls.length, 1);
  clock += 2 * 60 * 1000;
  answer = { account: "" };
  await owners.prefetch([sealed("alice")]);
  assert.equal(calls.length, 2);
  assert.equal(attributeFrame(sealed("alice"), { owners }).accountKind, "guest");
  await owners.prefetch([sealed(node)]);
  assert.equal(calls.length, 2);
  const mine = attributeFrame(sealed("alice"), { owners, selfNode: node, selfAccount: "alice" });
  assert.equal(mine.accountKind, "user");
  const direct = attributeFrame(sealed("bob"), { direct: true });
  assert.equal(direct.sender, "bob");
  let limited = 0;
  const failing = createNodeOwners({
    storage: memoryStorage(), now: () => clock,
    fetchImpl: async () => { limited += 1; return { ok: false, status: 429, json: async () => ({ error: "rate_limited" }) }; },
  });
  await failing.prefetch([sealed("alice")]);
  assert.equal(attributeFrame(sealed("alice"), { owners: failing }).accountKind, "guest");
  await failing.prefetch([sealed("alice")]);
  assert.equal(limited, 1);
  clock += 61 * 1000;
  await failing.prefetch([sealed("alice")]);
  assert.equal(limited, 2);
});

test("edits and deletes need the sealed author; history entries keep only the sender's own lines", () => {
  const node = "B".repeat(43);
  const auth = { sealed: true, node, acct: "bob", kid: "k".repeat(16), self: false };
  const edit = { type: "edit", target: "m1", senderId: node, [FRAME_AUTH]: auth };
  assert.equal(mayChangeMessage(edit, { senderId: node }), true);
  assert.equal(mayChangeMessage(edit, { senderId: "C".repeat(43) }), false);
  assert.equal(mayChangeMessage({ type: "edit", target: "m1", senderId: node }, { senderId: node }), false);
  assert.equal(mayChangeMessage({ ...edit, [FRAME_AUTH]: { srv: true } }, { senderId: node }), false);
  const history = {
    type: "history",
    senderId: node,
    sender: "bob",
    accountKind: "user",
    [FRAME_AUTH]: auth,
    entries: [
      { type: "chat", id: "own", senderId: node, sender: "bob", text: "mine" },
      { type: "chat", id: "theirs", senderId: "C".repeat(43), sender: "carol", text: "not mine" },
      { type: "dm", id: "dm", senderId: node, text: "psst" },
    ],
  };
  const entries = historyEntries(history);
  assert.deepEqual(entries.map((entry) => entry.id), ["own"]);
  assert.equal(entries[0][FRAME_AUTH], auth);
});

test("relay rooms carry no direct messages, private frames, invites or cove frames", () => {
  for (const frame of [
    { type: "dm", text: "x" },
    { type: "chat", conversation: "@bob", text: "x" },
    { type: "chat", private: true, text: "x" },
    { type: "invite" },
    { type: "cove-invite" },
    { type: "cove-open" },
  ]) assert.equal(isPrivateRelayFrame(frame), true, JSON.stringify(frame));
  assert.equal(isPrivateRelayFrame({ type: "chat", channel: "#general", conversation: "#general" }), false);
});

test("a public room seals person frames, accepts only server-marked plaintext and drops private frames", async () => {
  FakeWebSocket.instances = [];
  const roomName = "world-general";
  const scope = "public-world-general";
  const meStore = store("");
  const peerStore = store("");
  const peer = await peerStore.resolve(scope, roomName);
  const received = [];
  const refused = [];
  const transport = createChatRoomTransport({
    authorize: async () => ({ scope, passphrase: "relay-derived", roomName, webSocketUrl: "/ws" }),
    allowsControllerAccess: () => true,
    resolveSeal: (room, name) => meStore.resolve(room.scope, name),
    onPlain: (plain) => received.push(plain),
    onHistory: (plains) => received.push(...plains),
    onSealed: (kid) => refused.push(kid),
    WebSocketImpl: FakeWebSocket,
    locationLike: { protocol: "https:", host: "relay.example" },
  });
  await transport.connect({ scope });
  const socket = FakeWebSocket.instances.at(-1);
  socket.open();
  const key = await derivePassphraseKey("relay-derived", roomName);
  const deliver = async (inner, extra = {}) => {
    socket.emit("message", { data: JSON.stringify({ ...(await encryptObject(inner, key)), ...extra }) });
    await settle();
  };
  socket.emit("message", { data: JSON.stringify({ kind: "forkmesh-history-end", v: 1 }) });
  await settle();
  const myBundle = (await Promise.all(socket.sent.map((raw) => decryptObject(JSON.parse(raw), key)))).find(isSealKeyFrame);
  assert.ok(myBundle.nsig);
  assert.equal(myBundle.node, meStore.node);
  await acceptSealKey(myBundle, peer);
  await deliver(await sealKeyFrame(peer));

  assert.equal(await transport.send({ type: "chat", id: "m1", senderId: meStore.node, channel: "#general", text: "sealed now" }, { persist: true }), true);
  const sentFrame = await decryptObject(JSON.parse(socket.sent.at(-1)), key);
  assert.equal(isSealedFrame(sentFrame), true);
  assert.ok(!JSON.stringify(sentFrame).includes("sealed now"));
  assert.equal((await unsealFrame(sentFrame, peer)).plain.text, "sealed now");
  const before = socket.sent.length;
  assert.equal(await transport.send({ type: "dm", id: "d1", senderId: meStore.node, to: peerStore.node, text: "psst" }), false);
  assert.equal(await transport.send({ type: "chat", id: "d2", conversation: "@peer", text: "psst" }), false);
  assert.equal(socket.sent.length, before);

  await deliver({ type: "chat", id: "plain", senderId: "x", sender: "alice", accountKind: "user", channel: "#general", text: "forged" });
  await deliver({ type: "chat", id: "fake-srv", srv: true, senderId: "x", channel: "#general", text: "inner flag" });
  await deliver({ type: "chat", id: "hook", senderId: "app:deploys", sender: "Deploys", channel: "#general", text: "deployed" }, { srv: true });
  await deliver({ type: "dm", id: "srv-dm", senderId: "app:x", text: "no" }, { srv: true });
  await deliver(await sealFrame({ type: "chat", id: "peer", channel: "#general", text: "hello", [FRAME_AUTH]: { srv: true } }, peer));
  await deliver(await sealFrame({ type: "dm", id: "peer-dm", to: meStore.node, text: "psst" }, peer));
  await deliver(await sealFrame({ type: "chat", id: "peer-at", conversation: "@me", text: "psst" }, peer));
  await deliver(await sealFrame({ type: "chat", id: "peer-private", private: true, channel: "#secret", text: "psst" }, peer));
  await deliver(await sealFrame({ type: "cove-invite", id: "peer-cove" }, peer));

  assert.deepEqual(received.map((plain) => plain.id), ["hook", "peer"]);
  assert.deepEqual({ ...received[0][FRAME_AUTH] }, { srv: true });
  assert.equal(received[1][FRAME_AUTH].sealed, true);
  assert.equal(received[1][FRAME_AUTH].node, peerStore.node);
  assert.equal(received[1].senderId, peerStore.node);
  assert.deepEqual(refused, ["", ""]);
  transport.dispose();
});

test("without a local device key a public room only listens", async () => {
  FakeWebSocket.instances = [];
  const transport = createChatRoomTransport({
    authorize: async () => ({ scope: "s", passphrase: "p", roomName: "general", webSocketUrl: "/ws" }),
    allowsControllerAccess: () => true,
    resolveSeal: async () => null,
    WebSocketImpl: FakeWebSocket,
    locationLike: { protocol: "https:", host: "relay.example" },
    setTimeoutImpl: () => 1,
    clearTimeoutImpl: () => {},
  });
  assert.equal(await transport.connect({ scope: "s" }), true);
  FakeWebSocket.instances.at(-1).open();
  assert.equal(await transport.send({ type: "chat", id: "m", text: "plaintext" }, { persist: true }), false);
  assert.deepEqual(FakeWebSocket.instances.at(-1).sent, []);
  transport.dispose();
});

test("node ids from the vector decode to 32 bytes and other ids do not", () => {
  assert.equal(b64UrlToBytes(BINDING.node).length, 32);
  assert.equal(b64ToBytes(BINDING.nodeSeed).length, 32);
});

test("a bot line names the person who posted it", () => {
  const node = "D".repeat(43);
  const auth = { sealed: true, node, acct: node, kid: "k".repeat(16), self: false };
  const sealed = (sender, extra = {}) => ({ type: "chat", id: "b", bot: "forkbot", senderId: node, sender, text: "hi", [FRAME_AUTH]: auth, ...extra });
  const guest = botLine(attributeFrame(sealed("forkbot"), {}));
  assert.equal(guest.sender, `forkbot · via ${guestLabel(node)}`);
  assert.equal(guest.senderId, "forkbot");
  assert.equal(guest.postedBy, node);
  assert.equal(guest.botVia, true);
  assert.equal(botLine(guest), guest);
  const owners = { cached: () => "alice" };
  const named = botLine(attributeFrame(sealed("forkbot", { [FRAME_AUTH]: { ...auth, acct: "alice" } }), { owners }));
  assert.equal(named.sender, "forkbot · via alice");
  assert.equal(botViaName(sealed("Guest x", { bot: "claude" }), ["forkbot"]), "");
  assert.equal(botViaName({ type: "chat", bot: "forkbot", sender: "forkbot" }), "");
  const unsealed = { type: "chat", bot: "forkbot", senderId: "x", sender: "forkbot" };
  assert.equal(botLine(unsealed), unsealed);
  assert.equal(isBotViaName(named.sender), true);
  assert.equal(isBotViaName("forkbot"), false);
});

test("server channel rooms, private and notices too, open server-marked frames; direct messages never do", async () => {
  const rooms = [
    { scope: "private:" + "a".repeat(32), controller: false, direct: false, expect: ["hook"] },
    { scope: "private:" + "b".repeat(32), controller: true, direct: false, expect: ["hook"] },
    { scope: "direct:" + "c".repeat(32), controller: false, direct: true, expect: [] },
  ];
  for (const room of rooms) {
    FakeWebSocket.instances = [];
    const roomName = "chat-channel:" + room.scope.slice(-8) + ":v1";
    const received = [];
    const own = store("me");
    const transport = createChatRoomTransport({
      authorize: async () => ({ scope: room.scope, passphrase: "p", roomName, webSocketUrl: "/ws" }),
      allowsControllerAccess: () => room.controller,
      isDirectRoom: () => room.direct,
      resolveSeal: (r, name) => own.resolve(r.scope, name),
      onPlain: (plain) => received.push(plain),
      onHistory: (plains) => received.push(...plains),
      WebSocketImpl: FakeWebSocket,
      locationLike: { protocol: "https:", host: "relay.example" },
    });
    await transport.connect({ scope: room.scope });
    const socket = FakeWebSocket.instances.at(-1);
    socket.open();
    const key = await derivePassphraseKey("p", roomName);
    socket.emit("message", { data: JSON.stringify({ kind: "forkmesh-history-end", v: 1 }) });
    await settle();
    const frame = (id) => ({ type: "chat", id, channel: "#mail", senderId: "webhook:mail", sender: "Mail log", text: "sent", webhook: true, serverVisible: true });
    socket.emit("message", { data: JSON.stringify({ ...(await encryptObject(frame("hook"), key)), srv: true }) });
    socket.emit("message", { data: JSON.stringify(await encryptObject(frame("plain"), key)) });
    await settle();
    assert.deepEqual(received.map((plain) => plain.id), room.expect, room.scope);
    if (room.expect.length) {
      const shown = attributeFrame(received[0], {});
      assert.equal(shown.accountKind, "integration");
      assert.equal(shown.sender, "Mail log");
      assert.equal(shown.serverVisible, true);
    }
    transport.dispose();
  }
});

test("a guest gets the same name the phones and the desktop give it", async () => {
  const { guestLabel } = await import("../public/chat-frame-auth.js");
  assert.equal(guestLabel("vMHdwpa4f6TzmYXkeTWtKxKuUQXNefMzP6Q7-i9Vp1c"), "Guest 5502");
  assert.equal(guestLabel("node-alice-phone"), "Guest 2483");
  assert.equal(guestLabel(""), "Guest");
});
