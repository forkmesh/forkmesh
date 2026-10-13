import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "public" / "desktop-control.js"

HARNESS = r'''
import { readFileSync } from "node:fs";
const source = readFileSync(process.argv[2], "utf8");
const url = "data:text/javascript;base64," + Buffer.from(source).toString("base64");
const dc = await import(url);
const scenario = process.argv[3];
const enc = new TextEncoder();
const dec = new TextDecoder();
const hex = (bytes) => Buffer.from(bytes).toString("hex");
const result = {};

async function fakeDesktop({ desktopKeyPair, desktopKey, approve = true, account = "alice", node = "studio" }) {
  const sent = [];
  let toBrowser = null;
  let seq = 0;
  let receiveSeq = 0;
  let pending = "";
  let state = "hello";
  let keys = null;
  let transcriptBytes = null;
  let sendCounter = 0;
  let receiveCounter = 0;
  let browserKey = "";
  const requests = [];
  function reply(session, bytes) {
    const b64 = Buffer.from(bytes).toString("base64");
    for (let at = 0; at < b64.length; at += 9000) {
      toBrowser({ type: "control", action: "data", session, node, from: "node",
        seq: seq++, data: b64.slice(at, at + 9000), more: at + 9000 < b64.length });
    }
  }
  async function sealed(session, obj) {
    const frame = await dc.sealFrame(keys.serverToClient, sendCounter++, enc.encode(JSON.stringify(obj)));
    reply(session, frame);
  }
  async function handle(frame) {
    if (frame.node !== node) return;
    sent.push(frame);
    if (frame.type !== "control") return;
    if (frame.action === "close") { state = "closed"; return; }
    if (frame.seq !== receiveSeq) throw new Error("out of order");
    receiveSeq += 1;
    pending += frame.data;
    if (frame.more) return;
    const payload = new Uint8Array(Buffer.from(pending, "base64"));
    pending = "";
    if (state === "hello") {
      const hello = JSON.parse(dec.decode(payload));
      result.hello = hello;
      browserKey = hello.key;
      const eph = await dc.newEphemeral();
      const serverEph = dc.bytesToB64Url(eph.publicKey);
      transcriptBytes = dc.transcript(hello.key, hello.eph, hello.nonce, desktopKey, serverEph);
      const shared = await dc.x25519(eph.privateKey, dc.b64UrlToBytes(hello.eph, 32));
      const derived = await dc.deriveKeys(shared, transcriptBytes);
      keys = derived;
      const sig = dc.bytesToB64Url(await crypto.subtle.sign({ name: "Ed25519" }, desktopKeyPair.privateKey, dc.serverSignedBytes(transcriptBytes)));
      state = "auth";
      reply(frame.session, enc.encode(JSON.stringify({ t: "fm-control-welcome", v: 1, key: desktopKey, eph: serverEph, name: node, user: account, sig })));
      return;
    }
    if (state === "auth") {
      const auth = JSON.parse(dec.decode(payload));
      const pub = await crypto.subtle.importKey("raw", dc.b64UrlToBytes(browserKey, 32), { name: "Ed25519" }, false, ["verify"]);
      result.authVerified = await crypto.subtle.verify({ name: "Ed25519" }, pub, dc.b64UrlToBytes(auth.sig, 64), dc.clientSignedBytes(transcriptBytes));
      state = "open";
      if (approve) await sealed(frame.session, { event: "session", data: { paired: true, name: "studio" } });
      else await sealed(frame.session, { event: "pairing-denied", data: { reason: "pair_on_lan" } });
      return;
    }
    const plain = await dc.openFrame(keys.clientToServer, receiveCounter++, payload);
    const message = JSON.parse(dec.decode(plain));
    requests.push(message);
    if (message.method === "agents.list") {
      await sealed(frame.session, { event: "agents.changed", data: {} });
      await sealed(frame.session, { id: message.id, ok: true, result: { sessions: [{ id: 7, prompt: "x".repeat(20000) }] } });
    } else {
      await sealed(frame.session, { id: message.id, ok: false, error: "unsupported" });
    }
  }
  const factory = (handlers) => {
    toBrowser = (frame) => queueMicrotask(() => handlers.onFrame(frame));
    let chain = Promise.resolve();
    return {
      start() { queueMicrotask(() => handlers.onConnected()); },
      send(frame) { chain = chain.then(() => handle(JSON.parse(JSON.stringify(frame)))).catch((e) => { result.fakeError = String(e); }); return true; },
      dispose() {},
    };
  };
  return { factory, sent, requests, handle, attach: (fn) => { toBrowser = fn; } };
}

if (scenario === "vectors") {
  const t = dc.transcript("CK", "CE", "CN", "SK", "SE");
  result.transcript = dec.decode(t);
  const keys = await dc.deriveKeys(new Uint8Array(32).fill(7), t);
  result.c2s = hex(keys.clientToServer);
  result.s2c = hex(keys.serverToClient);
  const sealed = await dc.sealFrame(keys.clientToServer, 3, enc.encode('{"id":1}'));
  result.sealed = hex(sealed);
  result.opened = dec.decode(await dc.openFrame(keys.clientToServer, 3, sealed));
  result.wrongCounter = await dc.openFrame(keys.clientToServer, 4, sealed);
  result.wrongKey = await dc.openFrame(keys.serverToClient, 3, sealed);
} else if (scenario === "handshake" || scenario === "wrong-key" || scenario === "denied") {
  const desktopKeyPair = await crypto.subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]);
  const desktopKey = dc.bytesToB64Url(await crypto.subtle.exportKey("raw", desktopKeyPair.publicKey));
  const identity = await dc.loadIdentity("alice", { indexedDb: null });
  const fake = await fakeDesktop({ desktopKeyPair, desktopKey, approve: scenario !== "denied" });
  const connection = dc.createDesktopConnection({
    node: "Studio", identity, account: "alice",
    desktopKeys: scenario === "wrong-key" ? ["A".repeat(43)] : [desktopKey],
    channelFactory: fake.factory, timeoutMs: 5000,
  });
  const events = [];
  connection.on("agents.changed", () => events.push("agents.changed"));
  try {
    const info = await connection.opened;
    result.info = info;
    const listed = await connection.request("agents.list", {});
    result.listed = listed.sessions[0].prompt.length;
    result.events = events;
    try {
      await connection.request("agents.diff", { id: 7 });
    } catch (error) {
      result.unsupported = error.code;
    }
    result.requests = fake.requests.map((r) => r.method);
    result.chunked = fake.sent.filter((f) => f.action === "data").length;
    connection.close();
    await new Promise((resolve) => setTimeout(resolve, 50));
    result.closedFrame = fake.sent.at(-1).action;
  } catch (error) {
    result.error = error.code || String(error);
    await new Promise((resolve) => setTimeout(resolve, 50));
    result.closeFrames = fake.sent.filter((f) => f.action === "close").length;
  }
  result.authVerified = result.authVerified ?? null;
  result.helloFields = result.hello ? Object.keys(result.hello).sort() : [];
  result.helloPlatform = result.hello?.platform;
  result.helloUser = result.hello?.user;
  result.nodeName = fake.sent[0]?.node;
  result.fakeError = result.fakeError || null;
} else if (scenario === "hub") {
  const pairs = [];
  for (const node of ["studio", "laptop"]) {
    const desktopKeyPair = await crypto.subtle.generateKey({ name: "Ed25519" }, true, ["sign", "verify"]);
    const desktopKey = dc.bytesToB64Url(await crypto.subtle.exportKey("raw", desktopKeyPair.publicKey));
    pairs.push({ node, desktopKey, fake: await fakeDesktop({ desktopKeyPair, desktopKey, node }) });
  }
  let sockets = 0;
  const channelFactory = (handlers) => {
    sockets += 1;
    for (const pair of pairs) pair.fake.attach((frame) => queueMicrotask(() => handlers.onFrame(frame)));
    let chain = Promise.resolve();
    return {
      start() { queueMicrotask(() => handlers.onConnected()); },
      send(frame) {
        const copy = JSON.parse(JSON.stringify(frame));
        chain = chain.then(async () => { for (const pair of pairs) await pair.fake.handle(copy); });
        return true;
      },
      dispose() {},
    };
  };
  const fetchImpl = async () => ({ ok: true, status: 200, json: async () => ({
    ok: true, account: "alice", desktopKeys: pairs.map((p) => p.desktopKey),
    desktops: [...pairs.map((p) => ({ key: p.desktopKey, node: p.node })), { key: "x", node: "Bad Name" }],
  }) });
  const hub = dc.createDesktopControlHub({ account: "alice", sessionToken: () => "v2.tok", fetchImpl, channelFactory, timeoutMs: 5000 });
  result.desktops = (await hub.desktops()).map((d) => d.node);
  const [studio, laptop] = await Promise.all([hub.connect("studio"), hub.connect("Laptop")]);
  result.names = [studio.info.name, laptop.info.name];
  result.sockets = sockets;
  result.same = (await hub.connect("studio")) === studio;
  const [a, b] = await Promise.all([studio.request("agents.list"), laptop.request("agents.list")]);
  result.listed = [a.sessions.length, b.sessions.length];
  result.connection = hub.connection("laptop") === laptop;
  hub.closeAll();
  result.closed = [studio.isOpen, laptop.isOpen];
} else if (scenario === "bind") {
  const calls = [];
  let round = 0;
  const fetchImpl = async (url, options) => {
    const body = JSON.parse(options.body);
    calls.push({ url, body, auth: options.headers.authorization || "" });
    round += 1;
    if (round === 1) return { ok: false, status: 401, json: async () => ({ error: "device_revoked" }) };
    return { ok: true, status: 200, json: async () => ({ ok: true, account: "alice", desktopKeys: ["K"] }) };
  };
  const bound = await dc.bindIdentity({ account: "Alice", sessionToken: () => "v2.tok", fetchImpl, indexedDb: null });
  const pub = await crypto.subtle.importKey("raw", dc.b64UrlToBytes(calls[1].body.pubkey, 32), { name: "Ed25519" }, false, ["verify"]);
  result.verified = await crypto.subtle.verify({ name: "Ed25519" }, pub, dc.b64UrlToBytes(calls[1].body.sig, 64),
    enc.encode(`forkmesh-browser-control-v1\nalice\n${calls[1].body.pubkey}\n${calls[1].body.ts}`));
  result.rotated = calls[0].body.pubkey !== calls[1].body.pubkey;
  result.url = calls[0].url;
  result.auth = calls[0].auth;
  result.desktopKeys = bound.desktopKeys;
}
console.log(JSON.stringify(result));
'''


def _run(scenario, tmp_path):
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    harness = tmp_path / "harness.mjs"
    harness.write_text(HARNESS, encoding="utf-8")
    completed = subprocess.run(
        ["node", "--no-warnings", str(harness), str(MODULE), scenario],
        capture_output=True, text=True, timeout=60, check=False)
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout.strip().splitlines()[-1])


def test_primitives_match_the_desktop_and_phone_vectors(tmp_path):
    result = _run("vectors", tmp_path)
    assert result["transcript"] == "forkmesh-local-control-v1\nCK\nCE\nCN\nSK\nSE"
    assert result["c2s"] == (
        "6e7a4a8c0002d3118fbc2d04ddbc268e61b1b45af566060d38e3235b2bd1e239")
    assert result["s2c"] == (
        "b76be7e0ae92fe18b561280e896315649f905d4ebed4685676c4dc4b1158596f")
    assert result["sealed"] == "4fb9fc7fc067c05ecc5591ec3f59b2f37b2727354152adc0"
    assert result["opened"] == '{"id":1}'
    assert result["wrongCounter"] is None
    assert result["wrongKey"] is None


def test_handshake_requests_events_and_chunked_frames(tmp_path):
    result = _run("handshake", tmp_path)
    assert result.get("error") is None, result
    assert result["fakeError"] is None
    assert result["authVerified"] is True
    assert result["info"]["name"] == "studio"
    assert result["listed"] == 20000
    assert result["events"] == ["agents.changed"]
    assert result["unsupported"] == "unsupported_method"
    assert result["requests"] == ["agents.list", "agents.diff"]
    assert result["closedFrame"] == "close"
    assert result["helloFields"] == [
        "eph", "key", "name", "nonce", "platform", "t", "user", "v"]
    assert result["helloPlatform"] == "web"
    assert result["helloUser"] == "alice"
    assert result["nodeName"] == "studio"


def test_a_desktop_key_not_on_the_account_is_refused(tmp_path):
    result = _run("wrong-key", tmp_path)
    assert result["error"] == "wrong_desktop"
    assert result["closeFrames"] == 1


def test_a_desktop_that_does_not_approve_the_browser_closes_the_channel(tmp_path):
    result = _run("denied", tmp_path)
    assert result["error"] == "pair_on_lan"


def test_bind_signs_the_proof_and_rotates_a_revoked_key(tmp_path):
    result = _run("bind", tmp_path)
    assert result["url"] == "/api/accounts/devices/browser-control"
    assert result["auth"] == "Bearer v2.tok"
    assert result["verified"] is True
    assert result["rotated"] is True
    assert result["desktopKeys"] == ["K"]


def test_hub_shares_one_socket_between_desktops(tmp_path):
    result = _run("hub", tmp_path)
    assert result["desktops"] == ["studio", "laptop"]
    assert result["names"] == ["studio", "laptop"]
    assert result["sockets"] == 1
    assert result["same"] is True
    assert result["listed"] == [1, 1]
    assert result["connection"] is True
    assert result["closed"] == [False, False]
