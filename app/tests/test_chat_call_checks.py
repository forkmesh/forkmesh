"""Pre-call check, ringing, incoming alerts, call quality and the direct/relay note on /chat."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
CALLS = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")
HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
STUDIO = (PUBLIC / "call-studio.js").read_text(encoding="utf-8")
NATIVE_PAGES = [
    ROOT.parent / "native" / "ios" / "ForkMesh" / "Features" / "FaceChanger" / "direct-call.html",
    ROOT.parent / "native" / "android" / "app" / "src" / "main" / "assets" / "direct-call.html",
]

QUALITY_HARNESS = r'''
import { pathToFileURL } from "node:url";
const q = await import(pathToFileURL(process.argv[2]).href);
const out = {};
const sdp = ["v=0", "m=audio 9 UDP/TLS/RTP/SAVPF 111 0", "a=rtpmap:111 opus/48000/2", "a=fmtp:111 minptime=10;useinbandfec=0", "a=rtpmap:0 PCMU/8000", ""].join("\r\n");
out.tuned = q.tuneCallSdp({ type: "offer", sdp }).sdp.split("\r\n").find((l) => l.startsWith("a=fmtp:111"));
const bare = ["v=0", "a=rtpmap:109 opus/48000/2", "a=rtpmap:0 PCMU/8000", ""].join("\r\n");
out.inserted = q.tuneCallSdp({ type: "answer", sdp: bare }).sdp.split("\r\n");
out.noSdp = q.tuneCallSdp({});
out.constraints = [q.callMediaConstraints("video"), q.callMediaConstraints("audio")];
function stats(entries) {
  return { forEach(fn) { for (const e of entries) fn(e); } };
}
const direct = { getStats: async () => stats([
  { id: "T", type: "transport", selectedCandidatePairId: "P" },
  { id: "P", type: "candidate-pair", localCandidateId: "L", remoteCandidateId: "R", state: "succeeded" },
  { id: "L", type: "local-candidate", candidateType: "srflx", protocol: "udp" },
  { id: "R", type: "remote-candidate", candidateType: "host" },
]) };
const relay = { getStats: async () => stats([
  { id: "P", type: "candidate-pair", localCandidateId: "L", remoteCandidateId: "R", state: "succeeded", nominated: true },
  { id: "L", type: "local-candidate", candidateType: "relay", protocol: "udp" },
  { id: "R", type: "remote-candidate", candidateType: "srflx" },
]) };
const none = { getStats: async () => stats([]) };
out.routes = [await q.readCallRoute(direct), await q.readCallRoute(relay), await q.readCallRoute(none), await q.readCallRoute(null)];
out.labels = [q.callRouteLabel(out.routes[0]), q.callRouteLabel(out.routes[1]), q.callRouteLabel(null)];
const applied = [];
const sender = (strict) => ({
  getParameters: () => ({ encodings: [{}] }),
  async setParameters(params) {
    if (strict && params.degradationPreference) throw new Error("unsupported");
    applied.push(params);
  },
});
const pc = { getTransceivers: () => [
  { receiver: { track: { kind: "audio" } }, sender: sender(false) },
  { receiver: { track: { kind: "video" } }, sender: sender(true) },
] };
await q.tuneCallSenders(pc);
out.applied = applied;
out.unsentEncodings = await q.tuneCallSender({ getParameters: () => ({ encodings: [] }), setParameters: async () => {} }, "video");
class FakeRecorder {
  static isTypeSupported(type) { return type === "audio/webm;codecs=opus" || type === "video/webm;codecs=vp8,opus"; }
  constructor(stream, options) { this.stream = stream; this.options = options; this.state = "inactive"; this.listeners = {}; FakeRecorder.last = this; }
  addEventListener(type, fn) { this.listeners[type] = fn; }
  start() { this.state = "recording"; }
  stop() { this.state = "inactive"; this.listeners.dataavailable({ data: { size: 3 } }); this.listeners.stop(); }
}
class FakeStream { constructor(tracks) { this.tracks = tracks; } }
class FakeBlob { constructor(parts, options) { this.size = parts.length * 3; this.type = options.type; } }
let clock = 0;
const ticks = [];
let tick = null;
const clip = q.recordTestClip([{ kind: "audio", readyState: "live" }], {
  maxMs: 1000, MediaRecorderImpl: FakeRecorder, MediaStreamImpl: FakeStream, BlobImpl: FakeBlob,
  now: () => clock, setIntervalImpl: (fn) => { tick = fn; return 7; }, clearIntervalImpl() {},
  onTick: (ms, max) => ticks.push([ms, max]),
});
clock = 400; tick();
clock = 1200; tick();
const result = await clip.done;
out.clip = { mime: result.mime, video: result.video, size: result.blob.size, durationMs: result.durationMs, options: FakeRecorder.last.options, ticks };
try {
  q.recordTestClip([], { MediaRecorderImpl: FakeRecorder });
  out.emptyThrows = false;
} catch (_) {
  out.emptyThrows = true;
}
console.log(JSON.stringify(out));
'''

TONES_HARNESS = r'''
import { pathToFileURL } from "node:url";
const { createCallTones, CALL_TONES } = await import(pathToFileURL(process.argv[2]).href);
const made = { oscillators: [], gains: [], contexts: 0, resumed: 0, closed: 0 };
class FakeContext {
  constructor() { made.contexts += 1; this.state = "suspended"; this.currentTime = 10; this.destination = {}; }
  resume() { made.resumed += 1; this.state = "running"; return Promise.resolve(); }
  close() { made.closed += 1; return Promise.resolve(); }
  createGain() {
    const gain = { connected: true, gain: { setValueAtTime() {}, linearRampToValueAtTime() {} }, connect() {}, disconnect() { this.connected = false; } };
    made.gains.push(gain);
    return gain;
  }
  createOscillator() {
    const osc = { frequency: { value: 0 }, connect() {}, start(at) { this.at = at; }, stop(at) { this.until = at; } };
    made.oscillators.push(osc);
    return osc;
  }
}
let intervals = [];
const tones = createCallTones({ AudioContextImpl: FakeContext, setIntervalImpl: (fn, ms) => { intervals.push(ms); return intervals.length; }, clearIntervalImpl() {} });
const out = {};
out.ringback = tones.ringback();
out.again = tones.ringback();
out.playing = tones.playing();
out.ringbackFreqs = made.oscillators.map((o) => o.frequency.value);
out.ringbackLength = made.oscillators[0].until - made.oscillators[0].at;
out.intervals = [...intervals];
tones.ring();
out.ringPlaying = tones.playing();
out.firstGainDisconnected = !made.gains[0].connected;
out.ringFreqs = [...new Set(made.oscillators.slice(2).map((o) => o.frequency.value))];
tones.stop();
out.afterStop = tones.playing();
out.allDisconnected = made.gains.every((g) => !g.connected);
out.resumed = made.resumed > 0;
tones.close();
out.closed = made.closed;
out.contexts = made.contexts;
const silent = createCallTones({ AudioContextImpl: undefined });
out.silent = silent.ringback();
out.patterns = Object.keys(CALL_TONES);
console.log(JSON.stringify(out));
'''

CALLS_HARNESS = r'''
import { pathToFileURL } from "node:url";
const { createChatCalls } = await import(pathToFileURL(process.argv[2]).href);
class FakeStream {
  constructor(tracks = []) { this.tracks = [...tracks]; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  addTrack(t) { this.tracks.push(t); }
}
globalThis.MediaStream = FakeStream;
const track = (kind, id) => ({ kind, id, enabled: true, stopped: false, readyState: "live", stop() { this.stopped = true; } });
function element(id) {
  const listeners = {};
  const attributes = {};
  return {
    id, hidden: true, textContent: "", value: "", options: [], children: [], srcObject: null, style: {}, src: "",
    played: 0,
    classList: { on: new Set(), toggle(name, on) { if (on) this.on.add(name); else this.on.delete(name); } },
    setAttribute(name, value) { attributes[name] = String(value); },
    getAttribute(name) { return attributes[name] ?? null; },
    removeAttribute(name) { delete attributes[name]; if (name === "src") this.src = ""; },
    addEventListener(type, fn) { listeners[type] = fn; },
    append(...items) { this.options.push(...items); this.children.push(...items); },
    remove() {},
    pause() {},
    play() { this.played += 1; return Promise.resolve(); },
    querySelector() { return null; },
    querySelectorAll() { return this.children; },
    click() { return listeners.click?.(); },
  };
}
const elements = {};
const documentRef = {
  title: "Chat",
  hidden: true,
  hasFocus: () => false,
  querySelector(selector) {
    const id = selector.replace(/^#/, "");
    elements[id] ||= element(id);
    return elements[id];
  },
  createElement(tag) { return element(tag); },
};
let mediaCalls = 0;
const constraints = [];
const mic = track("audio", "mic");
const camera = track("video", "camera");
const sent = [];
const tones = { now: "", log: [], ring() { this.now = "ring"; this.log.push("ring"); }, ringback() { this.now = "ringback"; this.log.push("ringback"); }, stop() { this.now = ""; } };
const notifications = [];
class FakeNotification {
  constructor(title, options) { this.title = title; this.options = options; this.closed = false; notifications.push(this); }
  close() { this.closed = true; }
}
FakeNotification.permission = "granted";
const vibrations = [];
const clips = [];
function recordTestClipImpl(tracks, { maxMs, onTick }) {
  let settle;
  const done = new Promise((resolve) => { settle = resolve; });
  const clip = { tracks, maxMs, onTick, stopped: false, done, stop() { this.stopped = true; settle({ blob: { size: 9 }, mime: "audio/webm", video: false, durationMs: 1000 }); return done; } };
  clips.push(clip);
  return clip;
}
const intervals = [];
class FakePeer {
  constructor(config) { this.config = config; this.transceivers = []; this.connectionState = "new"; }
  addTransceiver(value) {
    const kind = typeof value === "string" ? value : value.kind;
    this.transceivers.push({ sender: { track: typeof value === "string" ? null : value, async replaceTrack(next) { this.track = next; } }, receiver: { track: { kind } } });
  }
  getTransceivers() { return this.transceivers; }
  async setLocalDescription() { this.localDescription = { type: "offer", sdp: "v=0\r\na=rtpmap:111 opus/48000/2\r\n" }; }
  async setRemoteDescription(d) { this.remoteDescription = d; }
  close() {}
}
const peers = [];
const calls = createChatCalls({
  selfId: "me",
  sendSignal: async (channel, type, extra) => { sent.push({ type, extra }); return true; },
  getUserMediaImpl: async (c) => { mediaCalls += 1; constraints.push(c); return new FakeStream(c.video ? [mic, camera] : [mic]); },
  RTCPeerConnectionImpl: class extends FakePeer { constructor(c) { super(c); peers.push(this); } },
  documentRef,
  storage: { getItem: () => null, setItem() {}, removeItem() {} },
  facesSupported: () => false,
  backgroundsSupported: () => false,
  voicesSupported: () => false,
  createVoicePreviewImpl: async ({ micStream }) => ({ stream: micStream, supported: false, headphones: false,
    choice: () => "off", voice: () => "", monitoring: () => false, setFace() {}, setChoice() {}, monitor() { return false; },
    track: () => micStream.getAudioTracks()[0], stop() {} }),
  setTimeoutImpl: () => 1, clearTimeoutImpl() {},
  setIntervalImpl: (fn, ms) => { intervals.push({ fn, ms }); return intervals.length; }, clearIntervalImpl() {},
  tones,
  NotificationImpl: FakeNotification,
  navigatorRef: { vibrate: (p) => { vibrations.push(p); return true; } },
  recordTestClipImpl,
  readRouteImpl: async () => ({ route: "direct", local: "srflx", remote: "host", protocol: "udp" }),
  URLImpl: { createObjectURL: () => "blob:clip", revokeObjectURL() {} },
});
const tick = () => new Promise((r) => setImmediate(r));
const out = {};
await calls.preview("dm:bob", "bob", "audio");
await tick();
out.preview = {
  previewing: calls.previewing(),
  startLabel: elements["chat-call-start"].textContent,
  startShown: !elements["chat-call-start"].hidden,
  testShown: !elements["chat-call-test-row"].hidden,
  testLabel: elements["chat-call-test"].textContent,
  constraints: constraints[0],
  localHidden: elements["chat-call-local"].hidden,
  tones: tones.now,
  invites: sent.filter((s) => s.type === "call-invite").length,
};
const recording = calls.recordTest();
await tick();
out.recordingLabel = elements["chat-call-test"].textContent;
clips[0].onTick(4000, 10000);
out.countdown = elements["chat-call-test"].textContent;
out.clipTracks = clips[0].tracks.map((t) => t.id);
elements["chat-call-test"].click();
out.recorded = await recording;
out.player = { shown: !elements["chat-call-test-player"].hidden, src: elements["chat-call-test-player"].src,
  played: elements["chat-call-test-player"].played, audio: elements["chat-call-test-player"].classList.on.has("is-audio"),
  label: elements["chat-call-test"].textContent };
elements["chat-call-start"].click();
await tick();
out.started = {
  previewing: calls.previewing(), active: calls.active(), mediaCalls,
  invites: sent.filter((s) => s.type === "call-invite").length,
  media: sent.find((s) => s.type === "call-invite")?.extra?.media,
  tones: tones.now, playerHidden: elements["chat-call-test-player"].hidden,
};
const callId = sent.find((s) => s.type === "call-invite").extra.call;
await calls.handleFrame({ type: "call-accept", call: callId, senderId: "bob-device" }, "dm:bob");
await tick();
out.accepted = { tones: tones.now, offerTuned: sent.find((s) => s.type === "call-offer")?.extra?.sdp?.sdp.includes("useinbandfec=1") };
const pc = peers[0];
pc.connectionState = "connected";
pc.onconnectionstatechange();
await tick();
await tick();
out.live = { state: elements["chat-call-state"].textContent, route: elements["chat-call-route"].textContent,
  routeShown: !elements["chat-call-route"].hidden, routeKind: elements["chat-call-route"].getAttribute("data-route"),
  routeTimer: intervals.some((i) => i.ms === 4000), apiRoute: calls.route()?.route };
calls.hangUp();
out.afterHangUp = { tones: tones.now, routeShown: !elements["chat-call-route"].hidden };
await calls.handleFrame({ type: "call-invite", call: "in-1", senderId: "carol-device", sender: "carol", media: "video" }, "dm:carol");
out.incoming = { shown: !elements["chat-call-incoming"].hidden, kind: elements["chat-call-incoming-kind"].textContent,
  name: elements["chat-call-incoming-label"].textContent, tones: tones.now, notifications: notifications.length,
  title: notifications[0]?.title, body: notifications[0]?.options.body, sticky: notifications[0]?.options.requireInteraction,
  vibrated: vibrations.length > 0 };
await calls.handleFrame({ type: "call-invite", call: "in-1", senderId: "carol-device", sender: "carol", media: "video" }, "dm:carol");
out.resent = { notifications: notifications.length };
const flash = intervals.find((i) => i.ms === 1000);
flash.fn();
out.flashTitle = documentRef.title;
elements["chat-call-decline"].click();
out.declined = { hidden: elements["chat-call-incoming"].hidden, tones: tones.now, closed: notifications[0].closed,
  title: documentRef.title, stoppedVibration: vibrations.at(-1) === 0, decline: sent.some((s) => s.type === "call-decline") };
console.log(JSON.stringify(out));
'''


def _node(harness, *args):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", *map(str, args)],
        input=harness, capture_output=True, text=True, timeout=120, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_quality_helpers_tune_opus_bitrates_and_read_the_route():
    data = _node(QUALITY_HARNESS, PUBLIC / "chat-call-quality.js")
    assert data["tuned"] == "a=fmtp:111 minptime=10;useinbandfec=1;usedtx=0;stereo=0;maxplaybackrate=48000;maxaveragebitrate=96000"
    assert data["inserted"][2] == "a=fmtp:109 useinbandfec=1;usedtx=0;stereo=0;maxplaybackrate=48000;maxaveragebitrate=96000"
    assert data["noSdp"] == {}
    video, audio = data["constraints"]
    assert video["video"]["width"] == {"ideal": 1280}
    assert video["video"]["height"] == {"ideal": 720}
    assert video["video"]["frameRate"] == {"ideal": 30, "max": 30}
    assert audio["video"] is False
    assert audio["audio"]["echoCancellation"] and audio["audio"]["noiseSuppression"] and audio["audio"]["autoGainControl"]
    assert audio["audio"]["sampleRate"] == 48000
    direct, relay, none, missing = data["routes"]
    assert direct == {"route": "direct", "local": "srflx", "remote": "host", "protocol": "udp"}
    assert relay["route"] == "relay"
    assert none is None and missing is None
    assert data["labels"] == [
        "Direct · peer-to-peer, no server in between",
        "Relayed · through a TURN relay server (third party)",
        "",
    ]
    audio_params, video_params = data["applied"]
    assert audio_params["encodings"][0]["maxBitrate"] == 96000
    assert audio_params["encodings"][0]["priority"] == "high"
    assert video_params["encodings"][0]["maxBitrate"] == 2500000
    assert video_params["encodings"][0]["maxFramerate"] == 30
    assert "degradationPreference" not in video_params
    assert data["unsentEncodings"] is False
    clip = data["clip"]
    assert clip["mime"] == "audio/webm;codecs=opus"
    assert clip["video"] is False
    assert clip["size"] == 3
    assert clip["durationMs"] == 1000
    assert clip["options"] == {"audioBitsPerSecond": 128000, "mimeType": "audio/webm;codecs=opus"}
    assert clip["ticks"] == [[400, 1000], [1000, 1000]]
    assert data["emptyThrows"] is True


def test_call_tones_play_ringback_and_ring_and_stop_at_once():
    data = _node(TONES_HARNESS, PUBLIC / "chat-call-tones.js")
    assert data["ringback"] is True and data["again"] is True
    assert data["playing"] == "ringback"
    assert data["ringbackFreqs"] == [440, 480]
    assert data["ringbackLength"] == 2
    assert data["intervals"] == [6000]
    assert data["ringPlaying"] == "ring"
    assert data["firstGainDisconnected"] is True
    assert data["ringFreqs"] == [400, 450]
    assert data["afterStop"] == ""
    assert data["allDisconnected"] is True
    assert data["resumed"] is True
    assert data["closed"] == 1 and data["contexts"] == 1
    assert data["silent"] is False
    assert data["patterns"] == ["ringback", "ring"]


def test_pre_call_check_records_a_test_clip_then_rings_with_ringback_and_reports_the_route():
    data = _node(CALLS_HARNESS, PUBLIC / "chat-calls.js")
    preview = data["preview"]
    assert preview["previewing"] is True
    assert preview["startLabel"] == "Start voice call"
    assert preview["startShown"] and preview["testShown"]
    assert preview["testLabel"] == "Record a 10-second test clip"
    assert preview["constraints"]["video"] is False
    assert preview["constraints"]["audio"]["autoGainControl"] is True
    assert preview["localHidden"] is True
    assert preview["tones"] == ""
    assert preview["invites"] == 0
    assert data["recordingLabel"] == "Stop test (10s left)"
    assert data["countdown"] == "Stop test (6s left)"
    assert data["clipTracks"] == ["mic"]
    assert data["recorded"] is True
    assert data["player"] == {"shown": True, "src": "blob:clip", "played": 1, "audio": True, "label": "Record again"}
    started = data["started"]
    assert started["previewing"] is False and started["active"] is True
    assert started["mediaCalls"] == 1
    assert started["invites"] == 1 and started["media"] == "audio"
    assert started["tones"] == "ringback"
    assert started["playerHidden"] is True
    assert data["accepted"] == {"tones": "", "offerTuned": True}
    live = data["live"]
    assert live["state"] == "Connected · end-to-end encrypted"
    assert live["route"] == "Direct · peer-to-peer, no server in between"
    assert live["routeShown"] is True and live["routeKind"] == "direct"
    assert live["routeTimer"] is True and live["apiRoute"] == "direct"
    assert data["afterHangUp"] == {"tones": "", "routeShown": False}


def test_incoming_call_rings_vibrates_notifies_once_and_stops_on_decline():
    data = _node(CALLS_HARNESS, PUBLIC / "chat-calls.js")
    incoming = data["incoming"]
    assert incoming["shown"] is True
    assert incoming["kind"] == "Incoming video call"
    assert incoming["name"] == "@carol"
    assert incoming["tones"] == "ring"
    assert incoming["notifications"] == 1
    assert incoming["title"] == "Incoming video call"
    assert incoming["body"] == "@carol is calling you"
    assert incoming["sticky"] is True
    assert incoming["vibrated"] is True
    assert data["resent"] == {"notifications": 1}
    assert data["flashTitle"] == "📞 @carol is calling you"
    declined = data["declined"]
    assert declined["hidden"] is True
    assert declined["tones"] == ""
    assert declined["closed"] is True
    assert declined["title"] == "Chat"
    assert declined["stoppedVibration"] is True
    assert declined["decline"] is True


def test_chat_page_has_the_pre_call_check_route_note_and_full_screen_incoming_call():
    for marker in (
        'id="chat-call-route"', 'id="chat-call-test-row"', 'id="chat-call-test"',
        'id="chat-call-test-player"', 'id="chat-call-incoming-kind"',
        'class="chat-call-incoming-accept"', 'class="chat-call-incoming-decline"',
    ):
        assert marker in HTML
    assert ".chat-call-incoming { position: fixed; inset: 0;" in CSS
    assert ".chat-call-test-player.is-audio" in CSS
    assert "requestStart: (media) => startDirectCall(media)," in CHAT
    assert 'from "./chat-call-quality.js"' in CALLS
    assert 'from "./chat-call-tones.js"' in CALLS
    assert "askCallNotifications(NotificationImpl);" in CALLS
    assert 'from "./chat-call-quality.js"' in STUDIO


def test_native_call_pages_share_quality_route_and_audio_test_clips():
    first, second = (path.read_text(encoding="utf-8") for path in NATIVE_PAGES)
    assert first == second
    for marker in (
        'const VIDEO_CONSTRAINTS = { facingMode: "user", width: { ideal: 1280 }, height: { ideal: 720 }, frameRate: { ideal: 30, max: 30 } };',
        "const VIDEO_MAX_BITRATE = 2500000;",
        "const AUDIO_MAX_BITRATE = 96000;",
        "return tuneSdp(plain(pc.localDescription));",
        'post({ event: "route", ...found });',
        'route: local === "relay" || remote === "relay" ? "relay" : "direct",',
        "async route() {",
        "const recorded = [video ? outgoingVideo() : null, outgoingAudio()].filter(Boolean);",
        'mime: mimeType || (video ? "video/mp4" : "audio/mp4"),',
        "stopRoute();",
        '"video/mp4;codecs=avc1.42E028,mp4a.40.2",',
    ):
        assert marker in first
    assert "videoBitsPerSecond: 900000" not in first
    assert "avc1.42E01E" not in first
    assert "avc1.42E01E" not in (PUBLIC / "chat-call-quality.js").read_text(encoding="utf-8")
