"""Direct call resilience on /chat: resent accepts and offers, duplicate frames, ICE restarts, timeouts, mic recovery, the mini window."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CALLS = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")
HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
METER = (PUBLIC / "call-quality-meter.js").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const { createChatCalls } = await import(pathToFileURL(process.argv[2]).href);
class FakeStream {
  constructor(tracks = []) { this.tracks = [...tracks]; }
  getTracks() { return this.tracks; }
  getAudioTracks() { return this.tracks.filter((t) => t.kind === "audio"); }
  getVideoTracks() { return this.tracks.filter((t) => t.kind === "video"); }
  addTrack(t) { this.tracks.push(t); }
  removeTrack(t) { this.tracks = this.tracks.filter((x) => x !== t); }
}
globalThis.MediaStream = FakeStream;
function track(kind, id) {
  const listeners = {};
  return {
    kind, id, enabled: true, readyState: "live", stopped: false,
    stop() { this.stopped = true; this.readyState = "ended"; },
    addEventListener(type, fn) { (listeners[type] ||= []).push(fn); },
    emit(type) { for (const fn of listeners[type] || []) fn({}); },
  };
}
function element(id) {
  const listeners = {};
  const attributes = {};
  return {
    id, hidden: true, textContent: "", value: "", options: [], children: [], srcObject: null, style: {}, dataset: {},
    muted: false, plays: 0, playError: null,
    classList: { on: new Set(), add(n) { this.on.add(n); }, remove(n) { this.on.delete(n); },
      toggle(n, on) { if (on) this.on.add(n); else this.on.delete(n); } },
    setAttribute(n, v) { attributes[n] = String(v); },
    getAttribute(n) { return attributes[n] ?? null; },
    removeAttribute(n) { delete attributes[n]; },
    addEventListener(type, fn) { (listeners[type] ||= []).push(fn); },
    emit(type, event = {}) { for (const fn of listeners[type] || []) fn(event); },
    append(...items) { this.options.push(...items); this.children.push(...items); },
    remove() {}, pause() {},
    play() {
      this.plays += 1;
      const error = this.playError;
      this.playError = null;
      return error ? Promise.reject(error) : Promise.resolve();
    },
    querySelector() { return null; },
    querySelectorAll() { return this.children; },
    click() { this.emit("click", {}); },
    setPointerCapture(pointer) { this.captured = pointer; },
    getBoundingClientRect() {
      return { left: parseFloat(this.style.left) || 0, top: parseFloat(this.style.top) || 0, width: 220, height: 160 };
    },
  };
}
function makeClock() {
  let next = 1;
  const live = new Map();
  const add = (fn, ms, every) => { const id = next++; live.set(id, { fn, ms, every }); return id; };
  return {
    live,
    setTimeoutImpl: (fn, ms) => add(fn, ms, false),
    setIntervalImpl: (fn, ms) => add(fn, ms, true),
    clearTimeoutImpl: (id) => { live.delete(id); },
    clearIntervalImpl: (id) => { live.delete(id); },
    fire(ms) {
      for (const [id, entry] of [...live]) {
        if (entry.ms !== ms || !live.has(id)) continue;
        if (!entry.every) live.delete(id);
        entry.fn();
      }
    },
    count(ms, every) { return [...live.values()].filter((e) => e.ms === ms && e.every === every).length; },
  };
}
class FakePeer {
  constructor(config) {
    this.config = config; this.transceivers = []; this.connectionState = "new"; this.signalingState = "stable";
    this.remoteSets = []; this.restarts = 0; this.n = 0; this.stats = []; this.ice = [];
  }
  addTransceiver(value) {
    const kind = typeof value === "string" ? value : value.kind;
    this.transceivers.push({ direction: "sendrecv", sender: { track: typeof value === "string" ? null : value, async replaceTrack(next) { this.track = next; } }, receiver: { track: { kind } } });
  }
  getTransceivers() { return this.transceivers; }
  restartIce() { this.restarts += 1; }
  async setLocalDescription() {
    this.n += 1;
    const type = this.signalingState === "have-remote-offer" ? "answer" : "offer";
    this.localDescription = { type, sdp: `v=0\r\na=rtpmap:111 opus/48000/2\r\na=ice-ufrag:${type}${this.n}\r\n` };
    this.signalingState = type === "offer" ? "have-local-offer" : "stable";
  }
  async setRemoteDescription(description) {
    this.remoteSets.push(description.sdp);
    this.remoteDescription = description;
    if (description.type === "offer") {
      this.signalingState = "have-remote-offer";
      if (!this.transceivers.length) {
        for (const kind of ["audio", "video"]) {
          this.transceivers.push({ direction: "recvonly", sender: { track: null, async replaceTrack(next) { this.track = next; } }, receiver: { track: { kind } } });
        }
      }
    } else {
      this.signalingState = "stable";
    }
  }
  async addIceCandidate(candidate) { this.ice.push(candidate); }
  async getStats() { const list = this.stats; return { forEach(fn) { list.forEach(fn); } }; }
  close() { this.closed = true; }
}
function makeEnv(self) {
  const env = { sent: [], peers: [], media: [], elements: {}, saved: {}, clock: makeClock(), t: 0, freshMics: [], failNext: false, hold: false };
  const documentRef = {
    title: "Chat", hidden: false, hasFocus: () => true,
    querySelector(selector) {
      const id = selector.replace(/^#/, "");
      env.elements[id] ||= element(id);
      return env.elements[id];
    },
    createElement(tag) { return element(tag); },
  };
  env.mic = track("audio", "mic");
  env.cam = track("video", "cam");
  const getUserMediaImpl = (constraints) => {
    env.media.push(constraints);
    if (env.failNext) { env.failNext = false; return Promise.reject(new Error("gone")); }
    const make = () => {
      const fresh = env.freshMics.shift();
      return new FakeStream(fresh ? [fresh] : constraints.video ? [env.mic, env.cam] : [env.mic]);
    };
    if (env.hold) return new Promise((resolve) => { env.release = () => resolve(make()); });
    return Promise.resolve(make());
  };
  const { clock } = env;
  env.calls = createChatCalls({
    selfId: self,
    sendSignal: async (channel, type, extra) => { env.sent.push({ type, ...extra }); return true; },
    getUserMediaImpl,
    RTCPeerConnectionImpl: class extends FakePeer { constructor(c) { super(c); env.peers.push(this); } },
    documentRef,
    windowRef: { innerWidth: 1000, innerHeight: 800, addEventListener() {} },
    storage: { getItem: (k) => env.saved[k] ?? null, setItem: (k, v) => { env.saved[k] = String(v); }, removeItem: (k) => { delete env.saved[k]; } },
    facesSupported: () => false, backgroundsSupported: () => false, voicesSupported: () => false,
    createVoicePreviewImpl: async ({ micStream }) => ({ stream: micStream, supported: false, headphones: false,
      choice: () => "off", voice: () => "", monitoring: () => false, setFace() {}, setChoice() {}, monitor() { return false; },
      track: () => micStream.getAudioTracks()[0], stop() {} }),
    setTimeoutImpl: clock.setTimeoutImpl, clearTimeoutImpl: clock.clearTimeoutImpl,
    setIntervalImpl: clock.setIntervalImpl, clearIntervalImpl: clock.clearIntervalImpl,
    tones: { ring() {}, ringback() {}, stop() {} },
    NotificationImpl: undefined,
    navigatorRef: {},
    readRouteImpl: async () => null,
    preferAudioCodecsImpl: () => { env.codecPrefs = (env.codecPrefs || 0) + 1; return true; },
    URLImpl: { createObjectURL: () => "blob:x", revokeObjectURL() {} },
    now: () => (env.t += 1000),
  });
  env.text = (id) => env.elements[id]?.textContent || "";
  env.shown = (id) => !env.elements[id]?.hidden;
  env.count = (type) => env.sent.filter((s) => s.type === type).length;
  return env;
}
const flush = async () => { for (let i = 0; i < 30; i += 1) await new Promise((r) => setImmediate(r)); };
const out = {};

{
  const env = makeEnv("me");
  const { calls, clock } = env;
  await calls.start("dm:bob", "bob", "video");
  const callId = env.sent.find((s) => s.type === "call-invite").call;
  const frame = (type, extra = {}) => calls.handleFrame({ type, call: callId, senderId: "bob-1", to: "me", ...extra }, "dm:bob");
  await frame("call-accept");
  await flush();
  const pc = env.peers[0];
  const offers = () => env.sent.filter((s) => s.type === "call-offer");
  const caller = out.caller = {
    firstOffers: offers().length, connectTimer: clock.count(30000, false), codecPrefs: env.codecPrefs,
    state: env.text("chat-call-state"),
  };
  await frame("call-accept");
  caller.dupAccept = offers().length;
  caller.sameSdp = offers()[1].sdp.sdp === offers()[0].sdp.sdp && offers()[1].to === "bob-1";
  await calls.handleFrame({ type: "call-accept", call: callId, senderId: "bob-2", to: "me" }, "dm:bob");
  caller.otherDevice = offers().length;
  for (let i = 0; i < 7; i += 1) clock.fire(3000);
  caller.resent = offers().length;
  caller.offerTimerLeft = clock.count(3000, true);
  await frame("call-ice", { ice: { candidate: "before-answer" } });
  caller.iceQueued = pc.ice.length;
  const answer = { type: "answer", sdp: "v=0\r\nanswer-1\r\n" };
  await frame("call-answer", { sdp: answer });
  caller.iceFlushed = pc.ice.length;
  pc.setRemoteDescription = (() => { const real = pc.setRemoteDescription.bind(pc); return async (d) => { if (pc.signalingState === "stable") throw new Error("wrong state"); return real(d); }; })();
  await frame("call-answer", { sdp: answer });
  await frame("call-answer", { sdp: { type: "answer", sdp: "v=0\r\nanswer-late\r\n" } });
  caller.answersApplied = pc.remoteSets.length;
  caller.stillActive = calls.active();
  pc.connectionState = "connected";
  pc.onconnectionstatechange();
  await flush();
  caller.live = { state: env.text("chat-call-state"), connectTimer: clock.count(30000, false), health: clock.count(1000, true) };
  pc.stats = [{ type: "inbound-rtp", kind: "audio", packetsReceived: 100 }];
  for (let i = 0; i < 5; i += 1) { clock.fire(1000); await flush(); }
  caller.watchdog = { restarts: pc.restarts, offers: offers().length, state: env.text("chat-call-state"), reconnecting: calls.reconnecting(),
    restartLoop: clock.count(6000, true), giveUp: clock.count(30000, false) };
  caller.restartSdpDiffers = offers().at(-1).sdp.sdp !== offers()[0].sdp.sdp;
  await frame("call-answer", { sdp: { type: "answer", sdp: "v=0\r\nanswer-2\r\n" } });
  caller.restartAnswered = pc.remoteSets.length;
  pc.stats = [{ type: "inbound-rtp", kind: "audio", packetsReceived: 150 }];
  clock.fire(1000);
  await flush();
  caller.recovered = { state: env.text("chat-call-state"), reconnecting: calls.reconnecting(),
    restartLoop: clock.count(6000, true), giveUp: clock.count(30000, false) };
  pc.connectionState = "disconnected";
  pc.onconnectionstatechange();
  caller.disconnected = { state: env.text("chat-call-state"), restarts: pc.restarts, grace: clock.count(2000, false) };
  clock.fire(2000);
  await flush();
  caller.afterGrace = { restarts: pc.restarts, offers: offers().length };
  clock.fire(6000);
  await flush();
  caller.again = pc.restarts;
  pc.connectionState = "connected";
  pc.onconnectionstatechange();
  await flush();
  caller.back = { state: env.text("chat-call-state"), restartLoop: clock.count(6000, true), giveUp: clock.count(30000, false) };
  pc.connectionState = "failed";
  pc.onconnectionstatechange();
  await flush();
  caller.failed = { restarts: pc.restarts, active: calls.active(), state: env.text("chat-call-state") };
  clock.fire(30000);
  await flush();
  const panel = env.elements["chat-call-panel"];
  caller.gaveUp = { active: calls.active(), bye: env.count("call-bye"), state: env.text("chat-call-state"),
    notice: env.text("chat-call-notice"), ended: panel.classList.on.has("is-ended"), shown: !panel.hidden,
    hangup: env.text("chat-call-hangup"), back: env.shown("chat-call-minimize"),
    intervals: [...clock.live.values()].filter((e) => e.every).length };
  env.elements["chat-call-hangup"].click();
  caller.closed = { hidden: panel.hidden, ended: panel.classList.on.has("is-ended"), timers: clock.live.size };
}

{
  const env = makeEnv("bob-1");
  const { calls, clock } = env;
  const invite = { type: "call-invite", call: "c1", senderId: "alice-1", sender: "alice", media: "video" };
  await calls.handleFrame(invite, "dm:alice");
  const callee = out.callee = { incoming: env.shown("chat-call-incoming") };
  env.hold = true;
  const accepting = calls.acceptInvite();
  await flush();
  await calls.handleFrame(invite, "dm:alice");
  clock.fire(9000);
  callee.stillRinging = env.shown("chat-call-incoming");
  env.hold = false;
  env.release();
  callee.accepted = await accepting;
  await flush();
  callee.firstAccept = env.count("call-accept");
  callee.acceptTo = env.sent.find((s) => s.type === "call-accept")?.to;
  callee.connectTimer = clock.count(30000, false);
  callee.incomingAfter = env.shown("chat-call-incoming");
  clock.fire(2000);
  clock.fire(2000);
  callee.twoResends = env.count("call-accept");
  for (let i = 0; i < 10; i += 1) clock.fire(2000);
  callee.acceptResends = env.count("call-accept");
  callee.acceptTimer = clock.count(2000, true);
  const frame = (type, extra = {}) => calls.handleFrame({ type, call: "c1", senderId: "alice-1", to: "bob-1", ...extra }, "dm:alice");
  await frame("call-ice", { ice: { candidate: "early" } });
  const offer = { type: "offer", sdp: "v=0\r\na=rtpmap:111 opus/48000/2\r\noffer-1\r\n" };
  await frame("call-offer", { sdp: offer });
  await flush();
  const pc = env.peers[0];
  const answers = () => env.sent.filter((s) => s.type === "call-answer");
  callee.answered = { answers: answers().length, applied: pc.remoteSets.length, iceFlushed: pc.ice.length,
    sending: pc.transceivers.map((t) => t.direction + ":" + (t.sender.track?.id || "")), codecPrefs: env.codecPrefs,
    tuned: answers()[0].sdp.sdp.includes("maxaveragebitrate=128000") };
  await frame("call-offer", { sdp: offer });
  await flush();
  callee.duplicate = { answers: answers().length, applied: pc.remoteSets.length, same: answers()[1].sdp.sdp === answers()[0].sdp.sdp };
  await frame("call-offer", { sdp: { type: "offer", sdp: "v=0\r\na=rtpmap:111 opus/48000/2\r\noffer-2\r\n" } });
  await flush();
  callee.restart = { answers: answers().length, applied: pc.remoteSets.length, fresh: answers()[2].sdp.sdp !== answers()[0].sdp.sdp };
  pc.connectionState = "connected";
  pc.onconnectionstatechange();
  await flush();
  callee.live = { state: env.text("chat-call-state"), connectTimer: clock.count(30000, false) };
  pc.connectionState = "failed";
  pc.onconnectionstatechange();
  await flush();
  callee.failed = { active: calls.active(), state: env.text("chat-call-state"), restarts: pc.restarts,
    giveUp: clock.count(30000, false), offers: env.count("call-offer") };
  pc.connectionState = "connected";
  pc.onconnectionstatechange();
  callee.back = { state: env.text("chat-call-state"), giveUp: clock.count(30000, false) };
  calls.hangUp();
  callee.hungUp = { bye: env.count("call-bye"), timers: clock.live.size };
}

{
  const env = makeEnv("bob-1");
  await env.calls.handleFrame({ type: "call-invite", call: "c2", senderId: "alice-1", sender: "alice", media: "audio" }, "dm:alice");
  await env.calls.acceptInvite();
  await flush();
  env.clock.fire(30000);
  await flush();
  const panel = env.elements["chat-call-panel"];
  out.timeout = { active: env.calls.active(), bye: env.count("call-bye"), state: env.text("chat-call-state"),
    notice: env.text("chat-call-notice"), ended: panel.classList.on.has("is-ended"), shown: !panel.hidden };
  env.clock.fire(8000);
  out.timeout.autoClosed = panel.hidden;
  out.timeout.timers = env.clock.live.size;
}

{
  const env = makeEnv("bob-1");
  await env.calls.handleFrame({ type: "call-invite", call: "c3", senderId: "alice-1", sender: "alice", media: "video" }, "dm:alice");
  env.hold = true;
  const accepting = env.calls.acceptInvite();
  await flush();
  await env.calls.handleFrame({ type: "call-bye", call: "c3", senderId: "alice-1" }, "dm:alice");
  env.release();
  out.byeWhileAccepting = { result: await accepting, active: env.calls.active(), state: env.text("chat-call-state"),
    notice: env.text("chat-call-notice"), shown: env.shown("chat-call-panel"), micStopped: env.mic.stopped,
    incoming: env.shown("chat-call-incoming") };
}

{
  const env = makeEnv("bob-1");
  await env.calls.preview("dm:carol", "carol", "audio");
  await env.calls.handleFrame({ type: "call-invite", call: "c4", senderId: "alice-1", sender: "alice", media: "audio" }, "dm:alice");
  out.inviteDuringPreview = { incoming: env.shown("chat-call-incoming"), busy: env.count("call-decline") };
}

{
  const env = makeEnv("me");
  await env.calls.start("dm:bob", "bob", "audio");
  const callId = env.sent.find((s) => s.type === "call-invite").call;
  await env.calls.handleFrame({ type: "call-decline", call: callId, senderId: "bob-1", to: "me", reason: "busy" }, "dm:bob");
  out.declined = { active: env.calls.active(), state: env.text("chat-call-state"), notice: env.text("chat-call-notice") };
}

{
  const env = makeEnv("me");
  await env.calls.start("dm:bob", "bob", "audio");
  const callId = env.sent.find((s) => s.type === "call-invite").call;
  await env.calls.handleFrame({ type: "call-accept", call: callId, senderId: "bob-1", to: "me" }, "dm:bob");
  await flush();
  env.elements["chat-call-mute"].click();
  const fresh = track("audio", "mic-2");
  env.freshMics.push(fresh);
  env.mic.readyState = "ended";
  env.mic.emit("ended");
  await flush();
  const pc = env.peers[0];
  const audioSender = () => pc.transceivers.find((t) => t.receiver.track.kind === "audio").sender.track?.id;
  out.mic = { constraints: env.media.at(-1), sender: audioSender(), enabled: fresh.enabled, notice: env.text("chat-call-state"),
    muteLabel: env.text("chat-call-mute") };
  env.clock.fire(5000);
  out.mic.noticeCleared = env.text("chat-call-state");
  env.failNext = true;
  fresh.readyState = "ended";
  fresh.emit("ended");
  await flush();
  out.mic.failed = { notice: env.text("chat-call-state"), active: env.calls.active() };
}

{
  const env = makeEnv("me");
  const { calls, elements: el } = env;
  await calls.start("dm:bob", "bob", "video");
  const panel = el["chat-call-panel"];
  const mini = out.mini = {
    back: env.shown("chat-call-minimize"), expand: env.shown("chat-call-expand"),
    placeholder: env.shown("chat-call-placeholder"), waiting: env.text("chat-call-placeholder-text"),
    avatar: env.text("chat-call-avatar"), localShown: env.shown("chat-call-local"), call: panel.classList.on.has("is-call"),
    videos: env.shown("chat-call-videos"),
  };
  el["chat-call-minimize"].click();
  mini.minimized = { mini: panel.classList.on.has("is-mini"), left: panel.style.left, top: panel.style.top,
    back: env.shown("chat-call-minimize"), expand: env.shown("chat-call-expand"), api: calls.minimized() };
  panel.emit("pointerdown", { pointerId: 3, clientX: 800, clientY: 650, target: el["chat-call-videos"], preventDefault() {} });
  panel.emit("pointermove", { pointerId: 3, clientX: 802, clientY: 651 });
  mini.jitter = { left: panel.style.left, dragging: panel.classList.on.has("is-dragging") };
  panel.emit("pointermove", { pointerId: 3, clientX: 100, clientY: 100 });
  mini.moved = { left: panel.style.left, top: panel.style.top, dragging: panel.classList.on.has("is-dragging") };
  panel.emit("pointermove", { pointerId: 3, clientX: -500, clientY: -500 });
  panel.emit("pointerup", { pointerId: 3 });
  mini.dropped = { left: panel.style.left, top: panel.style.top, saved: env.saved["fm.call.miniPosition"],
    mini: panel.classList.on.has("is-mini"), dragging: panel.classList.on.has("is-dragging") };
  panel.emit("pointerdown", { pointerId: 4, clientX: 20, clientY: 20, target: el["chat-call-videos"], preventDefault() {} });
  panel.emit("pointerup", { pointerId: 4 });
  mini.tapped = { mini: panel.classList.on.has("is-mini"), left: panel.style.left };
  el["chat-call-minimize"].click();
  mini.restored = { left: panel.style.left, top: panel.style.top };
  panel.emit("pointerdown", { pointerId: 5, clientX: 20, clientY: 20, target: { closest: (s) => (s === "button" ? {} : null) } });
  panel.emit("pointerup", { pointerId: 5 });
  mini.buttonPress = panel.classList.on.has("is-mini");
  el["chat-call-expand"].click();
  mini.expanded = { mini: panel.classList.on.has("is-mini"), left: panel.style.left };
  const callId = env.sent.find((s) => s.type === "call-invite").call;
  await calls.handleFrame({ type: "call-accept", call: callId, senderId: "bob-1", to: "me" }, "dm:bob");
  await flush();
  const pc = env.peers[0];
  const remote = el["chat-call-remote"];
  remote.playError = { name: "NotAllowedError" };
  const remoteVideo = track("video", "remote-video");
  pc.ontrack({ track: track("audio", "remote-audio") });
  pc.ontrack({ track: remoteVideo });
  await flush();
  const media = out.remote = { plays: remote.plays, muted: remote.muted, sound: env.shown("chat-call-sound"),
    waiting: env.shown("chat-call-placeholder"), attached: remote.srcObject?.getTracks().length };
  remoteVideo.emit("unmute");
  media.frames = { placeholder: env.shown("chat-call-placeholder"), waiting: panel.classList.on.has("is-waiting") };
  el["chat-call-sound"].click();
  await flush();
  media.unmuted = { muted: remote.muted, sound: env.shown("chat-call-sound"), plays: remote.plays };
  remoteVideo.emit("mute");
  media.stalled = env.shown("chat-call-placeholder");
  el["chat-call-minimize"].click();
  calls.hangUp();
  media.afterHangUp = { placeholder: env.shown("chat-call-placeholder"), mini: panel.classList.on.has("is-mini"),
    call: panel.classList.on.has("is-call"), left: panel.style.left, timers: env.clock.live.size };
}

console.log(JSON.stringify(out));
'''


@pytest.fixture(scope="module")
def data():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(PUBLIC / "chat-calls.js")],
        input=HARNESS, capture_output=True, text=True, timeout=120, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_caller_resends_its_offer_and_answers_duplicate_accepts_from_the_chosen_device(data):
    caller = data["caller"]
    assert caller["firstOffers"] == 1
    assert caller["connectTimer"] == 1
    assert caller["codecPrefs"] == 1
    assert caller["state"] == "Connecting…"
    assert caller["dupAccept"] == 2 and caller["sameSdp"] is True
    assert caller["otherDevice"] == 2
    assert caller["resent"] == 7
    assert caller["offerTimerLeft"] == 0


def test_caller_ignores_duplicate_answers_and_queues_ice_until_the_answer(data):
    caller = data["caller"]
    assert caller["iceQueued"] == 0 and caller["iceFlushed"] == 1
    assert caller["answersApplied"] == 1
    assert caller["stillActive"] is True
    assert caller["live"] == {"state": "Connected · end-to-end encrypted", "connectTimer": 0, "health": 1}


def test_silent_audio_makes_the_caller_restart_ice_until_packets_flow_again(data):
    caller = data["caller"]
    watchdog = caller["watchdog"]
    assert watchdog["restarts"] == 1
    assert watchdog["offers"] == 8
    assert watchdog["state"] == "Reconnecting…" and watchdog["reconnecting"] is True
    assert watchdog["restartLoop"] == 1 and watchdog["giveUp"] == 1
    assert caller["restartSdpDiffers"] is True
    assert caller["restartAnswered"] == 2
    assert caller["recovered"] == {"state": "Connected · end-to-end encrypted", "reconnecting": False, "restartLoop": 0, "giveUp": 0}


def test_disconnected_waits_two_seconds_then_restarts_every_six_and_failed_restarts_at_once(data):
    caller = data["caller"]
    assert caller["disconnected"] == {"state": "Reconnecting…", "restarts": 1, "grace": 1}
    assert caller["afterGrace"] == {"restarts": 2, "offers": 9}
    assert caller["again"] == 3
    assert caller["back"] == {"state": "Connected · end-to-end encrypted", "restartLoop": 0, "giveUp": 0}
    assert caller["failed"] == {"restarts": 4, "active": True, "state": "Reconnecting…"}


def test_a_connection_that_stays_down_for_thirty_seconds_ends_with_a_clear_message(data):
    caller = data["caller"]
    assert caller["gaveUp"] == {
        "active": False, "bye": 1, "state": "Call lost",
        "notice": "The connection dropped and did not come back. Try calling again.",
        "ended": True, "shown": True, "hangup": "Close", "back": False, "intervals": 0,
    }
    assert caller["closed"] == {"hidden": True, "ended": False, "timers": 0}


def test_callee_keeps_the_invite_while_accepting_and_resends_accept_until_the_offer(data):
    callee = data["callee"]
    assert callee["incoming"] is True
    assert callee["stillRinging"] is True
    assert callee["accepted"] is True
    assert callee["firstAccept"] == 1 and callee["acceptTo"] == "alice-1"
    assert callee["connectTimer"] == 1
    assert callee["incomingAfter"] is False
    assert callee["twoResends"] == 3
    assert callee["acceptResends"] == 9
    assert callee["acceptTimer"] == 0


def test_callee_answers_once_resends_the_same_answer_and_renegotiates_on_a_new_offer(data):
    callee = data["callee"]
    answered = callee["answered"]
    assert answered["answers"] == 1 and answered["applied"] == 1 and answered["iceFlushed"] == 1
    assert answered["sending"] == ["sendrecv:mic", "sendrecv:cam"]
    assert answered["codecPrefs"] == 1
    assert answered["tuned"] is True
    assert callee["duplicate"] == {"answers": 2, "applied": 1, "same": True}
    assert callee["restart"] == {"answers": 3, "applied": 2, "fresh": True}


def test_callee_waits_for_the_callers_restart_instead_of_hanging_up(data):
    callee = data["callee"]
    assert callee["live"] == {"state": "Connected · end-to-end encrypted", "connectTimer": 0}
    assert callee["failed"] == {"active": True, "state": "Reconnecting…", "restarts": 0, "giveUp": 1, "offers": 0}
    assert callee["back"] == {"state": "Connected · end-to-end encrypted", "giveUp": 0}
    assert callee["hungUp"] == {"bye": 1, "timers": 0}


def test_a_call_that_never_connects_ends_after_thirty_seconds(data):
    assert data["timeout"] == {
        "active": False, "bye": 1, "state": "Could not connect",
        "notice": "The call could not connect. Try again.", "ended": True, "shown": True,
        "autoClosed": True, "timers": 0,
    }


def test_a_hang_up_while_accepting_says_so_and_releases_the_camera(data):
    assert data["byeWhileAccepting"] == {
        "result": False, "active": False, "state": "Call ended",
        "notice": "@alice hung up before the call connected.", "shown": True, "micStopped": True,
        "incoming": False,
    }


def test_an_invite_during_the_pre_call_check_rings_instead_of_answering_busy(data):
    assert data["inviteDuringPreview"] == {"incoming": True, "busy": 0}


def test_a_declined_call_says_why(data):
    assert data["declined"] == {"active": False, "state": "Busy", "notice": "@bob is on another call. Try again later."}


def test_a_lost_microphone_is_reopened_and_put_back_on_the_call(data):
    mic = data["mic"]
    assert mic["constraints"]["video"] is False
    assert mic["constraints"]["audio"]["echoCancellation"] is True
    assert mic["sender"] == "mic-2"
    assert mic["enabled"] is False and mic["muteLabel"] == "Unmute"
    assert mic["notice"] == "Microphone reconnected"
    assert mic["noticeCleared"] == "Connecting…"
    assert mic["failed"] == {
        "notice": "Your microphone stopped. Check that it is connected and allowed for this site.",
        "active": True,
    }


def test_back_to_chat_turns_the_call_into_a_small_window_that_drags_and_expands(data):
    mini = data["mini"]
    assert mini["back"] is True and mini["expand"] is False
    assert mini["call"] is True and mini["videos"] is True
    assert mini["placeholder"] is True and mini["waiting"] == "Waiting for @bob's video…"
    assert mini["avatar"] == "B" and mini["localShown"] is True
    assert mini["minimized"] == {"mini": True, "left": "764px", "top": "624px", "back": False, "expand": True, "api": True}
    assert mini["jitter"] == {"left": "764px", "dragging": False}
    assert mini["moved"] == {"left": "64px", "top": "74px", "dragging": True}
    assert mini["dropped"] == {"left": "8px", "top": "8px", "saved": '{"left":8,"top":8}', "mini": True, "dragging": False}
    assert mini["tapped"] == {"mini": False, "left": ""}
    assert mini["restored"] == {"left": "8px", "top": "8px"}
    assert mini["buttonPress"] is True
    assert mini["expanded"] == {"mini": False, "left": ""}


def test_remote_video_plays_and_offers_sound_when_autoplay_is_blocked(data):
    media = data["remote"]
    assert media["plays"] == 2 and media["muted"] is True and media["sound"] is True
    assert media["waiting"] is True and media["attached"] == 2
    assert media["frames"] == {"placeholder": False, "waiting": False}
    assert media["unmuted"] == {"muted": False, "sound": False, "plays": 3}
    assert media["stalled"] is True
    assert media["afterHangUp"] == {"placeholder": False, "mini": False, "call": False, "left": "", "timers": 0}


def test_call_panel_has_the_back_button_mini_window_and_placeholder():
    for marker in (
        'id="chat-call-minimize"', 'id="chat-call-expand"', 'id="chat-call-videos"',
        'id="chat-call-placeholder"', 'id="chat-call-avatar"', 'id="chat-call-sound"', 'id="chat-call-notice"',
    ):
        assert marker in HTML
    assert ".chat-call-panel.is-mini" in CSS
    assert ".chat-call-panel.is-video.is-call {" in CSS
    assert ':has(video:not([hidden]))' not in CSS
    assert 'const MINI_POSITION_KEY = "fm.call.miniPosition";' in CALLS


def test_diagnostics_button_is_labelled_and_turns_red():
    assert 'diagText.textContent = "Diagnose";' in METER
    assert 'diag.setAttribute("aria-label", "Call diagnostics");' in METER
    rule = CSS.split(".fm-q-diag {", 1)[1].split("}", 1)[0]
    assert "padding: 0 8px" in rule and "color: var(--fg)" in rule
    assert '.fm-q-diag[data-state="warn"], .fm-q-diag[data-state="bad"] { border-color: var(--danger); color: var(--danger); }' in CSS


def test_a_ring_on_the_account_channel_opens_that_conversation():
    assert 'topic === "calls"' in CHAT
    assert "openRingingConversation(frame);" in CHAT
