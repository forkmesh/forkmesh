import { writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const Q = await import(pathToFileURL(resolve(root, "app/public/call-quality-meter.js")).href);

const KEYS = [
  "rttMs", "lossPct", "jitterMs", "sendKbps", "recvKbps", "inWidth", "inHeight", "inFps",
  "outWidth", "outHeight", "outFps", "limit", "faceFps", "faceMs", "faceLevel",
];

function sample(at, fields = {}) {
  const out = { at };
  for (const key of KEYS) out[key] = key === "limit" ? "" : null;
  return Object.assign(out, fields);
}

function series(count, start, make) {
  return Array.from({ length: count }, (_, i) => sample(start + i * 1000, make(i)));
}

const good = (i) => ({ rttMs: 40 + i, lossPct: 0, jitterMs: 4, sendKbps: 900, recvKbps: 1100, outWidth: 1280, outHeight: 720, outFps: 30, inWidth: 1280, inHeight: 720, inFps: 30 });

const CASES = [
  { name: "empty", samples: [], options: {} },
  { name: "too few", samples: series(2, 1000, good), options: { video: true } },
  { name: "healthy video", samples: series(8, 5000, good), options: { video: true } },
  { name: "healthy voice", samples: series(5, 9000, (i) => ({ rttMs: 60, lossPct: 0.2 * i, jitterMs: 8, sendKbps: 96, recvKbps: 96 })), options: {} },
  { name: "slow network", samples: series(8, 20000, (i) => ({ rttMs: [290, 310, 320, 305, 330, 300, 301, 312][i], lossPct: [3, 3.5, 3, 3.5, 3, 3.5, 3, 3.5][i], jitterMs: 65 + i })), options: { video: true } },
  { name: "terrible network", samples: series(6, 30000, (i) => ({ rttMs: 520 + i * 10, lossPct: 9 + i, jitterMs: 120, recvKbps: 80 })), options: {} },
  { name: "ties", samples: series(4, 40000, (i) => ({ rttMs: [300, 301, 300, 302][i], lossPct: [3, 3.5, 3, 3.5][i], jitterMs: [60, 61, 62, 61][i] })), options: {} },
  { name: "bandwidth limited", samples: series(8, 50000, (i) => ({ ...good(i), outWidth: 640, outHeight: 360, outFps: [12, 13, 11, 12, 14, 12, 12, 13][i], sendKbps: [250, 260, 270, 255, 240, 265, 250, 245][i], limit: i < 5 ? "bandwidth" : "" })), options: { video: true } },
  { name: "cpu limited and very choppy", samples: series(8, 60000, (i) => ({ ...good(i), outFps: 5 + (i % 2), limit: i % 4 === 0 ? "" : "cpu" })), options: { video: true } },
  { name: "not sending", samples: series(8, 70000, (i) => ({ ...good(i), outFps: 3, limit: "cpu" })), options: { video: true, sending: false } },
  { name: "poor incoming video", samples: series(8, 80000, (i) => ({ ...good(i), inHeight: 240, inWidth: 320, inFps: 9.5 + i * 0.25, recvKbps: 180 })), options: { video: true } },
  { name: "incoming under the floor", samples: series(8, 90000, (i) => ({ ...good(i), inHeight: 180, inFps: 6, recvKbps: 60 })), options: { video: true } },
  { name: "slow face", samples: series(8, 100000, (i) => ({ ...good(i), faceMs: 38 + i, faceFps: 22.5, faceLevel: 2 })), options: { video: true } },
  { name: "very slow face", samples: series(5, 110000, (i) => ({ ...good(i), faceMs: 70, faceLevel: 1.5 })), options: { video: false } },
  { name: "long history", samples: series(14, 120000, (i) => ({ rttMs: i < 6 ? 900 : 50, lossPct: i < 6 ? 20 : 0.25, jitterMs: 10, sendKbps: 1500.5, recvKbps: 999.6 })), options: { video: true } },
  { name: "gaps", samples: series(8, 130000, (i) => ({ rttMs: i % 3 ? null : 450, lossPct: i % 2 ? null : 4.05, jitterMs: null, sendKbps: i === 7 ? 12000 : null })), options: { video: true } },
  { name: "small window", samples: series(8, 140000, (i) => ({ rttMs: 300 + i })), options: { window: 2 } },
];

function memoryStates() {
  const empty = Q.emptyDiagnosticsMemory();
  const learned = Q.emptyDiagnosticsMemory();
  Q.recordFixOutcome(learned, "latency", "vpn", true);
  Q.recordFixOutcome(learned, "latency", "vpn", true);
  Q.recordFixOutcome(learned, "latency", "wired", false);
  Q.recordFixOutcome(learned, "loss", "band", true);
  Q.recordFixOutcome(learned, "loss", "wired", false);
  Q.recordFixOutcome(learned, "loss", "wired", false);
  Q.recordFixOutcome(learned, "loss", "router", false);
  Q.recordFixOutcome(learned, "jitter", "traffic", false);
  Q.recordFixOutcome(learned, "face", "faceOff", true);
  Q.recordFixOutcome(learned, "face", "tabs", false);
  learned.reports.push({ at: 1, issues: ["latency", "loss"], metrics: { rttMs: 410 } });
  learned.reports.push({ at: 2, issues: ["jitter"], metrics: {} });
  learned.reports.push({ at: 27000, issues: ["latency", "jitter"], metrics: { rttMs: 312.5 } });
  learned.notes.push("Turn off the VPN first; it doubled the round trip last time.");
  learned.notes.push("Wi-Fi in the kitchen is fine.");
  const one = Q.emptyDiagnosticsMemory();
  one.reports.push({ at: 3, issues: ["cpu"], metrics: {} });
  return [empty, learned, one];
}

const ENVIRONMENTS = [
  {},
  { userAgent: "ForkMesh iOS 1.0 (iPhone15,2; iOS 26.0)", cores: 6, memory: 5.5, connection: "" },
  { userAgent: "", cores: 0, memory: 8, connection: "wifi, 50 Mb/s down" },
];

const NOTES = [
  "LESSON: Turn off the VPN before calling.",
  "  lesson :   Plug   in\nthe phone  ",
  "Lesson:",
  "   ",
  "Wi-Fi in the kitchen is fine.",
  "x".repeat(400),
  "LESSON:LESSON: nested",
];

const clone = (value) => JSON.parse(JSON.stringify(value));

function remembered(memory, diagnosis) {
  const report = Q.rememberReport(clone(memory), diagnosis).reports.at(-1);
  return diagnosis.at === null ? { ...report, at: null } : report;
}

export function fixture() {
  const memories = memoryStates();
  const cases = CASES.map((entry) => {
    const diagnosis = Q.diagnoseCall(entry.samples, entry.options);
    return {
      name: entry.name,
      samples: entry.samples,
      options: entry.options,
      diagnosis: clone(diagnosis),
      lines: Q.describeDiagnosticsMetrics(diagnosis.metrics),
      ranked: memories.map((memory) => diagnosis.issues.map((issue) => Q.rankFixes(issue, memory))),
      prompts: memories.map((memory, index) => Q.diagnosticsPrompt(diagnosis, memory, ENVIRONMENTS[index % ENVIRONMENTS.length])),
      remembered: remembered(memories[1], diagnosis),
    };
  });
  const notes = [];
  let memory = clone(memories[1]);
  for (const text of NOTES) {
    const ok = Q.rememberNote(memory, text);
    notes.push({ text, ok, notes: clone(memory.notes) });
  }
  let crowded = Q.emptyDiagnosticsMemory();
  for (let i = 0; i < 25; i += 1) Q.rememberReport(crowded, Q.diagnoseCall(series(3, i * 10, () => ({ rttMs: 400 }))));
  for (let i = 0; i < 10; i += 1) Q.rememberNote(crowded, "note " + i);
  return {
    constants: {
      window: Q.DIAG_WINDOW, minSamples: Q.DIAG_MIN_SAMPLES, reports: Q.DIAG_REPORTS, notes: Q.DIAG_NOTES,
      noteChars: Q.DIAG_NOTE_CHARS, videoMinFps: Q.VIDEO_MIN_FPS, videoMinHeight: Q.VIDEO_MIN_HEIGHT,
      videoMinKbps: Q.VIDEO_MIN_KBPS, faceSlowMs: Q.FACE_SLOW_MS, faceBadMs: Q.FACE_BAD_MS, key: Q.DIAG_KEY,
    },
    memories,
    environments: ENVIRONMENTS,
    cases,
    notes,
    crowded: { reports: crowded.reports.length, firstAt: crowded.reports[0].at, notes: crowded.notes },
    stored: JSON.stringify(memories[1]),
  };
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const text = JSON.stringify(fixture(), null, 1) + "\n";
  for (const path of process.argv.slice(2)) writeFileSync(path, text);
  if (process.argv.length < 3) process.stdout.write(text);
}
