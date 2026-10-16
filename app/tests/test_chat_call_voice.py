import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CALLS = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")
VOICE = (PUBLIC / "chat-call-voice.js").read_text(encoding="utf-8")
WORKLET = (PUBLIC / "chat-call-voice-worklet.js").read_text(encoding="utf-8")
HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
NATIVE = ROOT.parent / "native"
IOS_PAGE = NATIVE / "ios" / "ForkMesh" / "Features" / "FaceChanger" / "direct-call.html"
ANDROID_PAGE = NATIVE / "android" / "app" / "src" / "main" / "assets" / "direct-call.html"

DSP_HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[2].replace(/chat-call-voice-worklet\.js$/, "");
const W = await import(pathToFileURL(base + "chat-call-voice-worklet.js").href);
const V = await import(pathToFileURL(base + "chat-call-voice.js").href);
const sr = 48000;
const seconds = 4;
function glottal(f0, glide) {
  const x = new Float32Array(sr * seconds);
  let phase = 0, y1 = 0, y2 = 0;
  const formants = [[700, 0.97], [1200, 0.96], [2600, 0.95]];
  const state = formants.map(() => [0, 0]);
  for (let i = 0; i < x.length; i++) {
    phase += f0 * Math.pow(2, glide * Math.sin(2 * Math.PI * 0.7 * i / sr) / 12) / sr;
    let s = 0;
    if (phase >= 1) { phase -= 1; s = 1; }
    let v = 0;
    formants.forEach(([hz, r], k) => {
      const w = 2 * Math.PI * hz / sr;
      const y = s + 2 * r * Math.cos(w) * state[k][0] - r * r * state[k][1];
      state[k][1] = state[k][0];
      state[k][0] = y;
      v += y;
    });
    x[i] = 0.02 * v;
  }
  return x;
}
function render(x, preset, seed, block = () => 128) {
  const engine = new W.VoiceEngine(sr, seed);
  engine.setVoice(preset);
  const y = new Float32Array(x.length);
  const times = [];
  for (let i = 0; i < x.length;) {
    const n = Math.min(block(), x.length - i);
    const t0 = process.hrtime.bigint();
    engine.process(x.subarray(i, i + n), y.subarray(i, i + n), n);
    times.push(Number(process.hrtime.bigint() - t0) / 1e3);
    i += n;
  }
  return { y, engine, times };
}
function medianPitch(y) {
  const detector = new W.PitchDetector(sr / 4, 1024);
  const dec = new Float64Array(Math.floor(y.length / 4));
  for (let i = 0; i < dec.length; i++) dec[i] = (y[4 * i] + y[4 * i + 1] + y[4 * i + 2] + y[4 * i + 3]) / 4;
  const found = [];
  for (let s = 6000; s + 1024 <= dec.length; s += 256) {
    const hz = detector.detect(dec.subarray(s, s + 1024));
    if (hz > 0) found.push(hz);
  }
  found.sort((a, b) => a - b);
  return found.length ? found[Math.floor(found.length / 2)] : 0;
}
function zeroRuns(y, from) {
  let run = 0, count = 0;
  for (let i = from; i < y.length; i++) {
    if (Math.abs(y[i]) < 1e-5) { run++; if (run === 48) count++; } else run = 0;
  }
  return count;
}
function worstStep(y, from) {
  let worst = 0, env = 0;
  for (let i = from; i < y.length; i++) {
    env += 0.002 * (Math.abs(y[i]) - env);
    if (i > from + 2) worst = Math.max(worst, Math.abs(y[i] - 2 * y[i - 1] + y[i - 2]) / (env + 1e-4));
  }
  return worst;
}
const out = { voices: V.CALL_VOICES.map((v) => v.id), shifts: [] };
for (const [caller, f0] of [["male", 115], ["female", 205]]) {
  const x = glottal(f0, 2);
  for (const preset of V.CALL_VOICES) {
    const r = render(x, preset, f0);
    const [low, high] = W.VOICE_LIMITS.ratio;
    const expected = f0 * Math.min(high, Math.max(low, preset.pitchHz / f0));
    out.shifts.push({ caller, voice: preset.id, target: expected, achieved: medianPitch(r.y), gaps: zeroRuns(r.y, 4000) });
  }
}
const sine = new Float32Array(sr * seconds);
let p = 0;
for (let i = 0; i < sine.length; i++) {
  p += 160 * Math.pow(2, 2 * Math.sin(2 * Math.PI * 0.5 * i / sr) / 12) / sr;
  sine[i] = 0.4 * Math.sin(2 * Math.PI * p) + 0.15 * Math.sin(4 * Math.PI * p + 0.3);
}
const off = render(sine, null, 0);
const latency = off.engine.latencySamples();
let offDiff = 0;
for (let i = latency; i < sine.length; i++) offDiff = Math.max(offDiff, Math.abs(off.y[i] - sine[i - latency]));
out.latencyMs = latency / sr * 1000;
out.offDiff = offDiff;
const steady = render(sine, V.callVoiceById("maya"), 160);
let seed = 7;
const random = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return 1 + (seed % 300); };
const ragged = render(sine, V.callVoiceById("maya"), 160, random);
let blockDiff = 0;
for (let i = 0; i < sine.length; i++) blockDiff = Math.max(blockDiff, Math.abs(steady.y[i] - ragged.y[i]));
out.blockDiff = blockDiff;
out.steadyStep = worstStep(steady.y, 4000);
const switching = new W.VoiceEngine(sr, 160);
const sy = new Float32Array(sine.length);
const order = ["maya", "kofi", "", "ingrid", "robot", "theo", "older-woman", "ren", "deep", ""];
for (let i = 0, k = 0; i < sine.length; i += 128) {
  if (i % (sr / 2) === 0) switching.setVoice(V.callVoiceById(order[k++ % order.length]));
  switching.process(sine.subarray(i, i + 128), sy.subarray(i, i + 128), 128);
}
out.switchStep = worstStep(sy, 4000);
out.switchGaps = zeroRuns(sy, 4000);
const loud = new Float32Array(sr).map((_, i) => 1.6 * Math.sin(2 * Math.PI * 150 * i / sr));
out.peak = render(loud, V.callVoiceById("ingrid"), 150).y.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
const times = steady.times.slice(400).sort((a, b) => a - b);
out.meanUs = times.reduce((a, b) => a + b, 0) / times.length;
out.plans = {
  maleToMaya: W.voicePlan(V.callVoiceById("maya"), 115),
  femaleToKofi: W.voicePlan(V.callVoiceById("kofi"), 210),
  femaleToMaya: W.voicePlan(V.callVoiceById("maya"), 196),
  robot: W.voicePlan(V.callVoiceById("robot"), 150),
  olderWoman: W.voicePlan(V.callVoiceById("older-woman"), 150),
  off: W.voicePlan(null, 150),
};
out.resolved = {
  faceKofi: V.resolveCallVoice("face", "kofi"),
  faceCustom: V.resolveCallVoice("face", "custom"),
  faceUnknown: V.resolveCallVoice("face", "someone-new"),
  picked: V.resolveCallVoice("Older-Woman", "maya"),
  off: V.resolveCallVoice("off", "maya"),
};
out.labels = V.CALL_VOICES.map((v) => v.label);
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
  addTrack(track) { this.tracks.push(track); }
}
globalThis.MediaStream = FakeStream;
const track = (kind, id) => ({ kind, id, enabled: true, stopped: false, stop() { this.stopped = true; } });
const listeners = {};
const element = (id) => ({
  id, hidden: true, value: "", textContent: "", options: [],
  append(option) { this.options.push(option); },
  addEventListener(type, fn) { (listeners[id + ":" + type] ||= []).push(fn); },
  classList: { toggle() {} },
});
const elements = Object.fromEntries(["chat-call-panel", "chat-call-voice-row", "chat-call-voice", "chat-call-voice-state"].map((id) => [id, element(id)]));
const fire = (id, type) => (listeners[id + ":" + type] || []).forEach((fn) => fn());
const documentRef = {
  querySelector: (selector) => elements[selector.slice(1)] || null,
  createElement: () => ({}),
};
const mic = track("audio", "mic");
const stages = [];
async function createVoiceStageImpl({ sourceTrack, voiceId, userPitchHz, onStats }) {
  const stage = { track: track("audio", "voice-" + stages.length), source: sourceTrack, seed: userPitchHz, current: voiceId,
    applied: [], stopped: false, voice() { return this.current; }, setVoice(id) { this.current = id; this.applied.push(id); },
    monitor(on) { this.monitored = on; return on; }, stop() { this.stopped = true; } };
  onStats({ userPitchHz: 121 });
  stages.push(stage);
  return stage;
}
let pc = null;
class FakePeer {
  constructor() { this.transceivers = []; pc = this; }
  addTransceiver(value) {
    const kind = typeof value === "string" ? value : value.kind;
    this.transceivers.push({ sender: { track: typeof value === "string" ? null : value,
      async replaceTrack(next) { this.track = next; } }, receiver: { track: { kind } } });
  }
  getTransceivers() { return this.transceivers; }
  async setLocalDescription() { this.localDescription = {}; }
  close() {}
}
const stored = new Map([["forkmesh.chat.callFace", "ingrid"], ["forkmesh.chat.callVoicePitch", "118"]]);
const storage = { getItem: (k) => stored.get(k) ?? null, setItem: (k, v) => stored.set(k, v), removeItem: (k) => stored.delete(k) };
const sent = [];
const calls = createChatCalls({
  selfId: "me",
  sendSignal: async (channel, type, extra) => { sent.push({ type, ...extra }); return true; },
  getUserMediaImpl: async () => new FakeStream([mic]),
  RTCPeerConnectionImpl: FakePeer,
  documentRef,
  storage,
  voicesSupported: () => true,
  createVoiceStageImpl,
  setTimeoutImpl: () => 1, clearTimeoutImpl() {},
  setIntervalImpl: () => 1, clearIntervalImpl() {},
});
const out = { options: elements["chat-call-voice"].options.map((o) => o.value) };
await calls.start("dm:bob", "bob", "audio");
await calls.voiceReady();
out.matchedFace = calls.voice();
out.seeded = stages[0].seed;
out.stageSeesMic = stages[0].source === mic;
out.pitchRemembered = stored.get("forkmesh.chat.callVoicePitch");
out.indicator = elements["chat-call-voice-state"].textContent;
out.rowShown = !elements["chat-call-voice-row"].hidden;
const callId = sent.find((s) => s.type === "call-invite").call;
await calls.handleFrame({ type: "call-accept", call: callId, senderId: "peer", to: "me" }, "dm:bob");
const audio = () => pc.transceivers.find((t) => t.receiver.track.kind === "audio").sender.track.id;
out.offeredAudio = audio();
elements["chat-call-voice"].value = "kofi";
fire("chat-call-voice", "change");
out.picked = calls.voice();
out.remembered = stored.get("forkmesh.chat.callVoice");
out.indicatorAfter = elements["chat-call-voice-state"].textContent;
calls.setVoice("off");
out.off = calls.voice();
out.stillSendingStage = audio();
out.monitor = calls.monitorVoice(true);
calls.hangUp();
out.stoppedOnHangup = stages[0].stopped;
out.stageCount = stages.length;
console.log(JSON.stringify(out));
'''


def _node(harness, module):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(PUBLIC / module)],
        input=harness, capture_output=True, text=True, timeout=180, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


@pytest.fixture(scope="module")
def dsp():
    return _node(DSP_HARNESS, "chat-call-voice-worklet.js")


FACE_VOICES = ["maya", "theo", "kofi", "ingrid", "ren"]
MORE_VOICES = ["deep", "older-man", "young-man", "warm", "low", "older-woman", "teen", "neutral", "robot"]


def test_every_face_has_a_voice_and_more_voices_follow(dsp):
    assert dsp["voices"] == FACE_VOICES + MORE_VOICES
    assert len(set(dsp["labels"])) == len(dsp["labels"])


def test_match_face_falls_back_to_your_own_voice_for_faces_without_one(dsp):
    assert dsp["resolved"] == {
        "faceKofi": "kofi", "faceCustom": "", "faceUnknown": "", "picked": "older-woman", "off": "",
    }


def test_each_voice_lands_on_its_pitch_from_a_male_and_a_female_caller(dsp):
    assert len(dsp["shifts"]) == 2 * len(FACE_VOICES + MORE_VOICES)
    for shift in dsp["shifts"]:
        semitones = 12 * abs(__import__("math").log2(shift["achieved"] / shift["target"]))
        assert semitones < 1.0, shift
        assert shift["gaps"] == 0, shift


def test_plans_follow_the_caller(dsp):
    plans = dsp["plans"]
    assert plans["maleToMaya"]["ratio"] > 1.6 and plans["maleToMaya"]["formant"] > 1.1
    assert plans["femaleToKofi"]["ratio"] < 0.5 and plans["femaleToKofi"]["formant"] < 0.85
    assert 0.95 < plans["femaleToMaya"]["ratio"] < 1.05
    assert plans["robot"]["range"] == 0
    assert plans["olderWoman"]["jitter"] > 0 and plans["olderWoman"]["tremor"] > 0
    assert plans["off"] == {
        "ratio": 1, "formant": 1, "brightness": 0, "breath": 0, "range": 1, "jitter": 0, "tremor": 0, "mix": 0,
    }


def test_own_voice_is_the_input_delayed_within_the_latency_budget(dsp):
    assert dsp["offDiff"] == 0
    assert dsp["latencyMs"] <= 40


def test_output_does_not_depend_on_render_block_sizes(dsp):
    assert dsp["blockDiff"] == 0


def test_no_clicks_or_gaps_while_shifting_and_switching_voices(dsp):
    assert dsp["steadyStep"] < 0.2
    assert dsp["switchStep"] < 0.2
    assert dsp["switchGaps"] == 0


def test_limiter_keeps_peaks_below_full_scale(dsp):
    assert dsp["peak"] <= 0.951


def test_engine_runs_well_inside_a_render_quantum(dsp):
    assert dsp["meanUs"] < 0.5 * 128 / 48000 * 1e6


def test_call_sends_the_changed_voice_and_follows_the_picker():
    data = _node(CALLS_HARNESS, "chat-calls.js")
    assert data["options"] == ["face", "off", *FACE_VOICES, *MORE_VOICES]
    assert data["matchedFace"] == "ingrid"
    assert data["seeded"] == 118
    assert data["stageSeesMic"] is True
    assert data["pitchRemembered"] == "121"
    assert data["indicator"] == "Voice: Ingrid"
    assert data["rowShown"] is True
    assert data["offeredAudio"] == "voice-0"
    assert data["picked"] == "kofi"
    assert data["remembered"] == "kofi"
    assert data["indicatorAfter"] == "Voice: Kofi"
    assert data["off"] == ""
    assert data["stillSendingStage"] == "voice-0"
    assert data["monitor"] is True
    assert data["stoppedOnHangup"] is True
    assert data["stageCount"] == 1


def test_call_panel_has_a_voice_picker():
    assert 'id="chat-call-voice-row"' in HTML
    assert 'id="chat-call-voice"' in HTML
    assert 'id="chat-call-voice-state"' in HTML
    assert 'from "./chat-call-voice.js"' in CALLS
    assert 'registerProcessor("fm-call-voice"' in WORKLET
    assert "export async function createVoiceStage(" in VOICE
    assert "export async function createVoicePreview(" in VOICE
    assert "monitor(on) {" in VOICE


def test_native_call_pages_inline_the_current_voice_modules():
    result = subprocess.run(
        ["python3", str(ROOT / "tools" / "sync_native_call_voice.py"), "--check"],
        capture_output=True, text=True, timeout=60, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert IOS_PAGE.read_text(encoding="utf-8") == ANDROID_PAGE.read_text(encoding="utf-8")


def test_native_call_page_exposes_voice_operations():
    page = IOS_PAGE.read_text(encoding="utf-8")
    assert "script-src 'unsafe-inline' blob:;" in page
    assert '<script type="text/plain" id="fm-call-voice-worklet">' in page
    assert "window.FmCallVoice = (() => {" in page
    assert "async setup(media, iceServers, face) {\n      const voice = arguments[3];" in page
    for op in ("async voice(id) {", "async voiceList() {", "async preview(media, voice, face, background, image", "async monitor(on) {"):
        assert op in page
    assert 'post({ event: "voice", id });' in page
    assert page.count("await voiceReady();") == 2


def test_apps_wire_the_voice_choice():
    ios_look = (NATIVE / "ios" / "ForkMeshCore" / "Sources" / "FMChat" / "FaceChanger" / "FaceChangerLook.swift").read_text(encoding="utf-8")
    ios_engine = (NATIVE / "ios" / "ForkMeshCore" / "Sources" / "FMChat" / "Direct" / "DirectCalls.swift").read_text(encoding="utf-8")
    ios_peer = (NATIVE / "ios" / "ForkMesh" / "Features" / "FaceChanger" / "WebCallPeer.swift").read_text(encoding="utf-8")
    ios_controls = (NATIVE / "ios" / "ForkMesh" / "Features" / "FaceChanger" / "FaceChangerControls.swift").read_text(encoding="utf-8")
    ios_views = (NATIVE / "ios" / "ForkMesh" / "Features" / "Chat" / "Calls" / "DirectCallViews.swift").read_text(encoding="utf-8")
    assert 'public static let voiceKey = "chat.callVoice"' in ios_look
    assert "func setVoice(_ id: String)" in ios_engine
    assert 'case "voice":' in ios_peer
    assert "static var headphonesConnected: Bool" in ios_peer
    assert 'accessibilityIdentifier("chat-call-voice")' in ios_controls
    assert "FaceChangerVoiceMenu(look: engine.look, shown: engine.voice)" in ios_views
    assert "look.setVoiceChoice(prefs?.string(forKey: FaceChangerLook.voiceKey)" in ios_controls
    android = NATIVE / "android" / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp"
    core = (NATIVE / "android" / "core" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "facechanger" / "FaceChangerLook.kt").read_text(encoding="utf-8")
    peer = (android / "feature" / "facechanger" / "WebCallPeer.kt").read_text(encoding="utf-8")
    feature = (android / "feature" / "facechanger" / "FaceChangerFeature.kt").read_text(encoding="utf-8")
    controls = (android / "feature" / "facechanger" / "FaceChangerControls.kt").read_text(encoding="utf-8")
    assert "fun setVoiceChoice(choice: String): Boolean" in core
    assert 'const val VOICE_PREF = "chat.callVoice"' in core
    assert '"voice" -> {' in peer
    assert "fun callHeadphonesConnected(context: Context): Boolean" in peer
    assert "prefs?.putString(FaceChangerLook.VOICE_PREF, choice)" in feature
    assert "pickVoice(messenger, state.voiceChoice)" in controls
