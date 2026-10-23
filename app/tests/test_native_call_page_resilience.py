import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT.parent / "native"
IOS_PAGE = NATIVE / "ios" / "ForkMesh" / "Features" / "FaceChanger" / "direct-call.html"
ANDROID_PAGE = NATIVE / "android" / "app" / "src" / "main" / "assets" / "direct-call.html"

HARNESS = r'''
import { readFileSync } from "node:fs";
const page = readFileSync(process.argv[2], "utf8");
const glue = page.slice(page.lastIndexOf("<script>\n") + 9, page.lastIndexOf("</script>"));

let now = 0;
const timers = [];
let timerSeq = 0;
const schedule = (fn, ms, repeat) => {
  const id = ++timerSeq;
  timers.push({ id, fn, at: now + Math.max(0, ms || 0), every: repeat ? Math.max(1, ms || 0) : 0 });
  return id;
};
const cancel = (id) => {
  const at = timers.findIndex((timer) => timer.id === id);
  if (at >= 0) timers.splice(at, 1);
};
globalThis.setTimeout = (fn, ms) => schedule(fn, ms, false);
globalThis.setInterval = (fn, ms) => schedule(fn, ms, true);
globalThis.clearTimeout = cancel;
globalThis.clearInterval = cancel;
Object.defineProperty(globalThis, "performance", { value: { now: () => now }, configurable: true });
const flush = async () => { for (let i = 0; i < 20; i += 1) await new Promise((resolve) => setImmediate(resolve)); };
const advance = async (ms) => {
  const end = now + ms;
  for (;;) {
    timers.sort((a, b) => a.at - b.at);
    const next = timers[0];
    if (!next || next.at > end) break;
    now = next.at;
    if (next.every) next.at += next.every;
    else timers.shift();
    next.fn();
    await flush();
  }
  now = end;
  await flush();
};

const posts = [];
globalThis.window = globalThis;
window.webkit = { messageHandlers: { fmCall: { postMessage: (text) => posts.push(JSON.parse(text)) } } };
const classes = new Set();
const body = {
  get className() { return [...classes].join(" "); },
  set className(value) { classes.clear(); for (const name of String(value).split(/\s+/).filter(Boolean)) classes.add(name); },
  classList: {
    add: (name) => classes.add(name),
    remove: (name) => classes.delete(name),
    contains: (name) => classes.has(name),
    toggle: (name, on) => { const want = on === undefined ? !classes.has(name) : Boolean(on); if (want) classes.add(name); else classes.delete(name); return want; },
  },
};
const video = () => ({ srcObject: null, play: () => Promise.resolve() });
const elements = { remote: video(), local: video() };
globalThis.document = { body, getElementById: (id) => elements[id] || null };

let trackSeq = 0;
class Track {
  constructor(kind) { this.kind = kind; this.id = kind + "-" + (++trackSeq); this.enabled = true; this.readyState = "live"; this.listeners = {}; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  stop() { this.readyState = "ended"; }
  end() { this.readyState = "ended"; for (const fn of this.listeners.ended || []) fn(); }
}
class Stream {
  constructor(tracks = []) { this.tracks = [...tracks]; }
  getTracks() { return [...this.tracks]; }
  getAudioTracks() { return this.tracks.filter((track) => track.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((track) => track.kind === "video"); }
  addTrack(track) { this.tracks.push(track); }
  removeTrack(track) { this.tracks = this.tracks.filter((item) => item !== track); }
}
globalThis.MediaStream = Stream;
const asked = [];
let micBroken = false;
Object.defineProperty(globalThis, "navigator", { value: { mediaDevices: {
  getUserMedia: async (constraints) => {
    asked.push(constraints);
    if (micBroken && constraints.audio && !constraints.video) throw new Error("NotReadableError");
    const tracks = [];
    if (constraints.audio) tracks.push(new Track("audio"));
    if (constraints.video) tracks.push(new Track("video"));
    return new Stream(tracks);
  },
} }, configurable: true });
globalThis.RTCRtpReceiver = { getCapabilities: () => ({ codecs: [
  { mimeType: "audio/opus" }, { mimeType: "audio/red" }, { mimeType: "audio/G722" }, { mimeType: "audio/telephone-event" },
] }) };
const connections = [];
class Sender {
  constructor(track) { this.track = track; this.params = { encodings: [{}] }; }
  replaceTrack(track) { this.track = track; return Promise.resolve(); }
  getParameters() { return JSON.parse(JSON.stringify(this.params)); }
  setParameters(params) { this.params = params; return Promise.resolve(); }
}
class Connection {
  constructor(config) {
    this.config = config; this.senders = []; this.transceivers = []; this.connectionState = "new"; this.signalingState = "stable";
    this.restarts = 0; this.offers = []; this.stats = new Map(); connections.push(this);
  }
  addTrack(track) {
    const sender = new Sender(track);
    this.senders.push(sender);
    this.transceivers.push({ sender, setCodecPreferences: (codecs) => { this.preferred = codecs.map((codec) => codec.mimeType); } });
    return sender;
  }
  getTransceivers() { return this.transceivers; }
  restartIce() { this.restarts += 1; }
  async createOffer(options) { this.offers.push(options || null); return { type: "offer", sdp: "v=0\r\na=ice-ufrag:u" + this.offers.length + "\r\na=rtpmap:111 opus/48000/2\r\n" }; }
  async createAnswer() { return { type: "answer", sdp: "v=0\r\na=rtpmap:111 opus/48000/2\r\n" }; }
  async setLocalDescription(description) { this.localDescription = description; this.signalingState = description.type === "offer" ? "have-local-offer" : "stable"; }
  async setRemoteDescription(description) {
    if (description.type === "answer" && this.signalingState !== "have-local-offer") throw new Error("InvalidStateError");
    this.remoteDescription = description;
    this.signalingState = description.type === "offer" ? "have-remote-offer" : "stable";
  }
  async addIceCandidate() {}
  async getStats() { return new Map(this.stats); }
  close() { this.connectionState = "closed"; }
  set(state) { this.connectionState = state; this.onconnectionstatechange && this.onconnectionstatechange(); }
}
globalThis.RTCPeerConnection = Connection;

new Function(glue)();
let callSeq = 0;
const call = async (name, args = []) => {
  const id = ++callSeq;
  window.fmCall(id, name, args);
  await flush();
  const result = posts.find((post) => post.event === "result" && post.id === id);
  if (!result) throw new Error("no result for " + name);
  if (!result.ok) throw new Error(name + ": " + result.error);
  return result.value;
};
const events = (event) => posts.filter((post) => post.event === event);
const out = {};

await call("setup", ["video", ["stun:stun.example"], "", ""]);
let pc = connections.at(-1);
out.plain = { body: body.className, preferred: pc.preferred || null };
await advance(1000);
const audioSender = pc.senders[0];
const videoSender = pc.senders[1];
pc.set("connected");
await flush();
out.plain.priorities = [audioSender.params.encodings[0].priority, audioSender.params.encodings[0].networkPriority,
  videoSender.params.encodings[0].priority, videoSender.params.encodings[0].networkPriority];
out.plain.bitrates = [audioSender.params.encodings[0].maxBitrate, videoSender.params.encodings[0].maxBitrate];
pc.set("failed");
await flush();
out.plain.closedOnFailed = events("closed").length;
out.plain.connectionEvents = events("connection").length;
pc.set("disconnected");
await advance(8000);
out.plain.closedAfterGrace = events("closed").length;
await call("close");

posts.length = 0;
await call("setup", ["video", ["stun:stun.example"], "", "", "", "", null, { resilient: true, staged: true }]);
pc = connections.at(-1);
out.resilient = { body: body.className, preferred: pc.preferred };
pc.set("connected");
await flush();
out.resilient.live = events("live").length;
out.resilient.connected = events("connection").map((event) => event.state);
pc.set("disconnected");
await advance(10000);
pc.set("failed");
await flush();
out.resilient.states = events("connection").map((event) => event.state);
out.resilient.closed = events("closed").length;
const restarted = await call("restart");
out.resilient.restart = { restarts: pc.restarts, options: pc.offers.at(-1), sdp: restarted.sdp, type: restarted.type };
const answer = await call("accept", [{ type: "answer", sdp: "v=0\r\n" }]);
let duplicate = null;
try { await call("accept", [{ type: "answer", sdp: "v=0\r\n" }]); } catch (error) { duplicate = String(error.message); }
out.resilient.answers = { first: answer, duplicate };

posts.length = 0;
pc.set("connected");
await flush();
pc.stats.set("in-audio", { id: "in-audio", type: "inbound-rtp", kind: "audio", packetsReceived: 100 });
pc.stats.set("in-video", { id: "in-video", type: "inbound-rtp", kind: "video", framesDecoded: 0 });
await advance(1000);
out.watch = { waiting: body.classList.contains("waiting") };
pc.stats.set("in-video", { id: "in-video", type: "inbound-rtp", kind: "video", framesDecoded: 30 });
await advance(1000);
out.watch.seen = events("remote-video").map((event) => [event.on, event.seen]);
out.watch.afterFirstFrame = body.className;
await advance(5000);
out.watch.stalled = events("connection").map((event) => event.state);
out.watch.videoOff = events("remote-video").map((event) => [event.on, event.seen]);
out.watch.offBody = body.className;
pc.stats.set("in-audio", { id: "in-audio", type: "inbound-rtp", kind: "audio", packetsReceived: 150 });
pc.stats.set("in-video", { id: "in-video", type: "inbound-rtp", kind: "video", framesDecoded: 90 });
await advance(1000);
out.watch.recovered = events("connection").map((event) => event.state);
out.watch.videoBack = events("remote-video").map((event) => [event.on, event.seen]);
await advance(6000);
out.watch.stalledAgain = events("connection").map((event) => event.state).length;

posts.length = 0;
await call("mute", [true]);
const oldMic = pc.senders[0].track;
oldMic.end();
await flush();
const newMic = pc.senders[0].track;
out.mic = { replaced: newMic !== oldMic, kind: newMic.kind, enabled: newMic.enabled, reported: events("mic").map((event) => event.ok),
  asked: asked.at(-1) };
micBroken = true;
newMic.end();
await advance(3000);
out.mic.broken = events("mic").map((event) => event.ok);
micBroken = false;
await call("wake");
out.mic.woken = { reported: events("mic").map((event) => event.ok), live: pc.senders[0].track.readyState };
await call("layout", ["mini"]);
out.layout = { mini: body.className };
await call("layout", ["full"]);
out.layout.full = body.className;
await call("close");
const before = posts.length;
pc.stats.set("in-audio", { id: "in-audio", type: "inbound-rtp", kind: "audio", packetsReceived: 150 });
await advance(10000);
out.closed = { quiet: posts.length === before, timers: timers.length };
console.log(JSON.stringify(out));
'''


def _run(page):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(page)],
        input=HARNESS, capture_output=True, text=True, timeout=120, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


@pytest.fixture(scope="module")
def run():
    return _run(IOS_PAGE)


def test_both_phone_pages_are_the_same_file():
    assert IOS_PAGE.read_bytes() == ANDROID_PAGE.read_bytes()


def test_a_host_that_does_not_opt_in_keeps_the_old_self_close(run):
    plain = run["plain"]
    assert plain["body"] == "video"
    assert plain["closedOnFailed"] == 1
    assert plain["closedAfterGrace"] == 2
    assert plain["connectionEvents"] == 0


def test_audio_keeps_priority_and_red_leads_the_audio_codecs(run):
    assert run["plain"]["priorities"] == ["high", "high", "medium", "medium"]
    assert run["plain"]["preferred"] == ["audio/red", "audio/opus", "audio/G722", "audio/telephone-event"]
    assert run["resilient"]["preferred"][:2] == ["audio/red", "audio/opus"]


def test_the_ios_host_gets_connection_states_instead_of_a_close(run):
    resilient = run["resilient"]
    assert resilient["body"] == "video staged waiting"
    assert resilient["live"] == 1
    assert resilient["connected"] == ["connected"]
    assert resilient["states"] == ["connected", "disconnected", "failed"]
    assert resilient["closed"] == 0


def test_restart_makes_an_ice_restart_offer_and_a_duplicate_answer_fails_harmlessly(run):
    restart = run["resilient"]["restart"]
    assert restart["restarts"] == 1
    assert restart["options"] == {"iceRestart": True}
    assert restart["type"] == "offer"
    assert "a=ice-ufrag:u1" in restart["sdp"]
    answers = run["resilient"]["answers"]
    assert answers["first"] is True
    assert "InvalidStateError" in answers["duplicate"]


def test_silent_inbound_audio_is_reported_as_a_stall_and_its_end_as_connected(run):
    watch = run["watch"]
    assert watch["waiting"] is True
    assert watch["seen"] == [[True, True]]
    assert "waiting" not in watch["afterFirstFrame"].split()
    assert watch["stalled"] == ["connected", "stalled"]
    assert watch["recovered"] == ["connected", "stalled", "connected"]
    assert watch["stalledAgain"] == 4


def test_the_remote_video_switches_to_the_avatar_when_it_stops(run):
    watch = run["watch"]
    assert watch["videoOff"] == [[True, True], [False, True]]
    assert "remote-off" in watch["offBody"].split()
    assert watch["videoBack"][-1] == [True, True]


def test_a_lost_microphone_is_reacquired_and_keeps_the_mute(run):
    mic = run["mic"]
    assert mic["replaced"] is True
    assert mic["kind"] == "audio"
    assert mic["enabled"] is False
    assert mic["reported"] == [True]
    assert mic["asked"]["audio"]["echoCancellation"] is True
    assert mic["asked"].get("video") is None
    assert mic["broken"] == [True, False]
    assert mic["woken"]["reported"] == [True, False, True]
    assert mic["woken"]["live"] == "live"


def test_the_mini_window_hides_the_self_view_and_close_stops_the_watchdog(run):
    assert "mini" in run["layout"]["mini"].split()
    assert "mini" not in run["layout"]["full"].split()
    assert run["closed"]["quiet"] is True
