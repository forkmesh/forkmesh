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
