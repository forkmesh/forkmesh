import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
PUBLIC = ROOT / "public"
MODULE = PUBLIC / "call-quality-meter.js"
PAGES = (
    REPOSITORY / "native" / "ios" / "ForkMesh" / "Features" / "FaceChanger" / "direct-call.html",
    REPOSITORY / "native" / "android" / "app" / "src" / "main" / "assets" / "direct-call.html",
)

HARNESS = r'''
import { pathToFileURL } from "node:url";
const Q = await import(pathToFileURL(process.argv.at(-1)).href);
const out = {};

function report(entries) {
  return new Map(entries.map((entry) => [entry.id, entry]));
}
const tick = (bytesOut, bytesIn, lost, received, decoded, level) => report([
  { id: "T", type: "transport", selectedCandidatePairId: "P" },
  { id: "P", type: "candidate-pair", currentRoundTripTime: 0.048, state: "succeeded" },
  { id: "OA", type: "outbound-rtp", kind: "audio", bytesSent: bytesOut / 4 },
  { id: "OV", type: "outbound-rtp", kind: "video", bytesSent: bytesOut, frameWidth: 640, frameHeight: 360, framesPerSecond: 24, qualityLimitationReason: "bandwidth" },
  { id: "IA", type: "inbound-rtp", kind: "audio", bytesReceived: bytesIn / 4, packetsLost: lost, packetsReceived: received, jitter: 0.004, audioLevel: level },
  { id: "IV", type: "inbound-rtp", kind: "video", bytesReceived: bytesIn, packetsLost: 0, packetsReceived: received, frameWidth: 1280, frameHeight: 720, framesDecoded: decoded },
  { id: "MS", type: "media-source", kind: "audio", audioLevel: 0.25 },
]);
const first = Q.summarizeRtcStats([tick(100000, 200000, 0, 1000, 100, 0.5)], null, 1000);
const second = Q.summarizeRtcStats([tick(225000, 450000, 40, 1960, 130, 0.5)], first.counters, 2000);
out.first = first.sample;
out.second = second.sample;

const fmmA = Q.summarizeFmmStats({
  transport: { currentRoundTripTime: 0.12, loss: 0.025 },
  outbound: [{ kind: "audio", bytesSent: 1000 }, { kind: "video", bytesSent: 10000, frameWidth: 640, frameHeight: 480, framesSent: 10, representation: "video" }],
  inbound: [{ kind: "audio", peer: "a", targetMs: 60 }, { kind: "video", peer: "a", ssrc: 2, bytesReceived: 50000, framesDecoded: 20, frameWidth: 320, frameHeight: 240 }],
}, null, 0);
const fmmB = Q.summarizeFmmStats({
  transport: { currentRoundTripTime: 0.12, loss: 0.025 },
  outbound: [{ kind: "audio", bytesSent: 3000 }, { kind: "video", bytesSent: 60000, frameWidth: 640, frameHeight: 480, framesSent: 40, representation: "video" }],
  inbound: [{ kind: "audio", peer: "a", targetMs: 60 }, { kind: "video", peer: "a", ssrc: 2, bytesReceived: 150000, framesDecoded: 50, frameWidth: 320, frameHeight: 240 }],
}, fmmA.counters, 2000);
out.fmm = fmmB.sample;

const grade = (rttMs, lossPct, jitterMs = null) => Q.gradeCallQuality({ rttMs, lossPct, jitterMs });
out.grades = {
  unknown: Q.gradeCallQuality({ rttMs: null, lossPct: null, jitterMs: 5 }),
  good: grade(40, 0.2, 5),
  fairRtt: grade(160, 0),
  fairLoss: grade(40, 4),
  poorRtt: grade(600, 0),
  poorLoss: grade(40, 9),
  poorJitter: grade(40, 0, 120),
};
out.bars = [Q.qualityBars({ rttMs: 40, lossPct: 0 }), Q.qualityBars({ rttMs: 200, lossPct: 0 }), Q.qualityBars({ rttMs: 350, lossPct: 0 }), Q.qualityBars({ rttMs: 40, lossPct: 10 })];
out.network = Q.describeNetwork(second.sample);
out.videoIn = Q.describeVideo(second.sample, "in");
out.videoOut = Q.describeVideo(second.sample, "out");
out.event = Q.qualityEvent(second.sample, [{ mic: 0.333, peer: null }, { mic: 0.5, peer: 0.25 }]);
out.silent = Q.levelFromSamples(new Float32Array(64));
out.loud = Q.levelFromSamples(new Float32Array(64).fill(1));
out.quiet = Q.levelFromSamples(new Float32Array(64).fill(0.01));

const timers = [];
const scope = {
  setInterval(fn, ms) { timers.push({ fn, ms }); return timers.length; },
  clearInterval(id) { timers[id - 1] = null; },
};
const pc = { getStats: async () => tick(100000, 200000, 0, 1000, 100, 0.5) };
const meter = Q.createCallQualityMeter({ readStats: Q.rtcStatsReader(() => [pc]), scope, AudioContextImpl: null });
await new Promise((resolve) => setTimeout(resolve, 10));
const levelTimer = timers.find((timer) => timer.ms === Q.QUALITY_LEVEL_MS);
for (let i = 0; i < 60; i += 1) levelTimer.fn();
out.meter = {
  samples: meter.samples.length,
  levels: meter.levels.length,
  drained: meter.drain().length,
  drainedAgain: meter.drain().length,
  peer: meter.levels.at(-1).peer,
  grade: meter.grade(),
};
meter.stop();
out.stopped = timers.filter(Boolean).length;
console.log(JSON.stringify(out));
'''


@pytest.fixture(scope="module")
def result():
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    run = subprocess.run(
        ["node", "--input-type=module", "-e", HARNESS, str(MODULE)],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert run.returncode == 0, run.stderr[-2000:]
    return json.loads(run.stdout)


def test_rtc_stats_become_a_sample(result):
    first = result["first"]
    assert first["rttMs"] == pytest.approx(48)
    assert first["sendKbps"] is None and first["lossPct"] is None
    second = result["second"]
    assert second["rttMs"] == pytest.approx(48)
    assert second["sendKbps"] == pytest.approx((125000 * 1.25) * 8 / 1000)
    assert second["recvKbps"] == pytest.approx((250000 * 1.25) * 8 / 1000)
    assert second["lossPct"] == pytest.approx(40 * 100 / (40 + 1920))
    assert second["jitterMs"] == pytest.approx(4)
    assert (second["inWidth"], second["inHeight"], second["inFps"]) == (1280, 720, 30)
    assert (second["outWidth"], second["outHeight"], second["outFps"]) == (640, 360, 24)
    assert second["limit"] == "bandwidth"
    assert second["micLevel"] == 0.25 and second["peerLevel"] == 0.5


def test_fmm_stats_become_a_sample(result):
    fmm = result["fmm"]
    assert fmm["rttMs"] == pytest.approx(120)
    assert fmm["lossPct"] == pytest.approx(2.5)
    assert fmm["jitterMs"] is None
    assert fmm["sendKbps"] == pytest.approx(52000 * 8 / 1000 / 2)
    assert fmm["recvKbps"] == pytest.approx(100000 * 8 / 1000 / 2)
    assert (fmm["inWidth"], fmm["inHeight"], fmm["inFps"]) == (320, 240, 15)
    assert (fmm["outWidth"], fmm["outFps"]) == (640, 15)


def test_grades_follow_the_thresholds(result):
    assert result["grades"] == {
        "unknown": "unknown",
        "good": "good",
        "fairRtt": "fair",
        "fairLoss": "fair",
        "poorRtt": "poor",
        "poorLoss": "poor",
        "poorJitter": "poor",
    }
    assert result["bars"] == [4, 3, 2, 1]


def test_descriptions_and_phone_event(result):
    assert result["network"] == "48 ms · 2.0% loss · 4 ms jitter"
    assert result["videoIn"] == "1280×720 · 30 fps · 2.5 Mb/s"
    assert result["videoOut"] == "640×360 · 24 fps · 1.3 Mb/s · limited by bandwidth"
    event = result["event"]
    assert event["event"] == "quality" and event["grade"] == "fair" and event["bars"] == 3
    assert event["rttMs"] == 48 and event["lossPct"] == 2.0
    assert event["mic"] == [0.33, 0.5] and event["peer"] == [None, 0.25]
    assert set(event) >= {"inWidth", "inHeight", "inFps", "outWidth", "outHeight", "outFps", "sendKbps", "recvKbps", "jitterMs", "limit"}


def test_levels_map_decibels(result):
    assert result["silent"] == 0
    assert result["loud"] == 1
    assert result["quiet"] == pytest.approx(1 / 3, abs=0.01)


def test_meter_keeps_bounded_histories(result):
    meter = result["meter"]
    assert meter["samples"] == 1
    assert meter["levels"] == 50
    assert meter["drained"] == 50 and meter["drainedAgain"] == 0
    assert meter["peer"] == 0.5
    assert meter["grade"] == "good"
    assert result["stopped"] == 0


def test_copies_are_in_sync():
    run = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "sync_call_quality.py"), "--check"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert run.returncode == 0, run.stdout
    assert PAGES[0].read_bytes() == PAGES[1].read_bytes()


def test_every_call_surface_mounts_the_widget():
    calls = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")
    assert 'from "./call-quality-meter.js"' in calls
    assert "startQuality(pc);" in calls and "stopQuality();" in calls
    chat = (PUBLIC / "chat.js").read_text(encoding="utf-8")
    assert "syncGroupCallQuality(active || null);" in chat
    assert ".fm-q-pill" in (PUBLIC / "chat.css").read_text(encoding="utf-8")
    office = (REPOSITORY / "office" / "public" / "app.js").read_text(encoding="utf-8")
    assert "from '../../app/public/call-quality-meter.js'" in office
    assert 'id="quality"' in (REPOSITORY / "office" / "public" / "index.html").read_text(encoding="utf-8")
    page = PAGES[0].read_text(encoding="utf-8")
    assert "window.FmCallQuality" in page
    assert "startQuality(current);" in page and "stopQuality();" in page


DIAGNOSTICS_HARNESS = r'''
import { pathToFileURL } from "node:url";
const Q = await import(pathToFileURL(process.argv.at(-1)).href);
const out = {};
const base = { rttMs: 40, lossPct: 0, jitterMs: 5, sendKbps: 1500, recvKbps: 1500, outWidth: 1280, outHeight: 720, outFps: 30, inWidth: 1280, inHeight: 720, inFps: 30, limit: "", faceMs: null, faceFps: null, faceLevel: null };
const series = (patch, count = 8) => Array.from({ length: count }, (_, i) => ({ ...base, at: 1000 * (i + 1), ...patch }));
out.healthy = Q.diagnoseCall(series({}), { video: true });
out.early = Q.diagnoseCall(series({ rttMs: 900 }, 2), { video: true });
out.network = Q.diagnoseCall(series({ rttMs: 620, lossPct: 4, jitterMs: 70 }), { video: true });
out.video = Q.diagnoseCall(series({ limit: "cpu", outFps: 6, inHeight: 180, inFps: 9, faceMs: 58, faceFps: 14, faceLevel: 3 }), { video: true });
out.cameraOff = Q.diagnoseCall(series({ limit: "bandwidth", outFps: 1 }), { video: true, sending: false });
out.theirCameraOff = Q.diagnoseCall(series({ recvKbps: 4, inFps: 0 }), { video: true });
out.voiceOnly = Q.diagnoseCall(series({ outFps: 3, limit: "cpu" }), { video: false });

const memory = Q.emptyDiagnosticsMemory();
const latency = out.network.issues.find((issue) => issue.id === "latency");
out.firstRank = Q.rankFixes(latency, memory).map((fix) => fix.id);
Q.recordFixOutcome(memory, "latency", "wired", false);
Q.recordFixOutcome(memory, "latency", "vpn", true);
out.learnedRank = Q.rankFixes(latency, memory).map((fix) => fix.id);
Q.rememberReport(memory, { ...out.network, at: 1 });
Q.rememberReport(memory, out.network);
out.noteSaved = Q.rememberNote(memory, "  LESSON:  turn the VPN off first ");
out.noteEmpty = Q.rememberNote(memory, "lesson:   ");
for (let i = 0; i < 30; i += 1) Q.rememberReport(memory, { ...out.healthy, at: 100 + i });
out.reports = memory.reports.length;
out.notes = memory.notes;
const stored = new Map();
const storage = { getItem: (key) => stored.get(key) ?? null, setItem: (key, value) => stored.set(key, value) };
Q.saveDiagnosticsMemory(storage, memory);
out.reloaded = Q.loadDiagnosticsMemory(storage).fixes;
out.broken = Q.loadDiagnosticsMemory({ getItem: () => "{not json" });
const promptMemory = Q.emptyDiagnosticsMemory();
Q.rememberReport(promptMemory, { ...out.network, at: 5 });
Q.recordFixOutcome(promptMemory, "latency", "vpn", true);
Q.recordFixOutcome(promptMemory, "loss", "wired", false);
Q.rememberNote(promptMemory, "LESSON: the office Wi-Fi drops packets; use the cable");
Q.rememberReport(promptMemory, out.network);
out.prompt = Q.diagnosticsPrompt(out.network, promptMemory, { userAgent: "TestBrowser/1", cores: 8 });
out.healthyPrompt = Q.diagnosticsPrompt(out.healthy, Q.emptyDiagnosticsMemory(), {});

const timers = [];
const scope = {
  setInterval(fn, ms) { timers.push({ fn, ms }); return timers.length; },
  clearInterval(id) { timers[id - 1] = null; },
};
const stage = { frames: 0, frameMs: 0, level: 0 };
let clock = 1000;
const realNow = Date.now;
Date.now = () => clock;
const meter = Q.createCallQualityMeter({ readStats: async () => ({ ...base, at: clock }), face: () => stage, scope, AudioContextImpl: null });
await new Promise((resolve) => setTimeout(resolve, 5));
const statsTimer = timers.find((timer) => timer && timer.ms === Q.QUALITY_STATS_MS);
for (let i = 0; i < 3; i += 1) {
  clock += 1000;
  stage.frames += 20;
  stage.frameMs += 20 * 45;
  stage.level = 2;
  await statsTimer.fn();
  await new Promise((resolve) => setTimeout(resolve, 5));
}
out.face = { fps: meter.latest.faceFps, ms: meter.latest.faceMs, level: meter.latest.faceLevel, first: meter.samples[0].faceMs };
meter.stop();
Date.now = realNow;
console.log(JSON.stringify(out));
'''

WIDGET_HARNESS = r'''
import { pathToFileURL } from "node:url";
const Q = await import(pathToFileURL(process.argv.at(-1)).href);
class Element {
  constructor(tag) { this.tagName = tag; this.children = []; this.dataset = {}; this.attributes = {}; this.listeners = {}; this.hidden = false; this.textContent = ""; this.value = ""; this.style = {}; this.className = ""; }
  append(...nodes) { this.children.push(...nodes); for (const node of nodes) node.parent = this; }
  replaceChildren(...nodes) { this.children = []; this.append(...nodes); }
  remove() { if (this.parent) this.parent.children = this.parent.children.filter((node) => node !== this); }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  getAttribute(name) { return this.attributes[name] ?? null; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  click() { for (const fn of this.listeners.click || []) fn(); }
  get classList() { return { toggle: () => {} }; }
  select() { this.selected = true; }
  getContext() { return null; }
  find(name) { if (String(this.className).split(" ").includes(name)) return this; for (const child of this.children) { const hit = child.find?.(name); if (hit) return hit; } return null; }
  text() { return [this.textContent, ...this.children.map((child) => child.text?.() || "")].join(" "); }
}
const documentRef = { createElement: (tag) => new Element(tag) };
const stored = new Map();
const storage = { getItem: (key) => stored.get(key) ?? null, setItem: (key, value) => stored.set(key, value) };
const listeners = new Set();
const base = { rttMs: 40, lossPct: 0, jitterMs: 5, sendKbps: 1500, recvKbps: 1500, outWidth: 1280, outHeight: 720, outFps: 30, inWidth: 1280, inHeight: 720, inFps: 30, limit: "" };
const meter = { samples: [], levels: [], latest: null, hasStats: true, subscribe(fn) { listeners.add(fn); return () => listeners.delete(fn); } };
const push = (patch) => { const sample = { ...base, at: 1000 * (meter.samples.length + 1), ...patch }; meter.samples.push(sample); meter.latest = sample; for (const fn of listeners) fn(meter); };
let copied = "";
const scope = { navigator: { userAgent: "TestBrowser/1", hardwareConcurrency: 4, clipboard: { writeText: async (text) => { copied = text; } } } };
const host = new Element("div");
const widget = Q.mountCallQualityWidget(host, meter, { documentRef, storage, scope, video: () => true });
const diag = widget.element.find("fm-q-diag");
const panel = widget.element.find("fm-q-diag-panel");
const out = {};
for (let i = 0; i < 4; i += 1) push({});
out.calm = diag.dataset.state;
for (let i = 0; i < 8; i += 1) push({ rttMs: 640 });
out.alert = diag.dataset.state;
out.alertLabel = diag.getAttribute("aria-label");
out.closedBefore = panel.hidden;
diag.click();
out.open = !panel.hidden;
out.panelText = panel.text();
out.prompt = panel.find("fm-q-prompt").value;
panel.find("fm-q-actions").children[0].click();
await new Promise((resolve) => setTimeout(resolve, 5));
out.copied = copied === out.prompt;
const memory = JSON.parse(stored.get(Q.DIAG_KEY));
out.reports = memory.reports.length;
for (let i = 0; i < 8; i += 1) push({});
out.cleared = diag.dataset.state;
out.learned = JSON.parse(stored.get(Q.DIAG_KEY)).fixes;
const input = panel.find("fm-q-note-input");
input.value = "LESSON: plug in the cable";
panel.children.at(-1).children[1].click();
out.notes = JSON.parse(stored.get(Q.DIAG_KEY)).notes;
out.promptAfter = panel.find("fm-q-prompt").value;
diag.click();
out.closed = panel.hidden;
for (let i = 0; i < 8; i += 1) push({ lossPct: 12 });
diag.click();
widget.destroy();
out.failed = JSON.parse(stored.get(Q.DIAG_KEY)).fixes;
out.removed = host.children.length;
console.log(JSON.stringify(out));
'''


def _run(harness):
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    run = subprocess.run(
        ["node", "--input-type=module", "-e", harness, str(MODULE)],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert run.returncode == 0, run.stderr[-2000:]
    return json.loads(run.stdout)


@pytest.fixture(scope="module")
def diagnostics():
    return _run(DIAGNOSTICS_HARNESS)


@pytest.fixture(scope="module")
def widget():
    return _run(WIDGET_HARNESS)


def test_diagnosis_finds_sustained_problems_only(diagnostics):
    assert diagnostics["healthy"]["severity"] == "ok" and diagnostics["healthy"]["issues"] == []
    assert diagnostics["early"]["severity"] == "unknown"
    network = {issue["id"]: issue["severity"] for issue in diagnostics["network"]["issues"]}
    assert network == {"latency": "bad", "loss": "warn", "jitter": "warn"}
    assert diagnostics["network"]["severity"] == "bad"
    assert diagnostics["network"]["issues"][0]["id"] == "latency"
    video = {issue["id"]: issue["severity"] for issue in diagnostics["video"]["issues"]}
    assert video == {"cpu": "warn", "sendFps": "bad", "receive": "warn", "face": "bad"}
    face = next(issue for issue in diagnostics["video"]["issues"] if issue["id"] == "face")
    assert "58 ms a frame" in face["detail"] and "14 frames a second" in face["detail"]
    assert diagnostics["cameraOff"]["issues"] == []
    assert diagnostics["theirCameraOff"]["issues"] == []
    assert diagnostics["voiceOnly"]["issues"] == []


def test_fix_order_learns_from_outcomes(diagnostics):
    assert diagnostics["firstRank"] == ["wired", "traffic", "vpn", "relay"]
    assert diagnostics["learnedRank"] == ["vpn", "traffic", "relay", "wired"]
    assert diagnostics["noteSaved"] is True and diagnostics["noteEmpty"] is False
    assert diagnostics["notes"] == ["turn the VPN off first"]
    assert diagnostics["reports"] == 20
    assert diagnostics["reloaded"]["latency:vpn"] == {"worked": 1, "failed": 0}
    assert diagnostics["broken"] == {"reports": [], "fixes": {}, "notes": []}


def test_prompt_carries_measurements_history_and_lessons(diagnostics):
    prompt = diagnostics["prompt"]
    assert prompt.startswith("Prompt v2 · learned from 1 earlier report on this device.")
    assert "620 ms round trip, 4.0% loss, 70 ms jitter" in prompt
    assert "TestBrowser/1; 8 CPU cores" in prompt
    assert "1. High latency (severe, network)." in prompt
    assert "Seen in 1 of 1 earlier reports on this device." in prompt
    assert "Worked here before: Turn off a VPN or proxy for the call. (1×)" in prompt
    assert "Tried here without clearing it: Use a wired connection" in prompt
    assert "- the office Wi-Fi drops packets; use the cable" in prompt
    assert 'starts with "LESSON:"' in prompt
    assert "No problem was detected automatically." in diagnostics["healthyPrompt"]


def test_meter_measures_the_face_stage(diagnostics):
    face = diagnostics["face"]
    assert face["first"] is None
    assert face["fps"] == pytest.approx(20)
    assert face["ms"] == pytest.approx(45)
    assert face["level"] == 2


def test_widget_turns_red_and_explains(widget):
    assert widget["calm"] == "ok"
    assert widget["alert"] == "bad"
    assert widget["alertLabel"].startswith("Call diagnostics: 1 problem found: high latency")
    assert widget["closedBefore"] is True and widget["open"] is True
    assert "High latency" in widget["panelText"] and "Use a wired connection" in widget["panelText"]
    assert widget["prompt"].startswith("Prompt v1 · learned from 0 earlier reports")
    assert widget["copied"] is True
    assert widget["reports"] == 1


def test_widget_learns_what_cleared_the_problem(widget):
    assert widget["cleared"] == "ok"
    assert widget["learned"] == {"latency:wired": {"worked": 1, "failed": 0}}
    assert widget["notes"] == ["plug in the cable"]
    assert "- plug in the cable" in widget["promptAfter"]
    assert widget["closed"] is True
    assert widget["failed"]["loss:wired"] == {"worked": 0, "failed": 1}
    assert widget["removed"] == 0
