export const QUALITY_LEVEL_MS = 100;
export const QUALITY_STATS_MS = 1000;
export const QUALITY_LEVEL_SAMPLES = 50;
export const QUALITY_STATS_SAMPLES = 60;
export const QUALITY_OPEN_KEY = "fm.call.qualityOpen";
export const QUALITY_RTT_STEPS = Object.freeze([150, 300, 500]);
export const QUALITY_LOSS_STEPS = Object.freeze([1, 3, 8]);
export const QUALITY_JITTER_STEPS = Object.freeze([30, 60, 100]);

const GRADE_LABELS = Object.freeze({
  good: "Good connection",
  fair: "Fair connection",
  poor: "Poor connection",
  unknown: "Measuring connection",
});
const LIMIT_LABELS = Object.freeze({
  bandwidth: "limited by bandwidth",
  cpu: "limited by CPU",
  other: "limited",
});
const QUALITY_COLORS = Object.freeze({
  you: "#58a6ff",
  them: "#98a2ae",
  line: "#e6edf3",
  grid: "rgba(152, 162, 174, 0.35)",
  good: "#3fb950",
  fair: "#d29922",
  poor: "#f85149",
});

function finite(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function maxOf(current, value) {
  if (value === null || value === undefined) return current;
  return current === null ? value : Math.max(current, value);
}

function statsList(report) {
  if (!report) return [];
  if (Array.isArray(report)) return report;
  if (typeof report.values === "function") return [...report.values()];
  const list = [];
  if (typeof report.forEach === "function") report.forEach((value) => list.push(value));
  return list;
}

function emptySample(at) {
  return {
    at,
    rttMs: null,
    lossPct: null,
    jitterMs: null,
    sendKbps: null,
    recvKbps: null,
    inWidth: null,
    inHeight: null,
    inFps: null,
    outWidth: null,
    outHeight: null,
    outFps: null,
    limit: "",
    micLevel: null,
    peerLevel: null,
    faceFps: null,
    faceMs: null,
    faceLevel: null,
  };
}

function rate(bytes, previousBytes, seconds) {
  if (bytes === null || previousBytes === null || previousBytes === undefined || seconds <= 0) return null;
  return Math.max(0, ((bytes - previousBytes) * 8) / 1000 / seconds);
}

export function summarizeRtcStats(reports, previous = null, at = Date.now()) {
  const sample = emptySample(at);
  const counters = { at, sent: 0, received: 0, lost: 0, packets: 0, frames: new Map() };
  const seconds = previous ? (at - previous.at) / 1000 : 0;
  let sentSeen = false;
  let receivedSeen = false;
  let inArea = 0;
  let outArea = 0;
  let remoteLoss = null;
  const list = Array.isArray(reports) && reports.some((item) => item && !item.type) ? reports : [reports];
  list.forEach((report, index) => {
    const entries = statsList(report);
    const byId = new Map(entries.map((entry) => [entry.id, entry]));
    let pair = null;
    for (const entry of entries) {
      if (entry.type === "transport" && entry.selectedCandidatePairId) pair = byId.get(entry.selectedCandidatePairId) || pair;
    }
    if (!pair) {
      pair = entries.find((entry) => entry.type === "candidate-pair" && entry.state === "succeeded" &&
        (entry.selected || entry.nominated)) || null;
    }
    const pairRtt = finite(pair?.currentRoundTripTime);
    if (pairRtt !== null) sample.rttMs = maxOf(sample.rttMs, pairRtt * 1000);
    for (const entry of entries) {
      const kind = entry.kind || entry.mediaType || "";
      if (entry.type === "outbound-rtp") {
        const bytes = finite(entry.bytesSent);
        if (bytes !== null) {
          counters.sent += bytes;
          sentSeen = true;
        }
        if (kind === "video") {
          const width = finite(entry.frameWidth) || 0;
          const height = finite(entry.frameHeight) || 0;
          if (width * height >= outArea && width * height > 0) {
            outArea = width * height;
            sample.outWidth = width;
            sample.outHeight = height;
            sample.outFps = finite(entry.framesPerSecond);
            const reason = String(entry.qualityLimitationReason || "");
            sample.limit = reason && reason !== "none" ? reason : "";
          }
        }
      } else if (entry.type === "inbound-rtp") {
        const bytes = finite(entry.bytesReceived);
        if (bytes !== null) {
          counters.received += bytes;
          receivedSeen = true;
        }
        const lost = finite(entry.packetsLost);
        const packets = finite(entry.packetsReceived);
        if (lost !== null) counters.lost += Math.max(0, lost);
        if (packets !== null) counters.packets += packets;
        if (kind === "audio") {
          const jitter = finite(entry.jitter);
          if (jitter !== null) sample.jitterMs = maxOf(sample.jitterMs, jitter * 1000);
          const level = finite(entry.audioLevel);
          if (level !== null) sample.peerLevel = maxOf(sample.peerLevel, level);
        } else if (kind === "video") {
          const width = finite(entry.frameWidth) || 0;
          const height = finite(entry.frameHeight) || 0;
          const key = index + ":" + entry.id;
          const decoded = finite(entry.framesDecoded);
          if (decoded !== null) counters.frames.set(key, decoded);
          let fps = finite(entry.framesPerSecond);
          const before = previous?.frames?.get(key);
          if (fps === null && decoded !== null && before !== undefined && seconds > 0) fps = Math.max(0, (decoded - before) / seconds);
          if (width * height >= inArea && width * height > 0) {
            inArea = width * height;
            sample.inWidth = width;
            sample.inHeight = height;
            sample.inFps = fps;
          }
        }
      } else if (entry.type === "remote-inbound-rtp") {
        const fraction = finite(entry.fractionLost);
        if (fraction !== null) remoteLoss = maxOf(remoteLoss, fraction * 100);
        const rtt = finite(entry.roundTripTime);
        if (rtt !== null && pairRtt === null) sample.rttMs = maxOf(sample.rttMs, rtt * 1000);
      } else if (entry.type === "media-source" && kind === "audio") {
        const level = finite(entry.audioLevel);
        if (level !== null) sample.micLevel = maxOf(sample.micLevel, level);
      }
    }
  });
  if (previous) {
    sample.sendKbps = sentSeen ? rate(counters.sent, previous.sent, seconds) : null;
    sample.recvKbps = receivedSeen ? rate(counters.received, previous.received, seconds) : null;
    const lost = counters.lost - (previous.lost || 0);
    const packets = counters.packets - (previous.packets || 0);
    if (lost >= 0 && packets >= 0 && lost + packets > 0) sample.lossPct = (lost * 100) / (lost + packets);
    else if (packets === 0 && lost === 0 && receivedSeen) sample.lossPct = 0;
  }
  if (remoteLoss !== null) sample.lossPct = maxOf(sample.lossPct, remoteLoss);
  return { sample, counters };
}

export function summarizeFmmStats(stats, previous = null, at = Date.now()) {
  const sample = emptySample(at);
  const counters = { at, sent: 0, received: 0, frames: new Map() };
  if (!stats) return { sample, counters };
  const seconds = previous ? (at - previous.at) / 1000 : 0;
  const transport = stats.transport || {};
  const rtt = finite(transport.currentRoundTripTime);
  if (rtt !== null && rtt > 0) sample.rttMs = rtt * 1000;
  const loss = finite(transport.loss);
  if (loss !== null) sample.lossPct = loss * 100;
  let inArea = 0;
  for (const entry of stats.outbound || []) {
    counters.sent += finite(entry.bytesSent) || 0;
    if (entry.kind === "video" && entry.representation !== "off") {
      const width = finite(entry.frameWidth) || 0;
      const height = finite(entry.frameHeight) || 0;
      if (width && height) {
        sample.outWidth = width;
        sample.outHeight = height;
      }
      const frames = finite(entry.framesSent);
      if (frames !== null) counters.frames.set("out", frames);
      const before = previous?.frames?.get("out");
      if (frames !== null && before !== undefined && seconds > 0) sample.outFps = Math.max(0, (frames - before) / seconds);
    }
  }
  for (const entry of stats.inbound || []) {
    if (entry.kind === "audio") continue;
    counters.received += finite(entry.bytesReceived) || 0;
    const key = String(entry.peer || "") + ":" + String(entry.ssrc ?? "");
    const decoded = finite(entry.framesDecoded);
    if (decoded !== null) counters.frames.set(key, decoded);
    const before = previous?.frames?.get(key);
    const fps = decoded !== null && before !== undefined && seconds > 0 ? Math.max(0, (decoded - before) / seconds) : null;
    const width = finite(entry.frameWidth) || 0;
    const height = finite(entry.frameHeight) || 0;
    if (width * height >= inArea && width * height > 0) {
      inArea = width * height;
      sample.inWidth = width;
      sample.inHeight = height;
      sample.inFps = fps;
    }
  }
  if (previous) {
    sample.sendKbps = rate(counters.sent, previous.sent, seconds);
    sample.recvKbps = rate(counters.received, previous.received, seconds);
  }
  return { sample, counters };
}

export function rtcStatsReader(connections) {
  let previous = null;
  return async () => {
    const list = [].concat(connections() || []).filter((pc) => pc && typeof pc.getStats === "function");
    if (!list.length) {
      previous = null;
      return null;
    }
    const reports = await Promise.all(list.map((pc) => pc.getStats().catch(() => null)));
    const result = summarizeRtcStats(reports.filter(Boolean), previous, Date.now());
    previous = result.counters;
    return result.sample;
  };
}

export function fmmStatsReader(session) {
  let previous = null;
  return async () => {
    const current = session();
    if (!current || typeof current.stats !== "function") {
      previous = null;
      return null;
    }
    const stats = await current.stats();
    if (!stats) return null;
    const result = summarizeFmmStats(stats, previous, Date.now());
    previous = result.counters;
    return result.sample;
  };
}

function step(value, steps) {
  if (value === null || value === undefined) return 0;
  let penalty = 0;
  for (const limit of steps) if (value >= limit) penalty += 1;
  return penalty;
}

export function qualityBars(sample) {
  if (!sample || (sample.rttMs === null && sample.lossPct === null)) return 0;
  const penalty = Math.max(
    step(sample.rttMs, QUALITY_RTT_STEPS),
    step(sample.lossPct, QUALITY_LOSS_STEPS),
    step(sample.jitterMs, QUALITY_JITTER_STEPS),
  );
  return Math.max(1, 4 - penalty);
}

export function gradeCallQuality(sample) {
  const bars = qualityBars(sample);
  if (!bars) return "unknown";
  if (bars === 4) return "good";
  if (bars === 1) return "poor";
  return "fair";
}

export function qualityGradeLabel(grade) {
  return GRADE_LABELS[grade] || GRADE_LABELS.unknown;
}

export function levelFromSamples(data) {
  if (!data || !data.length) return 0;
  let sum = 0;
  for (let i = 0; i < data.length; i += 1) sum += data[i] * data[i];
  const rms = Math.sqrt(sum / data.length);
  if (rms <= 0) return 0;
  const db = 20 * Math.log10(rms);
  return Math.min(1, Math.max(0, (db + 60) / 60));
}

function formatKbps(kbps) {
  if (kbps === null || kbps === undefined) return "";
  if (kbps >= 1000) return (kbps / 1000).toFixed(1) + " Mb/s";
  return Math.round(kbps) + " kb/s";
}

export function describeNetwork(sample) {
  if (!sample || sample.rttMs === null) return "Measuring…";
  const parts = [Math.round(sample.rttMs) + " ms"];
  if (sample.lossPct !== null) parts.push(sample.lossPct.toFixed(sample.lossPct < 10 ? 1 : 0) + "% loss");
  if (sample.jitterMs !== null) parts.push(Math.round(sample.jitterMs) + " ms jitter");
  return parts.join(" · ");
}

export function describeVideo(sample, direction = "in") {
  if (!sample) return "";
  const width = direction === "in" ? sample.inWidth : sample.outWidth;
  const height = direction === "in" ? sample.inHeight : sample.outHeight;
  const fps = direction === "in" ? sample.inFps : sample.outFps;
  const kbps = direction === "in" ? sample.recvKbps : sample.sendKbps;
  const parts = [];
  if (width && height) parts.push(width + "×" + height);
  if (fps !== null && fps !== undefined) parts.push(Math.round(fps) + " fps");
  const speed = formatKbps(kbps);
  if (speed) parts.push(speed);
  if (direction === "out" && sample.limit) parts.push(LIMIT_LABELS[sample.limit] || LIMIT_LABELS.other);
  return parts.join(" · ");
}

export function qualityEvent(sample, levels) {
  const grade = gradeCallQuality(sample);
  const round = (value, digits = 0) => (value === null || value === undefined ? null : Number(value.toFixed(digits)));
  return {
    event: "quality",
    grade,
    bars: qualityBars(sample),
    rttMs: round(sample?.rttMs),
    lossPct: round(sample?.lossPct, 1),
    jitterMs: round(sample?.jitterMs),
    sendKbps: round(sample?.sendKbps),
    recvKbps: round(sample?.recvKbps),
    inWidth: sample?.inWidth ?? null,
    inHeight: sample?.inHeight ?? null,
    inFps: round(sample?.inFps),
    outWidth: sample?.outWidth ?? null,
    outHeight: sample?.outHeight ?? null,
    outFps: round(sample?.outFps),
    limit: sample?.limit || "",
    mic: (levels || []).map((entry) => round(entry.mic, 2)),
    peer: (levels || []).map((entry) => (entry.peer === null ? null : round(entry.peer, 2))),
  };
}

export function createLevelTap(track, context) {
  if (!track || !context || typeof context.createMediaStreamSource !== "function") return null;
  const Stream = globalThis.MediaStream;
  if (typeof Stream !== "function") return null;
  let source;
  let analyser;
  let sink = null;
  try {
    source = context.createMediaStreamSource(new Stream([track]));
    analyser = context.createAnalyser();
    analyser.fftSize = 1024;
    source.connect(analyser);
    if (typeof context.createGain === "function" && context.destination) {
      sink = context.createGain();
      sink.gain.value = 0;
      analyser.connect(sink);
      sink.connect(context.destination);
    }
  } catch (_) {
    return null;
  }
  const data = new Float32Array(analyser.fftSize);
  return {
    track,
    level() {
      if (track.readyState === "ended" || track.enabled === false) return 0;
      analyser.getFloatTimeDomainData(data);
      return levelFromSamples(data);
    },
    stop() {
      try {
        source.disconnect();
        sink?.disconnect();
      } catch (_) {}
    },
  };
}

export function createCallQualityMeter({
  readStats = null,
  tracks = () => ({ mic: null, peers: [] }),
  face = () => null,
  scope = globalThis,
  levelMs = QUALITY_LEVEL_MS,
  statsMs = QUALITY_STATS_MS,
  levelSamples = QUALITY_LEVEL_SAMPLES,
  statsSamples = QUALITY_STATS_SAMPLES,
  AudioContextImpl = scope.AudioContext || scope.webkitAudioContext,
} = {}) {
  const levels = [];
  const samples = [];
  const listeners = new Set();
  const taps = new Map();
  let context = null;
  let latest = null;
  let drained = 0;
  let reading = false;
  let stopped = false;
  let faceBefore = null;

  function audioContext() {
    if (context || !AudioContextImpl) return context;
    try {
      context = new AudioContextImpl();
      if (context.state === "suspended") context.resume?.().catch?.(() => {});
    } catch (_) {
      context = null;
    }
    return context;
  }

  function syncTaps() {
    const wanted = tracks() || {};
    const live = new Map();
    const mic = wanted.mic && wanted.mic.readyState !== "ended" ? wanted.mic : null;
    if (mic) live.set("mic:" + mic.id, { role: "mic", track: mic });
    for (const track of wanted.peers || []) {
      if (track && track.kind === "audio" && track.readyState !== "ended") live.set("peer:" + track.id, { role: "peer", track });
    }
    for (const [key, tap] of taps) {
      if (!live.has(key)) {
        tap.stop();
        taps.delete(key);
      }
    }
    for (const [key, entry] of live) {
      if (taps.has(key)) continue;
      const tap = createLevelTap(entry.track, audioContext());
      if (tap) taps.set(key, { ...tap, role: entry.role });
    }
    return { hasMic: Boolean(mic), hasPeers: [...live.values()].some((entry) => entry.role === "peer") };
  }

  let present = syncTaps();

  function emit() {
    for (const listener of listeners) {
      try {
        listener(api);
      } catch (_) {}
    }
  }

  function tickLevels() {
    if (stopped) return;
    let mic = null;
    let peer = null;
    for (const tap of taps.values()) {
      const value = tap.level();
      if (tap.role === "mic") mic = Math.max(mic || 0, value);
      else peer = Math.max(peer || 0, value);
    }
    if (mic === null) mic = present.hasMic ? latest?.micLevel ?? 0 : 0;
    if (peer === null) peer = latest?.peerLevel ?? null;
    levels.push({ at: Date.now(), mic, peer });
    if (levels.length > levelSamples) levels.splice(0, levels.length - levelSamples);
    drained = Math.min(drained + 1, levelSamples);
    emit();
  }

  function measureFace(sample) {
    let stats = null;
    try {
      stats = face();
    } catch (_) {}
    const frames = finite(stats?.frames);
    const spent = finite(stats?.frameMs);
    if (frames === null || spent === null) {
      faceBefore = null;
      return;
    }
    const before = faceBefore;
    faceBefore = { at: sample.at, frames, spent };
    if (!before || frames <= before.frames || sample.at <= before.at || spent < before.spent) return;
    sample.faceFps = ((frames - before.frames) * 1000) / (sample.at - before.at);
    sample.faceMs = (spent - before.spent) / (frames - before.frames);
    sample.faceLevel = finite(stats.level);
  }

  async function tickStats() {
    if (stopped) return;
    present = syncTaps();
    if (!readStats || reading) return;
    reading = true;
    try {
      const sample = await readStats();
      if (stopped || !sample) return;
      measureFace(sample);
      latest = sample;
      samples.push(sample);
      if (samples.length > statsSamples) samples.splice(0, samples.length - statsSamples);
      emit();
    } catch (_) {
    } finally {
      reading = false;
    }
  }

  const levelTimer = scope.setInterval(tickLevels, levelMs);
  const statsTimer = scope.setInterval(() => void tickStats(), statsMs);
  void tickStats();

  const api = {
    get latest() { return latest; },
    get samples() { return samples; },
    get levels() { return levels; },
    get hasStats() { return Boolean(readStats); },
    grade: () => gradeCallQuality(latest),
    drain() {
      const fresh = levels.slice(levels.length - drained);
      drained = 0;
      return fresh;
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    stop() {
      if (stopped) return;
      stopped = true;
      scope.clearInterval(levelTimer);
      scope.clearInterval(statsTimer);
      for (const tap of taps.values()) tap.stop();
      taps.clear();
      listeners.clear();
      try {
        context?.close?.();
      } catch (_) {}
      context = null;
    },
  };
  return api;
}

export const DIAG_KEY = "fm.call.diagnostics";
export const DIAG_WINDOW = 8;
export const DIAG_MIN_SAMPLES = 3;
export const DIAG_REPORTS = 20;
export const DIAG_NOTES = 8;
export const DIAG_NOTE_CHARS = 280;
export const VIDEO_MIN_FPS = 15;
export const VIDEO_MIN_HEIGHT = 360;
export const VIDEO_MIN_KBPS = 100;
export const FACE_SLOW_MS = 34;
export const FACE_BAD_MS = 50;

const DIAG_FIXES = Object.freeze({
  wired: "Use a wired connection, or move closer to the Wi-Fi router.",
  traffic: "Pause downloads, uploads, cloud backups and video streaming on this network.",
  vpn: "Turn off a VPN or proxy for the call.",
  relay: "If the call says Relayed, this network blocks direct connections: try another network, or allow UDP on this one.",
  band: "Use 5 GHz Wi-Fi instead of 2.4 GHz, away from microwaves and Bluetooth speakers.",
  router: "Restart the Wi-Fi router.",
  camera: "Turn your camera off for a minute so your voice gets the whole connection.",
  tabs: "Close other tabs and apps, especially other calls, games and screen recorders.",
  filters: "Turn off the face, background or outfit filter.",
  power: "Plug the device in and turn off battery saver.",
  hardware: "Turn on hardware acceleration in the browser's settings and restart the browser.",
  light: "Add light in front of you: cameras lower their frame rate in the dark.",
  cameraApps: "Close other apps that use the camera.",
  theirSide: "Ask them to press their diagnostics button: poor incoming video is usually their connection or device.",
  background: "Choose None or Blur as the background instead of a picture.",
  faceOff: "Turn the face off when the call needs to be smooth.",
});

const DIAG_ISSUES = Object.freeze({
  latency: { title: "High latency", area: "network", cause: "Sound and video take a long time to arrive, so you talk over each other.", fixes: ["wired", "traffic", "vpn", "relay"] },
  loss: { title: "Packet loss", area: "network", cause: "Parts of the call are lost on the way, so voice drops out and video freezes or blurs.", fixes: ["wired", "band", "traffic", "router"] },
  jitter: { title: "Unsteady connection", area: "network", cause: "Packets arrive unevenly, so the call buffers more and voice can sound choppy.", fixes: ["traffic", "wired", "band", "vpn"] },
  bandwidth: { title: "Upload too slow for your video", area: "encoder", cause: "The browser is lowering your video quality because the upload cannot keep up.", fixes: ["traffic", "wired", "camera", "filters"] },
  cpu: { title: "Device too busy", area: "encoder", cause: "The browser is lowering your video quality because the processor cannot encode it in time.", fixes: ["tabs", "filters", "power", "hardware"] },
  sendFps: { title: "Your video is choppy", area: "camera", cause: "Your video leaves this device at a low frame rate.", fixes: ["light", "cameraApps", "filters", "power"] },
  receive: { title: "Their video is poor", area: "network", cause: "The other side's video arrives small or at a low frame rate.", fixes: ["theirSide", "traffic", "wired"] },
  face: { title: "Face filter is slow", area: "face filter", cause: "The face filter needs too long for each frame, so your face lags behind you.", fixes: ["tabs", "power", "hardware", "background", "faceOff"] },
});

function median(values) {
  if (!values.length) return null;
  const sorted = values.slice().sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

function mean(values) {
  return values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : null;
}

export function diagnoseCall(samples, { video = false, sending = true, window = DIAG_WINDOW } = {}) {
  const last = (samples || []).slice(-window).filter(Boolean);
  const pick = (key) => last.map((sample) => sample[key]).filter((value) => value !== null && value !== undefined && Number.isFinite(value));
  const need = Math.min(DIAG_MIN_SAMPLES, window);
  const metrics = {
    samples: last.length,
    rttMs: median(pick("rttMs")),
    lossPct: mean(pick("lossPct")),
    jitterMs: median(pick("jitterMs")),
    sendKbps: median(pick("sendKbps")),
    recvKbps: median(pick("recvKbps")),
    outFps: median(pick("outFps")),
    outHeight: median(pick("outHeight")),
    inFps: median(pick("inFps")),
    inHeight: median(pick("inHeight")),
    faceMs: median(pick("faceMs")),
    faceFps: median(pick("faceFps")),
    faceLevel: last.length ? last[last.length - 1].faceLevel ?? null : null,
  };
  const issues = [];
  const add = (id, severity, detail) => {
    const issue = DIAG_ISSUES[id];
    issues.push({ id, severity, title: issue.title, area: issue.area, cause: issue.cause, detail, fixes: issue.fixes.map((fix) => ({ id: fix, text: DIAG_FIXES[fix] })) });
  };
  const round = (value) => Math.round(value);
  if (pick("rttMs").length >= need && metrics.rttMs >= QUALITY_RTT_STEPS[1]) {
    add("latency", metrics.rttMs >= QUALITY_RTT_STEPS[2] ? "bad" : "warn", `Round trip ${round(metrics.rttMs)} ms; under ${QUALITY_RTT_STEPS[0]} ms feels natural.`);
  }
  if (pick("lossPct").length >= need && metrics.lossPct >= QUALITY_LOSS_STEPS[1]) {
    add("loss", metrics.lossPct >= QUALITY_LOSS_STEPS[2] ? "bad" : "warn", `${metrics.lossPct.toFixed(1)}% of packets lost; under ${QUALITY_LOSS_STEPS[0]}% is fine.`);
  }
  if (pick("jitterMs").length >= need && metrics.jitterMs >= QUALITY_JITTER_STEPS[1]) {
    add("jitter", metrics.jitterMs >= QUALITY_JITTER_STEPS[2] ? "bad" : "warn", `${round(metrics.jitterMs)} ms jitter; under ${QUALITY_JITTER_STEPS[0]} ms is fine.`);
  }
  if (video && sending && last.length >= need) {
    const limited = (reason) => last.filter((sample) => sample.limit === reason).length * 2 > last.length;
    const out = describeVideo({ ...last[last.length - 1], limit: "", outFps: metrics.outFps, sendKbps: metrics.sendKbps }, "out") || "reduced";
    if (limited("bandwidth")) add("bandwidth", "warn", `Video out is ${out}, limited by bandwidth.`);
    if (limited("cpu")) add("cpu", "warn", `Video out is ${out}, limited by CPU.`);
    if (pick("outFps").length >= need && metrics.outFps < VIDEO_MIN_FPS) {
      add("sendFps", metrics.outFps < VIDEO_MIN_FPS / 2 ? "bad" : "warn", `Your video leaves at ${round(metrics.outFps)} fps; ${VIDEO_MIN_FPS} or more looks smooth.`);
    }
  }
  if (video && pick("recvKbps").length >= need && metrics.recvKbps >= VIDEO_MIN_KBPS) {
    const slow = pick("inFps").length >= need && metrics.inFps < VIDEO_MIN_FPS;
    const small = pick("inHeight").length >= need && metrics.inHeight < VIDEO_MIN_HEIGHT;
    if (slow || small) {
      const parts = [];
      if (small) parts.push(`${round(metrics.inHeight)}p`);
      if (slow) parts.push(`${round(metrics.inFps)} fps`);
      add("receive", "warn", `Their video arrives at ${parts.join(", ")}.`);
    }
  }
  if (pick("faceMs").length >= need && metrics.faceMs > FACE_SLOW_MS) {
    const fps = metrics.faceFps === null ? "" : ` and draws ${round(metrics.faceFps)} frames a second`;
    add("face", metrics.faceMs > FACE_BAD_MS ? "bad" : "warn", `The face filter takes ${round(metrics.faceMs)} ms a frame${fps}; under ${FACE_SLOW_MS} ms keeps up with the camera.`);
  }
  issues.sort((a, b) => (a.severity === b.severity ? 0 : a.severity === "bad" ? -1 : 1));
  const severity = issues.some((issue) => issue.severity === "bad") ? "bad" : issues.length ? "warn" : last.length >= need ? "ok" : "unknown";
  return { at: last.length ? last[last.length - 1].at : null, severity, issues, metrics };
}

export function emptyDiagnosticsMemory() {
  return { reports: [], fixes: {}, notes: [] };
}

export function loadDiagnosticsMemory(storage) {
  try {
    const raw = JSON.parse(storage?.getItem(DIAG_KEY) || "null");
    if (!raw || typeof raw !== "object") return emptyDiagnosticsMemory();
    return {
      reports: Array.isArray(raw.reports) ? raw.reports.slice(-DIAG_REPORTS) : [],
      fixes: raw.fixes && typeof raw.fixes === "object" ? raw.fixes : {},
      notes: Array.isArray(raw.notes) ? raw.notes.filter((note) => typeof note === "string").slice(-DIAG_NOTES) : [],
    };
  } catch (_) {
    return emptyDiagnosticsMemory();
  }
}

export function saveDiagnosticsMemory(storage, memory) {
  try {
    storage?.setItem(DIAG_KEY, JSON.stringify(memory));
  } catch (_) {}
}

function fixRecord(memory, issueId, fixId) {
  const record = memory.fixes[issueId + ":" + fixId];
  return { worked: Number(record?.worked) || 0, failed: Number(record?.failed) || 0 };
}

export function rankFixes(issue, memory) {
  const score = (fix) => {
    const record = fixRecord(memory, issue.id, fix.id);
    return record.worked * 2 - record.failed;
  };
  return issue.fixes
    .map((fix, index) => ({ ...fix, ...fixRecord(memory, issue.id, fix.id), index }))
    .sort((a, b) => score(b) - score(a) || a.index - b.index)
    .map(({ index, ...fix }) => fix);
}

export function recordFixOutcome(memory, issueId, fixId, worked) {
  const key = issueId + ":" + fixId;
  const record = fixRecord(memory, issueId, fixId);
  if (worked) record.worked += 1;
  else record.failed += 1;
  memory.fixes[key] = record;
  return memory;
}

export function rememberReport(memory, diagnosis) {
  const metrics = {};
  for (const [key, value] of Object.entries(diagnosis.metrics || {})) {
    if (value !== null && value !== undefined && key !== "samples") metrics[key] = Number(Number(value).toFixed(1));
  }
  memory.reports.push({ at: diagnosis.at ?? Date.now(), issues: diagnosis.issues.map((issue) => issue.id), metrics });
  if (memory.reports.length > DIAG_REPORTS) memory.reports.splice(0, memory.reports.length - DIAG_REPORTS);
  return memory;
}

export function rememberNote(memory, text) {
  const note = String(text || "").replace(/^\s*lesson\s*:\s*/i, "").replace(/\s+/g, " ").trim().slice(0, DIAG_NOTE_CHARS);
  if (!note) return false;
  memory.notes = memory.notes.filter((entry) => entry !== note);
  memory.notes.push(note);
  if (memory.notes.length > DIAG_NOTES) memory.notes.splice(0, memory.notes.length - DIAG_NOTES);
  return true;
}

export function describeDiagnosticsMetrics(metrics) {
  const lines = [];
  const value = (number, digits = 0) => Number(number).toFixed(digits);
  if (metrics.rttMs !== null) lines.push(`Network: ${value(metrics.rttMs)} ms round trip` + (metrics.lossPct !== null ? `, ${value(metrics.lossPct, 1)}% loss` : "") + (metrics.jitterMs !== null ? `, ${value(metrics.jitterMs)} ms jitter` : ""));
  if (metrics.outFps !== null || metrics.sendKbps !== null) lines.push(`Video out: ` + [metrics.outHeight !== null ? value(metrics.outHeight) + "p" : "", metrics.outFps !== null ? value(metrics.outFps) + " fps" : "", formatKbps(metrics.sendKbps)].filter(Boolean).join(", "));
  if (metrics.inFps !== null || metrics.recvKbps !== null) lines.push(`Video in: ` + [metrics.inHeight !== null ? value(metrics.inHeight) + "p" : "", metrics.inFps !== null ? value(metrics.inFps) + " fps" : "", formatKbps(metrics.recvKbps)].filter(Boolean).join(", "));
  if (metrics.faceMs !== null) lines.push(`Face filter: ${value(metrics.faceMs)} ms a frame` + (metrics.faceFps !== null ? `, ${value(metrics.faceFps)} fps` : "") + (metrics.faceLevel !== null ? `, quality step ${metrics.faceLevel}` : ""));
  return lines;
}

export function diagnosticsPrompt(diagnosis, memory, environment = {}) {
  const earlier = memory.reports.filter((report) => report.at !== diagnosis.at);
  const lines = [];
  lines.push(`Prompt v${earlier.length + 1} · learned from ${earlier.length} earlier report${earlier.length === 1 ? "" : "s"} on this device.`);
  lines.push("");
  lines.push("You are helping me fix the quality of a ForkMesh call (WebRTC in the browser, with an optional on-device face filter). Measurements from the last few seconds of my call:");
  for (const line of describeDiagnosticsMetrics(diagnosis.metrics)) lines.push("- " + line);
  const device = [environment.userAgent, environment.cores ? environment.cores + " CPU cores" : "", environment.memory ? environment.memory + " GB memory" : "", environment.connection].filter(Boolean);
  if (device.length) lines.push("- Device: " + device.join("; "));
  lines.push("");
  if (!diagnosis.issues.length) {
    lines.push("No problem was detected automatically. If the call still feels wrong to me, help me find out why.");
  } else {
    lines.push("Problems detected:");
    diagnosis.issues.forEach((issue, i) => {
      const seen = earlier.filter((report) => report.issues.includes(issue.id)).length;
      lines.push(`${i + 1}. ${issue.title} (${issue.severity === "bad" ? "severe" : "noticeable"}, ${issue.area}). ${issue.detail}`);
      if (seen) lines.push(`   Seen in ${seen} of ${earlier.length} earlier reports on this device.`);
      const ranked = rankFixes(issue, memory);
      const worked = ranked.filter((fix) => fix.worked > 0);
      const failed = ranked.filter((fix) => fix.failed > 0 && fix.worked === 0);
      if (worked.length) lines.push("   Worked here before: " + worked.map((fix) => `${fix.text} (${fix.worked}×)`).join(" "));
      if (failed.length) lines.push("   Tried here without clearing it: " + failed.map((fix) => `${fix.text} (${fix.failed}×)`).join(" "));
      lines.push("   Suggested order: " + ranked.map((fix) => fix.text).join(" "));
    });
  }
  if (memory.notes.length) {
    lines.push("");
    lines.push("Lessons saved from earlier calls on this device (newest last):");
    for (const note of memory.notes) lines.push("- " + note);
  }
  lines.push("");
  lines.push("Please:");
  lines.push("1. Say in one or two sentences what most likely causes this, using the numbers above.");
  lines.push("2. Give me the steps to fix it, most likely first. Start from what worked here before, and skip what was tried without clearing it unless something has changed.");
  lines.push("3. If it looks like a ForkMesh bug rather than my network or device, say which part (network, camera, encoder or face filter) and what to look for in chrome://webrtc-internals.");
  lines.push('4. End with one line that starts with "LESSON:" saying what to try first next time on this device. I will save it so the next version of this prompt starts from it.');
  return lines.join("\n");
}

export function diagnosticsEnvironment(scope = globalThis) {
  const navigatorRef = scope.navigator || {};
  const connection = navigatorRef.connection;
  const link = connection ? [connection.effectiveType, connection.downlink ? connection.downlink + " Mb/s down" : "", connection.rtt ? connection.rtt + " ms link" : ""].filter(Boolean).join(", ") : "";
  return {
    userAgent: String(navigatorRef.userAgent || ""),
    cores: Number(navigatorRef.hardwareConcurrency) || 0,
    memory: Number(navigatorRef.deviceMemory) || 0,
    connection: link,
  };
}

function canvasFor(documentRef, width, height, name) {
  const canvas = documentRef.createElement("canvas");
  canvas.className = "fm-q-chart";
  canvas.dataset.chart = name;
  canvas.style.width = width + "px";
  canvas.style.height = height + "px";
  canvas.setAttribute("role", "img");
  return canvas;
}

function prepare(canvas) {
  const ratio = Math.max(1, Math.min(3, globalThis.devicePixelRatio || 1));
  const width = parseFloat(canvas.style.width) || canvas.clientWidth || 48;
  const height = parseFloat(canvas.style.height) || canvas.clientHeight || 16;
  if (canvas.width !== Math.round(width * ratio)) canvas.width = Math.round(width * ratio);
  if (canvas.height !== Math.round(height * ratio)) canvas.height = Math.round(height * ratio);
  const context = canvas.getContext?.("2d");
  if (!context) return null;
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  return { context, width, height };
}

function readColors(element) {
  const style = globalThis.getComputedStyle?.(element);
  const pick = (name, fallback) => (style?.getPropertyValue("--fm-q-" + name) || "").trim() || fallback;
  const colors = {};
  for (const [name, fallback] of Object.entries(QUALITY_COLORS)) colors[name] = pick(name, fallback);
  return colors;
}

export function drawVoiceWave(canvas, levels, colors, count = QUALITY_LEVEL_SAMPLES) {
  const view = prepare(canvas);
  if (!view) return;
  const { context, width, height } = view;
  const middle = height / 2;
  const slot = width / count;
  const bar = Math.max(1, slot - 1);
  const recent = levels.slice(-count);
  const offset = count - recent.length;
  context.fillStyle = colors.grid;
  context.fillRect(0, middle - 0.5, width, 1);
  recent.forEach((entry, i) => {
    const x = (offset + i) * slot;
    const up = Math.max(0.5, (entry.mic || 0) * (middle - 1));
    context.fillStyle = colors.you;
    context.fillRect(x, middle - 0.5 - up, bar, up);
    if (entry.peer !== null && entry.peer !== undefined) {
      const down = Math.max(0.5, entry.peer * (middle - 1));
      context.fillStyle = colors.them;
      context.fillRect(x, middle + 0.5, bar, down);
    }
  });
}

export function drawSparkline(canvas, values, colors, { color = colors.line, floor = 0, marks = [] } = {}) {
  const view = prepare(canvas);
  if (!view) return;
  const { context, width, height } = view;
  const count = QUALITY_STATS_SAMPLES;
  const known = values.filter((value) => value !== null && value !== undefined);
  context.fillStyle = colors.grid;
  context.fillRect(0, height - 1, width, 1);
  const slot = width / Math.max(1, count - 1);
  const offset = count - values.length;
  marks.forEach((mark, i) => {
    if (!mark) return;
    context.fillStyle = mark;
    context.fillRect((offset + i) * slot - 1, 0, 2, height - 1);
  });
  if (!known.length) return;
  const top = Math.max(floor, ...known) * 1.15 || 1;
  context.strokeStyle = color;
  context.lineWidth = 1.5;
  context.lineJoin = "round";
  context.lineCap = "round";
  context.beginPath();
  let drawing = false;
  values.forEach((value, i) => {
    if (value === null || value === undefined) {
      drawing = false;
      return;
    }
    const x = (offset + i) * slot;
    const y = height - 2 - (value / top) * (height - 4);
    if (drawing) context.lineTo(x, y);
    else context.moveTo(x, y);
    drawing = true;
  });
  context.stroke();
}

export function mountCallQualityWidget(host, meter, {
  documentRef = globalThis.document,
  storage = globalThis.localStorage,
  video = () => false,
  sending = () => true,
  peerLabel = "Them",
  scope = globalThis,
  visible = () => !documentRef?.hidden,
} = {}) {
  if (!host || !meter || !documentRef) return null;
  const root = documentRef.createElement("div");
  root.className = "fm-q";
  root.dataset.grade = "unknown";
  const pill = documentRef.createElement("button");
  pill.type = "button";
  pill.className = "fm-q-pill";
  const bars = documentRef.createElement("span");
  bars.className = "fm-q-bars";
  bars.setAttribute("aria-hidden", "true");
  for (let i = 0; i < 4; i += 1) bars.append(documentRef.createElement("i"));
  const wave = canvasFor(documentRef, 40, 16, "wave");
  wave.setAttribute("aria-hidden", "true");
  const pillText = documentRef.createElement("span");
  pillText.className = "fm-q-text";
  pill.append(bars, wave, pillText);
  const panel = documentRef.createElement("div");
  panel.className = "fm-q-panel";
  const heading = documentRef.createElement("div");
  heading.className = "fm-q-heading";
  const rows = {};
  const row = (name, label, chartWidth) => {
    const line = documentRef.createElement("div");
    line.className = "fm-q-row";
    line.dataset.row = name;
    const title = documentRef.createElement("span");
    title.className = "fm-q-label";
    title.textContent = label;
    const chart = canvasFor(documentRef, chartWidth, 22, name);
    const value = documentRef.createElement("span");
    value.className = "fm-q-value";
    line.append(title, chart, value);
    panel.append(line);
    rows[name] = { line, chart, value };
  };
  panel.append(heading);
  row("voice", "Voice", 120);
  row("network", "Network", 120);
  row("video", "Video in", 120);
  row("send", "Video out", 120);
  const bar = documentRef.createElement("span");
  bar.className = "fm-q-bar";
  const diag = documentRef.createElement("button");
  diag.type = "button";
  diag.className = "fm-q-diag";
  diag.dataset.state = "unknown";
  diag.setAttribute("aria-expanded", "false");
  diag.title = "Show diagnostics and how to fix them";
  diag.setAttribute("aria-label", "Call diagnostics");
  const dot = documentRef.createElement("span");
  dot.className = "fm-q-dot";
  dot.setAttribute("aria-hidden", "true");
  const diagText = documentRef.createElement("span");
  diagText.className = "fm-q-diag-text";
  diagText.setAttribute("aria-hidden", "true");
  diagText.textContent = "Diagnose";
  diag.append(dot, diagText);
  bar.append(pill, diag);
  const diagPanel = documentRef.createElement("div");
  diagPanel.className = "fm-q-panel fm-q-diag-panel";
  diagPanel.hidden = true;
  const diagHeading = documentRef.createElement("div");
  diagHeading.className = "fm-q-heading";
  const issueList = documentRef.createElement("ol");
  issueList.className = "fm-q-issues";
  const promptLabel = documentRef.createElement("label");
  promptLabel.className = "fm-q-label";
  const promptBox = documentRef.createElement("textarea");
  promptBox.className = "fm-q-prompt";
  promptBox.readOnly = true;
  promptBox.rows = 6;
  promptBox.id = "fm-q-prompt-" + Math.random().toString(36).slice(2, 8);
  promptLabel.htmlFor = promptBox.id;
  const copy = documentRef.createElement("button");
  copy.type = "button";
  copy.className = "fm-q-action";
  copy.textContent = "Copy prompt";
  const noteInput = documentRef.createElement("input");
  noteInput.type = "text";
  noteInput.className = "fm-q-note-input";
  noteInput.maxLength = DIAG_NOTE_CHARS + 16;
  noteInput.placeholder = "Paste the LESSON line from the answer";
  noteInput.setAttribute("aria-label", "Lesson from the answer");
  const saveNote = documentRef.createElement("button");
  saveNote.type = "button";
  saveNote.className = "fm-q-action";
  saveNote.textContent = "Save lesson";
  const diagStatus = documentRef.createElement("span");
  diagStatus.className = "fm-q-status";
  diagStatus.setAttribute("role", "status");
  const actions = documentRef.createElement("div");
  actions.className = "fm-q-actions";
  actions.append(copy, diagStatus);
  const note = documentRef.createElement("div");
  note.className = "fm-q-actions";
  note.append(noteInput, saveNote);
  diagPanel.append(diagHeading, issueList, promptLabel, promptBox, actions, note);
  root.append(bar, panel, diagPanel);
  host.append(root);

  let memory = loadDiagnosticsMemory(storage);
  let diagnosis = diagnoseCall([]);
  let diagnosedFor = null;
  let diagOpen = false;
  const pending = new Map();

  let open = false;
  try {
    open = storage?.getItem(QUALITY_OPEN_KEY) === "1";
  } catch (_) {}
  let colors = readColors(root);
  let colorsAt = Date.now();
  let drawnFor;
  const written = new Map();

  function put(key, value, apply) {
    if (written.get(key) === value) return;
    written.set(key, value);
    apply(value);
  }

  function setOpen(value) {
    open = Boolean(value);
    root.dataset.open = open ? "true" : "false";
    panel.hidden = !open;
    pill.setAttribute("aria-expanded", open ? "true" : "false");
    try {
      storage?.setItem(QUALITY_OPEN_KEY, open ? "1" : "0");
    } catch (_) {}
    colors = readColors(root);
    drawnFor = undefined;
    render();
  }

  function refreshDiagnosis() {
    if (diag.hidden !== !meter.hasStats) diag.hidden = !meter.hasStats;
    if (!meter.hasStats || meter.latest === diagnosedFor) return;
    diagnosedFor = meter.latest;
    diagnosis = diagnoseCall(meter.samples, { video: video(), sending: sending() });
    diag.dataset.state = diagnosis.severity;
    const count = diagnosis.issues.length;
    const summary = count
      ? `${count} problem${count === 1 ? "" : "s"} found: ${diagnosis.issues.map((issue) => issue.title.toLowerCase()).join(", ")}`
      : diagnosis.severity === "ok" ? "No problems found" : "Checking the call";
    diag.title = summary + (diagOpen ? ". Hide diagnostics" : ". Show diagnostics and how to fix them");
    diag.setAttribute("aria-label", "Call diagnostics: " + summary);
    if (diagnosis.severity === "unknown" || !pending.size) return;
    const present = new Set(diagnosis.issues.map((issue) => issue.id));
    let learned = false;
    for (const [issueId, fixId] of pending) {
      if (present.has(issueId)) continue;
      recordFixOutcome(memory, issueId, fixId, true);
      pending.delete(issueId);
      learned = true;
    }
    if (learned) saveDiagnosticsMemory(storage, memory);
  }

  function renderDiagnostics() {
    const count = diagnosis.issues.length;
    diagHeading.textContent = count
      ? `${count} problem${count === 1 ? "" : "s"} found`
      : diagnosis.severity === "ok" ? "No problems found" : "Not enough measurements yet";
    const items = diagnosis.issues.map((issue) => {
      const item = documentRef.createElement("li");
      item.className = "fm-q-issue";
      item.dataset.severity = issue.severity;
      const title = documentRef.createElement("strong");
      title.textContent = issue.title;
      const detail = documentRef.createElement("p");
      detail.textContent = issue.detail + " " + issue.cause;
      const fixes = documentRef.createElement("ol");
      fixes.className = "fm-q-fixes";
      for (const fix of rankFixes(issue, memory)) {
        const step = documentRef.createElement("li");
        step.textContent = fix.text + (fix.worked ? " (worked here before)" : fix.failed ? " (tried here before)" : "");
        fixes.append(step);
      }
      item.append(title, detail, fixes);
      return item;
    });
    if (!count) {
      const item = documentRef.createElement("li");
      item.className = "fm-q-issue";
      item.dataset.severity = "ok";
      const detail = documentRef.createElement("p");
      detail.textContent = describeDiagnosticsMetrics(diagnosis.metrics).join(". ") || "Measurements start once the call connects.";
      item.append(detail);
      items.push(item);
    }
    issueList.replaceChildren(...items);
    const earlier = memory.reports.filter((report) => report.at !== diagnosis.at).length;
    promptLabel.textContent = `Prompt for an AI assistant · v${earlier + 1}, learned from ${earlier} earlier report${earlier === 1 ? "" : "s"}`;
    promptBox.value = diagnosticsPrompt(diagnosis, memory, diagnosticsEnvironment(scope));
  }

  function setDiagOpen(value) {
    diagOpen = Boolean(value);
    diagPanel.hidden = !diagOpen;
    diag.setAttribute("aria-expanded", diagOpen ? "true" : "false");
    root.dataset.diagnostics = diagOpen ? "true" : "false";
    if (!diagOpen) return;
    memory = loadDiagnosticsMemory(storage);
    diagnosis = diagnoseCall(meter.samples, { video: video(), sending: sending() });
    if (diagnosis.at !== null && memory.reports.at(-1)?.at !== diagnosis.at) rememberReport(memory, diagnosis);
    for (const issue of diagnosis.issues) {
      if (!pending.has(issue.id)) pending.set(issue.id, rankFixes(issue, memory)[0].id);
    }
    saveDiagnosticsMemory(storage, memory);
    diagStatus.textContent = "";
    renderDiagnostics();
  }

  function render() {
    refreshDiagnosis();
    if (!visible()) return;
    if (Date.now() - colorsAt > 1000) {
      colors = readColors(root);
      colorsAt = Date.now();
    }
    const latest = meter.latest;
    const grade = meter.hasStats ? gradeCallQuality(latest) : "unknown";
    const count = meter.hasStats ? qualityBars(latest) : 0;
    put("grade", grade, (value) => { root.dataset.grade = value; });
    put("stats", meter.hasStats ? "true" : "false", (value) => { root.dataset.stats = value; });
    put("bars", count, (value) => [...bars.children].forEach((bar, i) => bar.classList.toggle("is-on", i < value)));
    const network = meter.hasStats ? describeNetwork(latest) : "";
    const rtt = meter.hasStats && latest?.rttMs !== null && latest?.rttMs !== undefined
      ? Math.round(latest.rttMs) + " ms"
      : "";
    put("rtt", rtt, (value) => {
      pillText.textContent = value;
      pillText.hidden = !value;
    });
    put("barsHidden", !meter.hasStats, (value) => { bars.hidden = value; });
    const summary = meter.hasStats ? qualityGradeLabel(grade) + (network && latest ? " · " + network : "") : "Microphone level";
    put("summary", summary + (open ? "|open" : ""), () => {
      pill.title = summary + (open ? ". Hide live charts" : ". Show live charts");
      pill.setAttribute("aria-label", "Call quality: " + summary);
    });
    drawVoiceWave(wave, meter.levels, colors, 20);
    if (!open) return;
    put("heading", meter.hasStats ? qualityGradeLabel(grade) : "Microphone", (value) => { heading.textContent = value; });
    drawVoiceWave(rows.voice.chart, meter.levels, colors);
    const hasPeer = meter.levels.some((entry) => entry.peer !== null && entry.peer !== undefined);
    put("voice", hasPeer ? "You ▲ · " + peerLabel + " ▼" : "You", (value) => {
      rows.voice.value.textContent = value;
      rows.voice.value.title = value;
      rows.voice.chart.setAttribute("aria-label", "Voice levels over the last five seconds");
    });
    const showVideo = meter.hasStats && video();
    put("rows", String(meter.hasStats) + showVideo, () => {
      rows.network.line.hidden = !meter.hasStats;
      rows.video.line.hidden = !showVideo;
      rows.send.line.hidden = !showVideo;
    });
    if (!meter.hasStats || drawnFor === latest) return;
    drawnFor = latest;
    const samples = meter.samples;
    const marks = samples.map((sample) => {
      if (sample.lossPct === null || sample.lossPct < QUALITY_LOSS_STEPS[0]) return "";
      return sample.lossPct >= QUALITY_LOSS_STEPS[2] ? colors.poor : colors.fair;
    });
    drawSparkline(rows.network.chart, samples.map((sample) => sample.rttMs), colors, { floor: 100, marks });
    rows.network.value.textContent = network;
    rows.network.chart.setAttribute("aria-label", "Round-trip time over the last minute; ticks mark packet loss. " + network);
    if (!rows.video.line.hidden) {
      drawSparkline(rows.video.chart, samples.map((sample) => sample.recvKbps), colors, { floor: 200 });
      rows.video.value.textContent = describeVideo(latest, "in") || "No video";
      rows.video.chart.setAttribute("aria-label", "Incoming bitrate over the last minute. " + rows.video.value.textContent);
      drawSparkline(rows.send.chart, samples.map((sample) => sample.sendKbps), colors, { floor: 200 });
      rows.send.value.textContent = describeVideo(latest, "out") || "Camera off";
      rows.send.chart.setAttribute("aria-label", "Outgoing bitrate over the last minute. " + rows.send.value.textContent);
    }
    for (const entry of Object.values(rows)) entry.value.title = entry.value.textContent;
  }

  pill.addEventListener("click", () => setOpen(!open));
  diag.addEventListener("click", () => setDiagOpen(!diagOpen));
  copy.addEventListener("click", () => {
    const done = (text) => {
      diagStatus.textContent = text;
    };
    const write = scope.navigator?.clipboard?.writeText?.(promptBox.value);
    if (write?.then) {
      write.then(() => done("Copied. Paste it into your AI assistant."), () => {
        promptBox.select?.();
        done("Select the prompt and copy it.");
      });
    } else {
      promptBox.select?.();
      done("Select the prompt and copy it.");
    }
  });
  saveNote.addEventListener("click", () => {
    memory = loadDiagnosticsMemory(storage);
    if (!rememberNote(memory, noteInput.value)) {
      diagStatus.textContent = "Paste the LESSON line first.";
      return;
    }
    saveDiagnosticsMemory(storage, memory);
    noteInput.value = "";
    renderDiagnostics();
    diagStatus.textContent = "Saved. The next prompt starts from it.";
  });
  const unsubscribe = meter.subscribe(() => render());
  setOpen(open);

  return {
    element: root,
    render,
    setOpen,
    setDiagnosticsOpen: setDiagOpen,
    diagnosis: () => diagnosis,
    destroy() {
      unsubscribe();
      if (pending.size) {
        memory = loadDiagnosticsMemory(storage);
        for (const [issueId, fixId] of pending) recordFixOutcome(memory, issueId, fixId, false);
        pending.clear();
        saveDiagnosticsMemory(storage, memory);
      }
      root.remove();
    },
  };
}
