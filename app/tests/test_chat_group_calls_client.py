import json
import shutil
import subprocess
from pathlib import Path

import pytest

PUBLIC = Path(__file__).resolve().parents[1] / "public"
ANA = "a" * 32
BEN = "b" * 32
CAT = "c" * 32
SELF_ONE = "1" * 32
SELF_TWO = "2" * 32

HARNESS = r"""
import { pathToFileURL } from "node:url";
const { createGroupCalls } = await import(pathToFileURL(process.argv[2]).href);
const [ANA, BEN, CAT, SELF_ONE, SELF_TWO] = JSON.parse(process.argv[3]);
const realTimeout = setTimeout;
globalThis.setTimeout = (fn) => realTimeout(fn, 0);
globalThis.setInterval = () => 0;
globalThis.clearInterval = () => {};
let clock = 1_800_000_000_000;
Date.now = () => clock;
const flush = async () => { for (let i = 0; i < 10; i += 1) await new Promise((resolve) => realTimeout(resolve, 2)); };
const hex = (buffer) => Buffer.from(new Uint8Array(buffer)).toString("hex");
const posted = [];
const sockets = [];
const sent = [];
class FakeWorker {
  postMessage(message) { posted.push(message); }
  terminate() {}
}
class FakeStream {
  constructor(tracks = []) { this.tracks = tracks; this.id = "local"; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  addTrack(t) { this.tracks.push(t); }
  removeTrack() {}
}
class FakePeerConnection {
  constructor() { this.localDescription = null; this.connectionState = "new"; }
  addTransceiver() { return { sender: {} }; }
  async createOffer() { return { type: "offer", sdp: "v=0" }; }
  async setLocalDescription(description) { this.localDescription = description; }
  async setRemoteDescription() {}
  close() {}
}
class FakeSocket {
  constructor(url) { this.url = url; this.readyState = 1; this.sent = []; sockets.push(this); }
  send(text) { this.sent.push(JSON.parse(text)); }
  close() { this.readyState = 3; }
}
const scope = {
  RTCRtpScriptTransform: class { constructor(worker, options) { this.options = options; } },
  RTCPeerConnection: FakePeerConnection,
  WebSocket: FakeSocket,
  Worker: FakeWorker,
  MediaStream: FakeStream,
  crypto: globalThis.crypto,
  navigator: { mediaDevices: { getUserMedia: async () => new FakeStream([{ kind: "audio", enabled: true, stop() {} }]) } },
};
const calls = createGroupCalls({
  sendFrame: async (channel, type, fields) => { sent.push({ channel, type, ...fields }); },
  fetchTicket: async () => ({ url: "wss://calls.example/calls/ws", ticket: "t" }),
  accountName: "me",
  scope,
});
const out = {};
out.joined = await calls.join("#team", "chan", "audio");
sockets[0].onmessage({ data: JSON.stringify({ type: "joined", peer: SELF_ONE }) });
await flush();
sockets[0].onclose();
await flush();
sockets[1].onmessage({ data: JSON.stringify({ type: "joined", peer: SELF_TWO }) });
await flush();
out.installed = posted.filter((m) => m.op === "send-key").map((m) => ({ keyId: m.keyId, raw: hex(m.raw) }));
out.shared = sent.filter((m) => m.type === "group-call-key").map((m) => ({ peer: m.peer, keyId: m.keyId,
  raw: Buffer.from(m.key, "base64").toString("hex") }));
out.offers = sockets.map((s) => s.sent.filter((m) => m.type === "offer").length);

const presence = (peer, sender, call = "in") => calls.handleFrame({ type: "group-call-presence", peer, call, sender }, "#team");
const key = (peer, sender, keyId = 1) => calls.handleFrame({ type: "group-call-key", peer, keyId, sender,
  key: Buffer.from(new Uint8Array(16).fill(keyId)).toString("base64") }, "#team");
const names = () => calls.roster("#team").map((entry) => entry.name + ":" + entry.peer[0]).sort();
const keysFor = (peer) => posted.filter((m) => m.op === "key" && m.peer === peer).length;
const forgets = (peer) => posted.filter((m) => m.op === "forget" && m.peer === peer).length;

presence(ANA, "Ana");
out.bound = names();
presence(ANA, "mallory", "out");
out.afterForeignOut = names();
out.forgetAfterForeignOut = forgets(ANA);
presence(ANA, "mallory");
out.afterForeignIn = names();
key(ANA, "mallory");
out.keysAfterForeignKey = keysFor(ANA);
key(ANA, "ana", 2);
out.keysAfterOwnerKey = keysFor(ANA);
presence(BEN, "");
presence(BEN, "   ");
key(BEN, "");
out.afterAnonymous = names();
out.keysAfterAnonymous = keysFor(BEN);
key(CAT, "cat", 3);
presence(CAT, "mallory");
out.afterKeyBinding = names();
presence(CAT, "cat");
out.afterKeyOwnerPresence = names();
presence(ANA, "ana", "out");
out.afterOwnerOut = names();
out.forgetAfterOwnerOut = forgets(ANA);
presence(ANA, "mallory");
out.afterReclaim = names();
clock += 61_000;
presence(CAT, "dan");
out.afterExpiry = names();
calls.leave();
console.log(JSON.stringify(out));
"""


def _run():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    peers = json.dumps([ANA, BEN, CAT, SELF_ONE, SELF_TWO])
    result = subprocess.run(
        [node, "--input-type=module", "-", str(PUBLIC / "chat-group-calls.js"), peers],
        input=HARNESS, capture_output=True, text=True, timeout=120, check=False)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


@pytest.fixture(scope="module")
def run():
    return _run()


def test_every_join_installs_and_shares_a_new_key(run):
    assert run["joined"] == {"ok": True}
    assert run["offers"] == [1, 1]
    installed = run["installed"]
    assert len(installed) == 2
    assert installed[0]["keyId"] != installed[1]["keyId"]
    assert installed[0]["raw"] != installed[1]["raw"]
    assert all(len(item["raw"]) == 32 for item in installed)
    shared = [item for item in run["shared"] if item["peer"] in (SELF_ONE, SELF_TWO)]
    assert {(item["peer"], item["keyId"], item["raw"]) for item in shared} == {
        (SELF_ONE, installed[0]["keyId"], installed[0]["raw"]),
        (SELF_TWO, installed[1]["keyId"], installed[1]["raw"]),
    }


def test_a_peer_id_stays_with_the_first_sender_that_claimed_it(run):
    assert run["bound"] == ["ana:a"]
    assert run["afterForeignOut"] == ["ana:a"]
    assert run["forgetAfterForeignOut"] == 0
    assert run["afterForeignIn"] == ["ana:a"]
    assert run["keysAfterForeignKey"] == 0
    assert run["keysAfterOwnerKey"] == 1


def test_frames_without_a_sender_are_ignored(run):
    assert run["afterAnonymous"] == ["ana:a"]
    assert run["keysAfterAnonymous"] == 0


def test_a_key_frame_binds_the_peer_too(run):
    assert run["afterKeyBinding"] == ["ana:a"]
    assert run["afterKeyOwnerPresence"] == ["ana:a", "cat:c"]


def test_the_binding_ends_when_its_sender_leaves_or_goes_quiet(run):
    assert run["afterOwnerOut"] == ["cat:c"]
    assert run["forgetAfterOwnerOut"] == 1
    assert run["afterReclaim"] == ["cat:c", "mallory:a"]
    assert run["afterExpiry"] == ["dan:c"]
