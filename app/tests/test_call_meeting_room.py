import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
REPO = ROOT.parent
sys.path.insert(0, str(REPO / "marketplace" / "packages" / "booking" / "content" / "worker"))
import booking  # noqa: E402

ROOM = "0123456789abcdef0123456789abcdef"
KEY = "ab" * 32
OTHER_KEY = "cd" * 32

CRYPTO_HARNESS = r"""
import { pathToFileURL } from "node:url";
const [path, payload] = process.argv.slice(-2);
const m = await import(pathToFileURL(path).href);
const [room, key, other] = JSON.parse(payload);
const out = {};
out.parsed = m.parseMeetingLink("?" + room, "#" + key);
out.upper = m.parseMeetingLink("?" + room.toUpperCase(), "#" + key);
out.noKey = m.parseMeetingLink("?" + room, "");
out.badRoom = m.parseMeetingLink("?alice", "#" + key);
const keys = await m.deriveMeetingKeys(key, room);
const again = await m.deriveMeetingKeys(key, room);
const otherKeys = await m.deriveMeetingKeys(other, room);
const otherRoom = await m.deriveMeetingKeys(key, "f".repeat(32));
out.code = keys.code;
out.sameCode = JSON.stringify(keys.code) === JSON.stringify(again.code);
out.otherCode = JSON.stringify(keys.code) !== JSON.stringify(otherKeys.code);
const now = 1_800_000_000_000;
const small = await m.sealMeetingBox(keys, { t: "group-call-presence", f: { peer: "a".repeat(32), call: "in" }, n: "Grace" }, { now });
const big = await m.sealMeetingBox(keys, { t: "group-call-key", f: { peer: "a".repeat(32), keyId: 3, key: "x".repeat(24) }, n: "G".repeat(40), tr: true, cam: true, mic: true }, { now });
out.boxes = [small, big];
out.opened = await m.openMeetingBox(again, small, { now: now + 1000 });
out.wrongKey = await m.openMeetingBox(otherKeys, small, { now });
out.wrongRoom = await m.openMeetingBox(otherRoom, small, { now });
const bytes = m.fromBase64url(small);
bytes[bytes.length - 1] ^= 1;
out.tampered = await m.openMeetingBox(keys, m.base64url(bytes), { now });
out.stale = await m.openMeetingBox(keys, small, { now: now + 6 * 60 * 1000 });
out.garbage = await m.openMeetingBox(keys, "!!!", { now });
out.names = [m.cleanMeetingName("  Grace ‮  Hopper\n"), m.cleanMeetingName("x".repeat(80)).length];
try { await m.deriveMeetingKeys("short", room); out.rejected = false; } catch (_) { out.rejected = true; }
console.log(JSON.stringify(out));
"""

TRANSCRIBE_HARNESS = r"""
import { pathToFileURL } from "node:url";
const m = await import(pathToFileURL(process.argv.at(-1)).href);
const made = [];
const timers = [];
class Modern {
  constructor() { this.started = null; made.push(this); }
  start(track) { if (track && track.bad) throw new TypeError("no"); this.started = track || "mic"; }
  stop() { this.stopped = true; this.onend?.(); }
  abort() { this.aborted = true; }
}
Modern.prototype.processLocally = false;
Modern.available = async ({ langs, processLocally }) => (processLocally && langs[0] === "en-US" ? "available" : "downloadable");
Modern.install = async () => true;
class Legacy {
  constructor() { this.started = null; made.push(this); }
  start(track) { this.started = track || "mic"; }
  stop() {}
}
Legacy.prototype.mode = "";
Legacy.availableOnDevice = async () => "available";
class Cloud {
  start() {}
}
const out = {};
out.status = [
  await m.offlineTranscriptionStatus("en-US", { SpeechRecognition: Modern }),
  await m.offlineTranscriptionStatus("fr-FR", { SpeechRecognition: Modern }),
  await m.offlineTranscriptionStatus("en-US", { webkitSpeechRecognition: Legacy }),
  await m.offlineTranscriptionStatus("en-US", { webkitSpeechRecognition: Cloud }),
  await m.offlineTranscriptionStatus("en-US", {}),
];
out.install = [await m.installOfflineTranscription("fr-FR", { SpeechRecognition: Modern }),
               await m.installOfflineTranscription("fr-FR", { SpeechRecognition: Cloud })];
let clock = 1000;
const transcriber = m.createTranscriber({ lang: "en-US", scope: { SpeechRecognition: Modern }, now: () => clock, setTimeoutImpl: (fn) => timers.push(fn) });
transcriber.addSource("self", () => "Alice");
out.beforeStart = made.length;
out.started = transcriber.start();
const self = made[0];
out.self = { local: self.processLocally, lang: self.lang, continuous: self.continuous, interim: self.interimResults, started: self.started };
transcriber.addSource("peer", () => "Grace", { id: "track-1" });
const peer = made[1];
out.peerStarted = peer.started;
self.onresult({ resultIndex: 0, results: [Object.assign([{ transcript: " hello  there " }], { isFinal: true }), Object.assign([{ transcript: "and" }], { isFinal: false })] });
clock = 2000;
peer.onresult({ resultIndex: 0, results: [Object.assign([{ transcript: "hi Alice" }], { isFinal: true })] });
out.lines = transcriber.lines;
out.live = transcriber.live();
self.onend();
out.restartQueued = timers.length;
timers.shift()();
out.restarted = made.length;
out.text = m.transcriptText(transcriber.lines, { title: "Meeting", locale: "en-GB" });
transcriber.stop();
out.running = transcriber.running;
const legacy = m.createTranscriber({ lang: "en-US", scope: { webkitSpeechRecognition: Legacy }, setTimeoutImpl: () => {} });
const before = made.length;
legacy.addSource("self", "Alice");
legacy.addSource("peer", "Grace", { id: "t" });
legacy.start();
out.legacy = { made: made.length - before, mode: made[before].mode, remote: legacy.remoteAudio };
const errors = [];
const cloud = m.createTranscriber({ lang: "en-US", scope: { SpeechRecognition: Cloud }, onError: (text) => errors.push(text) });
cloud.addSource("self", "Alice");
out.cloudStarted = cloud.start();
out.file = m.transcriptFileName("Quick chat!", new Date(2026, 10, 3, 9, 5));
out.language = [m.transcribeLanguage({ navigator: { language: "fr-CA" } }), m.transcribeLanguage({ navigator: { language: "xx" } })];
console.log(JSON.stringify(out));
"""


EXCHANGE_HARNESS = r"""
import { pathToFileURL } from "node:url";
const [calls, crypto] = process.argv.slice(-2);
const { createGroupCalls } = await import(pathToFileURL(calls).href);
const m = await import(pathToFileURL(crypto).href);
const realTimeout = setTimeout;
globalThis.setTimeout = (fn) => realTimeout(fn, 0);
globalThis.setInterval = () => 0;
globalThis.clearInterval = () => {};
const flush = async () => { for (let i = 0; i < 10; i += 1) await new Promise((resolve) => realTimeout(resolve, 2)); };
const hex = (buffer) => Buffer.from(new Uint8Array(buffer)).toString("hex");
const room = "0123456789abcdef0123456789abcdef";
const keys = await m.deriveMeetingKeys("ab".repeat(32), room);
const stranger = await m.deriveMeetingKeys("cd".repeat(32), room);
const relay = [];
class FakeStream {
  constructor(tracks = []) { this.tracks = tracks; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  addTrack(t) { this.tracks.push(t); }
  removeTrack() {}
}
class FakePeerConnection {
  addTransceiver() { return { sender: {} }; }
  async createOffer() { return { type: "offer", sdp: "v=0" }; }
  async setLocalDescription(d) { this.localDescription = d; }
  async setRemoteDescription() {}
  close() {}
}
function person(name, peer, sealKeys) {
  const posted = [];
  const sockets = [];
  class FakeWorker { postMessage(message) { posted.push(message); } terminate() {} }
  class FakeSocket { constructor() { this.readyState = 1; this.sent = []; sockets.push(this); } send(t) { this.sent.push(JSON.parse(t)); } close() {} }
  const scope = {
    RTCRtpScriptTransform: class {}, RTCPeerConnection: FakePeerConnection, WebSocket: FakeSocket,
    Worker: FakeWorker, MediaStream: FakeStream, crypto: globalThis.crypto,
  };
  const local = new FakeStream([{ kind: "audio", enabled: true, stop() {} }]);
  const calls = createGroupCalls({
    sendFrame: async (_key, type, body) => {
      relay.push({ from: peer, box: await m.sealMeetingBox(sealKeys, m.meetingEnvelope(type, body, { name, microphone: true })) });
    },
    fetchTicket: async () => ({ url: "wss://calls.example/calls/ws", ticket: "t", peer }),
    accountName: name.toLowerCase(),
    fmm: { enabled: false },
    acquireMedia: async () => local,
    scope,
  });
  const seen = { cursor: 0, names: [] };
  return {
    calls, posted, sockets, seen, peer, sealKeys,
    async deliver() {
      for (; seen.cursor < relay.length; seen.cursor += 1) {
        const frame = m.meetingFrame(await m.openMeetingBox(sealKeys, relay[seen.cursor].box));
        if (!frame || frame.peer === peer) continue;
        seen.names.push(frame.name);
        if (frame.frame) calls.handleFrame(frame.frame, "meeting:" + room);
      }
    },
  };
}
const ana = person("Ana Lima", "a".repeat(32), keys);
const ben = person("Ben Okafor", "b".repeat(32), keys);
const eve = person("Eve", "e".repeat(32), stranger);
for (const who of [ana, ben, eve]) {
  await who.calls.join("meeting:" + room, room, "audio");
  who.sockets[0].onmessage({ data: JSON.stringify({ type: "joined", peer: who.peer }) });
  await flush();
}
for (let round = 0; round < 3; round += 1) {
  for (const who of [ana, ben, eve]) await who.deliver();
  await flush();
}
const sendKey = (who) => hex(who.posted.filter((x) => x.op === "send-key").at(-1).raw);
const keysFrom = (who, peer) => who.posted.filter((x) => x.op === "key" && x.peer === peer).map((x) => hex(x.raw));
console.log(JSON.stringify({
  anaHasBen: keysFrom(ana, ben.peer).includes(sendKey(ben)),
  benHasAna: keysFrom(ben, ana.peer).includes(sendKey(ana)),
  eveHasAny: keysFrom(eve, ana.peer).length + keysFrom(eve, ben.peer).length,
  anaHasEve: keysFrom(ana, eve.peer).length,
  anaSaw: [...new Set(ana.seen.names)],
  roster: ana.calls.roster("meeting:" + room).map((entry) => entry.name),
  status: m.meetingFrame(m.meetingEnvelope("meeting-status", { peer: "c".repeat(32) }, { name: "Cy", transcribing: true })),
  leaving: m.meetingFrame(m.meetingEnvelope("group-call-presence", { peer: "c".repeat(32), call: "out" }, { name: "Cy" })).leaving,
  noPeer: m.meetingFrame(m.meetingEnvelope("group-call-key", { peer: "nope" }, { name: "Cy" })),
}));
"""


def node(script, *args):
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    result = subprocess.run(["node", "--input-type=module", "-e", script, "--", *args],
                            capture_output=True, text=True, timeout=60, check=False)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_meeting_links_keys_and_sealed_boxes():
    out = node(CRYPTO_HARNESS, str(PUBLIC / "call-meeting-crypto.js"),
               json.dumps([ROOM, KEY, OTHER_KEY]))
    assert out["parsed"] == {"room": ROOM, "key": KEY, "error": ""}
    assert out["upper"]["room"] == ROOM
    assert out["noKey"]["error"] == "key" and out["badRoom"]["error"] == "room"
    assert re.fullmatch(r"\d{3} \d{3}", out["code"]["digits"]) and len(out["code"]["words"]) == 3
    assert out["sameCode"] and out["otherCode"]
    for box in out["boxes"]:
        assert booking.BOX_RE.fullmatch(box), box
    assert out["opened"]["t"] == "group-call-presence" and out["opened"]["n"] == "Grace"
    assert out["opened"]["f"] == {"peer": "a" * 32, "call": "in"}
    for name in ("wrongKey", "wrongRoom", "tampered", "stale", "garbage"):
        assert out[name] is None, name
    assert out["names"] == ["Grace Hopper", 40]
    assert out["rejected"] is True


def test_transcription_runs_only_on_the_device():
    out = node(TRANSCRIBE_HARNESS, str(PUBLIC / "call-transcribe.js"))
    assert out["status"] == ["available", "downloadable", "available", "unsupported", "unsupported"]
    assert out["install"] == [True, False]
    assert out["beforeStart"] == 0 and out["started"] is True
    assert out["self"] == {"local": True, "lang": "en-US", "continuous": True, "interim": True,
                           "started": "mic"}
    assert out["peerStarted"] == {"id": "track-1"}
    assert [(line["speaker"], line["text"], line["at"]) for line in out["lines"]] == [
        ("Alice", "hello there", 1000), ("Grace", "hi Alice", 2000)]
    assert out["live"] == [{"speaker": "Alice", "text": "and"}]
    assert out["restartQueued"] == 1 and out["restarted"] == 3
    assert "Meeting\n\n[" in out["text"] and "] Alice: hello there\n" in out["text"]
    assert out["running"] is False
    assert out["legacy"] == {"made": 1, "mode": "ondevice-only", "remote": False}
    assert out["cloudStarted"] is False
    assert out["file"] == "quick-chat-transcript-2026-11-03-0905.txt"
    assert out["language"] == ["fr-FR", "en-US"]


def test_the_room_page_never_joins_unencrypted_and_talks_to_the_booking_api():
    page = (PUBLIC / "call.html").read_text(encoding="utf-8")
    script = (PUBLIC / "call-room.js").read_text(encoding="utf-8")
    assert '<script type="module" src="/call-room.js?v=${version}"></script>' in page
    assert '<meta name="referrer" content="no-referrer" />' in page
    assert 'const API = "/api/booking/-/call/";' in script
    assert booking._route("/api/booking/-/call/" + ROOM + "/signal") == ("room", ROOM, "signal")
    assert booking._route("/api/booking/-/call/" + ROOM + "/ticket") == ("room", ROOM, "ticket")
    assert "fmm: { enabled: false }" in script
    assert "!encryption" in script and "groupCallEncryptionMode(globalThis)" in script
    assert 'credentials: "omit"' in script
    assert "location.hash" in script and "fetch(" in script
    assert not re.search(r"fetch\([^)]*hash", script)


def test_two_people_with_the_link_swap_frame_keys_and_a_stranger_gets_none():
    out = node(EXCHANGE_HARNESS, str(PUBLIC / "chat-group-calls.js"), str(PUBLIC / "call-meeting-crypto.js"))
    assert out["anaHasBen"] is True and out["benHasAna"] is True
    assert out["eveHasAny"] == 0 and out["anaHasEve"] == 0
    assert out["anaSaw"] == ["Ben Okafor"]
    assert out["roster"] == ["ben okafor"]
    assert out["status"]["frame"] is None and out["status"]["transcribing"] is True
    assert out["status"]["name"] == "Cy" and out["status"]["microphone"] is False
    assert out["leaving"] is True and out["noPeer"] is None
