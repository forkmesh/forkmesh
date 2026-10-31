export const CALL_AUDIO_CONSTRAINTS = Object.freeze({
  echoCancellation: true,
  noiseSuppression: true,
  autoGainControl: true,
  channelCount: 1,
  sampleRate: 48000,
});
export const CALL_VIDEO_CONSTRAINTS = Object.freeze({
  facingMode: "user",
  width: { ideal: 1280 },
  height: { ideal: 720 },
  frameRate: { ideal: 30, max: 30 },
});
export const CALL_VIDEO_MAX_BITRATE = 2_500_000;
export const CALL_AUDIO_MAX_BITRATE = 128_000;
export const CALL_AUDIO_PRIORITY = "high";
export const CALL_VIDEO_PRIORITY = "medium";
export const CALL_AUDIO_CODEC_ORDER = Object.freeze(["audio/red", "audio/opus"]);
export const CALL_TEST_CLIP_MS = 10_000;
export const CALL_ROUTE_CHECK_MS = 4000;

const TEST_VIDEO_MIMES = [
  "video/mp4;codecs=avc1.42E028,mp4a.40.2",
  "video/mp4;codecs=avc1,mp4a.40.2",
  "video/mp4",
  "video/webm;codecs=vp9,opus",
  "video/webm;codecs=vp8,opus",
  "video/webm",
];
const TEST_AUDIO_MIMES = [
  "audio/mp4;codecs=mp4a.40.2",
  "audio/mp4",
  "audio/webm;codecs=opus",
  "audio/ogg;codecs=opus",
  "audio/webm",
];
const TEST_VIDEO_BPS = 2_500_000;
const TEST_AUDIO_BPS = 128_000;
const OPUS_FMTP = [
  ["useinbandfec", "1"],
  ["usedtx", "0"],
  ["stereo", "0"],
  ["maxplaybackrate", "48000"],
  ["maxaveragebitrate", String(CALL_AUDIO_MAX_BITRATE)],
  ["sprop-maxcapturerate", "48000"],
];

export function callMediaConstraints(media) {
  return {
    audio: { ...CALL_AUDIO_CONSTRAINTS },
    video: media === "video" ? { ...CALL_VIDEO_CONSTRAINTS } : false,
  };
}

export async function tuneCallSender(sender, kind) {
  if (!sender || typeof sender.getParameters !== "function" || typeof sender.setParameters !== "function") return false;
  const apply = async (full) => {
    const params = sender.getParameters();
    if (!params?.encodings?.length) return false;
    for (const encoding of params.encodings) {
      encoding.maxBitrate = kind === "video" ? CALL_VIDEO_MAX_BITRATE : CALL_AUDIO_MAX_BITRATE;
      if (kind === "video") encoding.maxFramerate = 30;
      if (full) {
        const priority = kind === "video" ? CALL_VIDEO_PRIORITY : CALL_AUDIO_PRIORITY;
        encoding.priority = priority;
        encoding.networkPriority = priority;
      }
    }
    if (full && kind === "video") params.degradationPreference = "balanced";
    await sender.setParameters(params);
    return true;
  };
  try {
    return await apply(true);
  } catch (_) {
    try {
      return await apply(false);
    } catch (_) {
      return false;
    }
  }
}

export async function tuneCallSenders(pc) {
  for (const transceiver of pc?.getTransceivers?.() || []) {
    const kind = transceiver.receiver?.track?.kind || transceiver.sender?.track?.kind || "";
    if (kind === "audio" || kind === "video") await tuneCallSender(transceiver.sender, kind);
  }
}

export function orderCallAudioCodecs(codecs) {
  const list = Array.isArray(codecs) ? codecs : [];
  const rank = (codec) => {
    const at = CALL_AUDIO_CODEC_ORDER.indexOf(String(codec?.mimeType || "").toLowerCase());
    return at < 0 ? CALL_AUDIO_CODEC_ORDER.length : at;
  };
  if (!list.some((codec) => rank(codec) === 0)) return null;
  return list.map((codec, index) => ({ codec, index }))
    .sort((a, b) => rank(a.codec) - rank(b.codec) || a.index - b.index)
    .map((entry) => entry.codec);
}

export function preferCallAudioCodecs(pc, RTCRtpSenderImpl = globalThis.RTCRtpSender) {
  if (typeof RTCRtpSenderImpl?.getCapabilities !== "function") return false;
  let ordered = null;
  try {
    ordered = orderCallAudioCodecs(RTCRtpSenderImpl.getCapabilities("audio")?.codecs);
  } catch (_) {
    return false;
  }
  if (!ordered) return false;
  let applied = false;
  for (const transceiver of pc?.getTransceivers?.() || []) {
    const kind = transceiver.receiver?.track?.kind || transceiver.sender?.track?.kind || "";
    if (kind !== "audio" || typeof transceiver.setCodecPreferences !== "function") continue;
    try {
      transceiver.setCodecPreferences(ordered);
      applied = true;
    } catch (_) {}
  }
  return applied;
}

export function tuneCallSdp(description) {
  if (!description || typeof description.sdp !== "string") return description;
  const lines = description.sdp.split("\r\n");
  const rtpmap = lines.find((line) => /^a=rtpmap:\d+ opus\/48000/i.test(line));
  if (!rtpmap) return { type: description.type, sdp: description.sdp };
  const payload = rtpmap.slice("a=rtpmap:".length).split(" ")[0];
  const prefix = `a=fmtp:${payload} `;
  const at = lines.findIndex((line) => line.startsWith(prefix));
  const params = new Map();
  if (at >= 0) {
    for (const part of lines[at].slice(prefix.length).split(";")) {
      const [key, ...rest] = part.trim().split("=");
      if (key) params.set(key, rest.join("="));
    }
  }
  for (const [key, value] of OPUS_FMTP) params.set(key, value);
  const line = prefix + [...params].map(([key, value]) => `${key}=${value}`).join(";");
  if (at >= 0) lines[at] = line;
  else lines.splice(lines.indexOf(rtpmap) + 1, 0, line);
  return { type: description.type, sdp: lines.join("\r\n") };
}

export async function readCallRoute(pc) {
  if (!pc || typeof pc.getStats !== "function") return null;
  const stats = await pc.getStats();
  const byId = new Map();
  stats.forEach((report) => byId.set(report.id, report));
  let pair = null;
  stats.forEach((report) => {
    if (report.type === "transport" && report.selectedCandidatePairId) {
      pair = byId.get(report.selectedCandidatePairId) || pair;
    }
  });
  if (!pair) {
    stats.forEach((report) => {
      if (!pair && report.type === "candidate-pair" && report.state === "succeeded" &&
          (report.selected || report.nominated)) pair = report;
    });
  }
  if (!pair) return null;
  const local = String(byId.get(pair.localCandidateId)?.candidateType || "");
  const remote = String(byId.get(pair.remoteCandidateId)?.candidateType || "");
  return {
    route: local === "relay" || remote === "relay" ? "relay" : "direct",
    local,
    remote,
    protocol: String(byId.get(pair.localCandidateId)?.protocol || ""),
  };
}

export function callRouteLabel(route) {
  if (route?.route === "relay") return "Relayed · through a TURN relay server (third party)";
  if (route?.route === "direct") return "Direct · peer-to-peer, no server in between";
  return "";
}

export function pickTestClipMime(video, MediaRecorderImpl = globalThis.MediaRecorder) {
  if (typeof MediaRecorderImpl?.isTypeSupported !== "function") return "";
  return (video ? TEST_VIDEO_MIMES : TEST_AUDIO_MIMES).find((type) => MediaRecorderImpl.isTypeSupported(type)) || "";
}

export function recordTestClip(tracks, {
  maxMs = CALL_TEST_CLIP_MS,
  onTick = () => {},
  MediaRecorderImpl = globalThis.MediaRecorder,
  MediaStreamImpl = globalThis.MediaStream,
  BlobImpl = globalThis.Blob,
  now = () => globalThis.performance.now(),
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
} = {}) {
  const live = (tracks || []).filter((track) => track && track.readyState !== "ended");
  if (!live.length || !MediaRecorderImpl) throw new Error("nothing_to_record");
  const video = live.some((track) => track.kind === "video");
  const mimeType = pickTestClipMime(video, MediaRecorderImpl);
  const options = video
    ? { videoBitsPerSecond: TEST_VIDEO_BPS, audioBitsPerSecond: TEST_AUDIO_BPS }
    : { audioBitsPerSecond: TEST_AUDIO_BPS };
  if (mimeType) options.mimeType = mimeType;
  const recorder = new MediaRecorderImpl(new MediaStreamImpl(live), options);
  const chunks = [];
  let startedAt = 0;
  let timer = null;
  let settle;
  let fail;
  const done = new Promise((resolve, reject) => {
    settle = resolve;
    fail = reject;
  });
  const finish = () => {
    if (timer !== null) clearIntervalImpl(timer);
    timer = null;
  };
  recorder.addEventListener("dataavailable", (event) => {
    if (event.data?.size) chunks.push(event.data);
  });
  recorder.addEventListener("error", (event) => {
    finish();
    fail(event.error || new Error("recording_failed"));
  });
  recorder.addEventListener("stop", () => {
    finish();
    const type = (recorder.mimeType || mimeType || (video ? "video/webm" : "audio/webm")).toLowerCase();
    settle({
      blob: new BlobImpl(chunks, { type }),
      mime: type,
      video,
      durationMs: Math.round(Math.min(maxMs, now() - startedAt)),
    });
  });
  const stop = () => {
    if (recorder.state !== "inactive") recorder.stop();
    return done;
  };
  recorder.start(500);
  startedAt = now();
  timer = setIntervalImpl(() => {
    const elapsed = now() - startedAt;
    onTick(Math.min(elapsed, maxMs), maxMs);
    if (elapsed >= maxMs) stop();
  }, 200);
  return { stop, done, video };
}
