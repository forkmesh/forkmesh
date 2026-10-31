import {
  newClient, OutFrame,
  KindAudio, KindVideo, KindAuth,
  EventJoined, EventTrackAdded, EventTrackRemoved, EventKeyframeRequest, EventError, EventRecover,
  EventSendRepresentation, EventReferencePicture, EventRepresentation,
  RepOff, RepVideo, RepFace,
  AudioPlay, AudioConceal, AudioInsert, AudioDRED, AudioWaiting,
} from "./client.js";
import { append, Join, KeyframeRequest } from "./wire.js";
import { createFrameSealer, createFrameOpener, CLEAR_AUDIO, CLEAR_VIDEO } from "./fmm-seal.js";

const TICK_MS = 5;
const AUTH_EVERY_MS = 200;
const STANDBY_PING_MS = 5000;
const DATAGRAM_CEILING = 1152;
const SAMPLE_RATE = 48000;
const FRAME_SAMPLES = 960;
const FRAME_US = 20000;
const PLAYOUT_BACKLOG = 10;
const MIN_DEAD_MS = 400;
const STALL_MS = 40;
const QUIET_MS = 300;
const MAX_DEAD_MS = 1000;
const LONE_DEAD_MS = 10000;
const SENT_RECENT_MS = 100;
const RECONFIGURE_MS = 250;
const RECONFIGURE_RATIO = 0.08;
const AUDIO_RECONFIGURE_MS = 2000;
const MAX_ENCODE_QUEUE = 2;
const PADDING = Uint8Array.of(0x12, 0, 0, 0);
const CODECS = {
  h264: { encode: "avc1.42E01F", decode: "avc1.42E01F", avc: { format: "annexb" } },
  vp8: { encode: "vp8", decode: "vp8" },
};
const TIERS = [
  { width: 640, height: 360, from: 450000 },
  { width: 480, height: 270, from: 250000 },
  { width: 320, height: 180, from: 0 },
];

const epoch = performance.now();
const nowUs = () => Math.round((performance.now() - epoch) * 1000);
const nowMs = () => performance.now();
const post = (message, transfer = []) => self.postMessage(message, transfer);

const state = {
  started: false,
  closing: false,
  config: null,
  access: null,
  session: null,
  standby: null,
  moving: false,
  moved: false,
  peer: "",
  audioStream: 0,
  authStream: 0,
  videoTrack: 0,
  encodings: [],
  faceStreams: new Set(),
  remotes: new Map(),
  audioPort: null,
  audioEncoder: null,
  audioBitrate: 0,
  audioConfiguredAt: 0,
  audioLoss: 0,
  muted: false,
  cameraOff: false,
  codec: "vp8",
  lastAuthFlush: 0,
  timer: 0,
  sealer: createFrameSealer(),
  opener: null,
  counters: {
    audioSent: 0, audioBytesSent: 0, videoFramesSent: 0, videoBytesSent: 0, keyframesSent: 0,
    faceFramesSent: 0, referencesSent: 0, keyRequestsIn: 0, keyRequestsOut: 0, recoverAsKeyframe: 0, failovers: 0, failoverMs: 0, deadReason: "", deadAtWall: 0,
  },
};

state.opener = createFrameOpener({
  now: () => Date.now(),
  onState: (peer, auth) => post({ type: "auth", peer, state: auth }),
});

function hashesFrom(list) {
  return (list || []).map((text) => ({
    algorithm: "sha-256",
    value: Uint8Array.from(atob(text), (c) => c.charCodeAt(0)),
  }));
}

function concatBytes(a, b) {
  if (!a || a.length === 0) return b;
  const out = new Uint8Array(a.length + b.length);
  out.set(a);
  out.set(b, a.length);
  return out;
}

async function openSession(endpoint) {
  const wt = new WebTransport(endpoint.url, {
    serverCertificateHashes: hashesFrom(endpoint.hashes),
    congestionControl: "low-latency",
    requireUnreliable: true,
  });
  await wt.ready;
  try {
    wt.datagrams.incomingHighWaterMark = 512;
    wt.datagrams.outgoingHighWaterMark = 512;
    wt.datagrams.outgoingMaxAge = 400;
  } catch (_) {}
  const stream = await wt.createBidirectionalStream();
  const link = {
    endpoint,
    wt,
    control: stream.writable.getWriter(),
    datagrams: wt.datagrams.writable.getWriter(),
    reader: stream.readable.getReader(),
    datagramReader: wt.datagrams.readable.getReader(),
    client: null,
    closed: false,
    lastHeard: nowMs(),
    lastSent: 0,
    heard: false,
    pending: [],
    writeChain: Promise.resolve(),
  };
  wt.closed.then((info) => {
    link.closed = true;
    if (link === state.standby) post({ type: "log", text: "standby closed " + JSON.stringify(info || {}) });
  }, (error) => {
    link.closed = true;
    if (link === state.standby) post({ type: "log", text: "standby failed " + String(error?.message || error) });
  });
  return link;
}

function transportFor(link) {
  return {
    sendDatagram(p) {
      if (link.closed) return true;
      const t = nowMs();
      if (t - link.lastSent > QUIET_MS) link.sendingSince = t;
      link.lastSent = t;
      link.datagrams.write(p.slice()).catch(() => {});
      return false;
    },
    maxDatagramSize() {
      const size = Number(link.wt.datagrams.maxDatagramSize) || 1200;
      return Math.max(0, Math.min(size - 4, DATAGRAM_CEILING));
    },
    sendControl(p) {
      if (link.closed) return true;
      const copy = p.slice();
      link.writeChain = link.writeChain.then(() => link.control.write(copy)).catch(() => {});
      return false;
    },
  };
}

async function readControl(link) {
  try {
    for (;;) {
      const { value, done } = await link.reader.read();
      if (done) break;
      if (link.client) link.client.handleControl(value, nowUs());
      else link.pending.push(value);
    }
  } catch (_) {}
  link.closed = true;
}

async function readDatagrams(link) {
  try {
    for (;;) {
      const { value, done } = await link.datagramReader.read();
      if (done) break;
      link.lastHeard = nowMs();
      link.heard = true;
      if (link.client) link.client.handleDatagram(value, nowUs());
    }
  } catch (_) {}
  link.closed = true;
}

function attachClient(link, ticket) {
  const cfg = state.config.client || {};
  link.client = newClient(transportFor(link), cfg);
  for (const chunk of link.pending.splice(0)) link.client.handleControl(chunk, nowUs());
  link.client.join(ticket, nowUs());
  return link.client;
}

function publishAll(client) {
  const t = nowUs();
  state.audioStream = client.publishAudio("audio/opus", t);
  client.setAudioRedundancy(state.audioStream, 0);
  state.authStream = client.publishAuth("auth/e2ee-v2", t);
  state.encodings = [];
  state.faceStreams.clear();
  state.videoTrack = 0;
  if (!state.config.video) return;
  const count = Math.max(1, Math.min(2, state.config.simulcast || 1));
  state.videoTrack = client.publishVideoTrack("video/" + state.codec, 3, count, Boolean(state.config.face), t);
  for (let i = 0; i < count; i += 1) {
    const stream = count === 1 ? state.videoTrack : client.encodingStream(state.videoTrack, i);
    const top = i === count - 1;
    state.encodings.push({
      stream,
      maxWidth: top ? 640 : 320,
      maxHeight: top ? 360 : 180,
      encoder: null,
      width: 0,
      height: 0,
      bitrate: 0,
      configuredAt: 0,
      keyNext: true,
      chain: Promise.resolve(),
      canvas: null,
    });
  }
  const face = client.faceStream(state.videoTrack);
  if (face) state.faceStreams.add(face);
  state.faceStreams.add(state.videoTrack);
}

function resetRemotes() {
  for (const remote of state.remotes.values()) closeRemote(remote);
  state.remotes.clear();
  if (state.audioPort) state.audioPort.postMessage({ t: "reset" });
}

function closeRemote(remote) {
  try { remote.decoder?.close(); } catch (_) {}
  remote.decoder = null;
  if (remote.kind === KindAudio && state.audioPort) state.audioPort.postMessage({ t: "drop", id: remote.stream });
  if (remote.kind === KindVideo) post({ type: "track-removed", stream: remote.stream, peer: remote.peer, kind: "video" });
}

function remoteCodec(name) {
  const tail = String(name || "").split("/")[1] || "";
  return CODECS[tail] ? tail : "vp8";
}

function addRemote(client, ev) {
  const kind = ev.track;
  const remote = {
    stream: ev.stream,
    kind,
    peer: ev.peer,
    name: ev.name,
    codec: remoteCodec(ev.name),
    decoder: null,
    decoderReady: false,
    toc: 0x78,
    chain: Promise.resolve(),
    started: false,
    pulled: 0,
    played: 0,
    concealed: 0,
    concealEvents: 0,
    concealing: false,
    waiting: 0,
    dropped: 0,
    bytes: 0,
    frames: 0,
    decoded: 0,
    keyDecoded: 0,
    width: 0,
    height: 0,
    rep: RepVideo,
    needKey: true,
    lastDecodedAt: 0,
    intervals: 0,
    intervalSum: 0,
    freezes: 0,
    freezeMs: 0,
    delaySum: 0,
    delayCount: 0,
    sealDrops: 0,
    firstPlayWall: 0,
  };
  state.remotes.set(ev.stream, remote);
  client.subscribe(ev.stream, 7, nowUs());
  if (kind === KindAudio) {
    remote.decoder = new AudioDecoder({
      output: (data) => {
        const pcm = new Float32Array(data.numberOfFrames);
        data.copyTo(pcm, { planeIndex: 0, format: "f32-planar" });
        data.close();
        if (state.audioPort) state.audioPort.postMessage({ t: "pcm", id: remote.stream, pcm }, [pcm.buffer]);
      },
      error: () => { remote.decoder = null; },
    });
    remote.decoder.configure({ codec: "opus", sampleRate: SAMPLE_RATE, numberOfChannels: 1 });
  } else if (kind === KindVideo) {
    post({ type: "track", stream: ev.stream, kind: "video", peer: ev.peer, name: ev.name });
  }
}

function videoDecoderFor(remote) {
  if (remote.decoder && remote.decoder.state !== "closed") return remote.decoder;
  remote.decoderReady = false;
  remote.decoder = new VideoDecoder({
    output: (frame) => {
      const at = nowMs();
      if (remote.lastDecodedAt) {
        const gap = at - remote.lastDecodedAt;
        remote.intervals += 1;
        remote.intervalSum += gap;
        const mean = remote.intervalSum / remote.intervals;
        if (gap > Math.max(3 * mean, mean + 150)) {
          remote.freezes += 1;
          remote.freezeMs += gap;
        }
      }
      remote.lastDecodedAt = at;
      remote.decoded += 1;
      remote.width = frame.displayWidth;
      remote.height = frame.displayHeight;
      post({ type: "video", stream: remote.stream, peer: remote.peer, frame }, [frame]);
    },
    error: (error) => {
      remote.decoder = null;
      needRemoteKey(remote);
      remote.decodeErrors = (remote.decodeErrors || 0) + 1;
      if (remote.decodeErrors <= 3) post({ type: "log", text: "video decoder " + remote.codec + ": " + String(error?.message || error) });
    },
  });
  remote.decoder.configure({ codec: CODECS[remote.codec].decode, optimizeForLatency: true });
  return remote.decoder;
}

function handleEvents(client) {
  for (;;) {
    const ev = client.pollEvent();
    if (!ev) return;
    switch (ev.kind) {
      case EventJoined:
        state.peer = ev.peer;
        post({ type: "joined", peer: ev.peer, moved: state.moved });
        break;
      case EventTrackAdded:
        addRemote(client, ev);
        break;
      case EventTrackRemoved: {
        const remote = state.remotes.get(ev.stream);
        if (remote) {
          closeRemote(remote);
          state.remotes.delete(ev.stream);
        }
        break;
      }
      case EventKeyframeRequest:
        state.counters.keyRequestsIn += 1;
        requestKey(ev.stream);
        break;
      case EventRecover:
        state.counters.recoverAsKeyframe += 1;
        requestKey(ev.stream);
        break;
      case EventSendRepresentation:
        if (ev.rep === RepVideo) requestKey(ev.stream);
        post({ type: "send-face", stream: ev.stream, on: ev.rep === RepFace });
        break;
      case EventReferencePicture:
        post({ type: "reference-request", stream: ev.stream, maxBytes: ev.maxBytes });
        break;
      case EventRepresentation: {
        const remote = state.remotes.get(ev.stream);
        if (remote) {
          if (ev.rep === RepVideo && remote.rep === RepFace) needRemoteKey(remote);
          remote.rep = ev.rep;
          post({ type: "representation", stream: ev.stream, peer: remote.peer, rep: ev.rep === RepFace ? "face" : ev.rep === RepOff ? "off" : "video" });
        }
        break;
      }
      case EventError:
        post({ type: "error", code: ev.code, text: ev.text });
        break;
      default:
        break;
    }
  }
}

function needRemoteKey(remote) {
  remote.needKey = true;
  const link = state.session;
  if (!link || link.closed || !link.client) return;
  if (remote.keyAskedAt && nowMs() - remote.keyAskedAt < 500) return;
  remote.keyAskedAt = nowMs();
  state.counters.keyRequestsOut += 1;
  try {
    transportFor(link).sendControl(append(null, new KeyframeRequest({ stream: remote.stream })));
  } catch (_) {}
}

function requestKey(stream) {
  if (state.faceStreams.has(stream) && state.session?.client?.sendingRepresentation(stream) === RepFace) {
    post({ type: "face-key", stream });
  }
  for (const enc of state.encodings) if (enc.stream === stream) enc.keyNext = true;
}

function pollVideo(client) {
  const t = nowUs();
  for (const remote of state.remotes.values()) {
    if (remote.kind !== KindVideo) continue;
    for (;;) {
      const f = client.pollVideo(remote.stream, t);
      if (!f) break;
      remote.frames += 1;
      remote.bytes += f.data.length;
      const face = f.face;
      const keyframe = f.keyframe;
      const timestamp = f.timestamp;
      const data = f.data;
      remote.chain = remote.chain.then(async () => {
        const { plain, drop } = await state.opener.open(remote.peer, data, CLEAR_VIDEO);
        if (!plain) {
          remote.sealDrops += 1;
          if (!face) needRemoteKey(remote);
          return;
        }
        if (face) {
          post({ type: "face", stream: remote.stream, peer: remote.peer, data: plain, reference: keyframe }, [plain.buffer]);
          return;
        }
        if (remote.needKey && !keyframe) {
          remote.dropped += 1;
          needRemoteKey(remote);
          return;
        }
        const decoder = videoDecoderFor(remote);
        if (keyframe) {
          remote.needKey = false;
          remote.keyDecoded += 1;
        }
        try {
          decoder.decode(new EncodedVideoChunk({ type: keyframe ? "key" : "delta", timestamp, data: plain }));
        } catch (_) {
          needRemoteKey(remote);
          remote.dropped += 1;
        }
        void drop;
      });
    }
  }
}

function pollAuth(client) {
  for (const remote of state.remotes.values()) {
    if (remote.kind !== KindAuth) continue;
    for (;;) {
      const bytes = client.pollAuth(remote.stream);
      if (!bytes) break;
      void state.opener.accept(remote.peer, bytes);
    }
  }
}

async function flushAuth(client) {
  const messages = await state.sealer.flush();
  if (!state.authStream || client !== state.session?.client) return;
  for (const m of messages) {
    try { client.sendAuth(state.authStream, m, nowUs()); } catch (_) {}
  }
}

function pullDue() {
  const real = nowUs();
  if (!state.audioEpochUs) {
    state.audioEpochUs = real;
    state.audioPulls = 0;
  }
  let due = Math.floor((real - state.audioEpochUs) / FRAME_US);
  if (due - state.audioPulls > PLAYOUT_BACKLOG) {
    state.counters.pullSkips = (state.counters.pullSkips || 0) + (due - state.audioPulls - 1);
    state.audioPulls = due - 1;
  }
  while (state.audioPulls < due) {
    state.audioPulls += 1;
    pullAudio(state.audioEpochUs + state.audioPulls * FRAME_US);
  }
}

function pullAudio(t) {
  const client = state.session?.client;
  if (!client) return;
  const c = state.counters;
  const real = nowUs();
  if (c.lastTickUs) {
    const gap = (real - c.lastTickUs) / 1000;
    c.tickMaxMs = Math.max(c.tickMaxMs || 0, gap);
    if (gap > 30) c.tickLate = (c.tickLate || 0) + 1;
  }
  c.lastTickUs = real;
  for (const remote of state.remotes.values()) {
    if (remote.kind !== KindAudio || !remote.decoder) continue;
    const out = client.pullAudio(remote.stream, t);
    const status = out.status;
    remote.statuses = remote.statuses || [0, 0, 0, 0, 0];
    remote.statuses[status] += 1;
    if (status === AudioWaiting) {
      remote.waiting += 1;
      continue;
    }
    remote.started = true;
    remote.pulled += 1;
    remote.jbDelay = (remote.jbDelay || 0) + (client.remotes?.get?.(remote.stream)?.audio?.targetUs?.() || 0) / 1e6;
    const data = out.data;
    remote.chain = remote.chain.then(async () => {
      let packet = null;
      if (status === AudioPlay && data) {
        const { plain } = await state.opener.open(remote.peer, data, CLEAR_AUDIO);
        if (plain && plain.length) packet = plain;
        else remote.sealDrops += 1;
      }
      if (packet) {
        if (!remote.firstPlayWall) remote.firstPlayWall = Date.now();
        remote.toc = packet[0];
        remote.played += 1;
        remote.concealing = false;
      } else {
        packet = Uint8Array.of(remote.toc & 0xfc);
        remote.concealed += 1;
        if (!remote.concealing) remote.concealEvents += 1;
        remote.concealing = true;
        void AudioConceal; void AudioInsert; void AudioDRED;
      }
      if (!remote.decoder || remote.decoder.state === "closed") return;
      try {
        remote.decoder.decode(new EncodedAudioChunk({ type: "key", timestamp: remote.pulled * 20000, data: packet }));
      } catch (_) {}
    });
  }
}

function audioEncoderConfig(bitrate, rate, channels) {
  return {
    codec: "opus",
    sampleRate: rate,
    numberOfChannels: channels,
    bitrate,
    opus: {
      frameDuration: 20000,
      complexity: 9,
      useinbandfec: true,
      packetlossperc: Math.max(0, Math.min(30, Math.round(state.audioLoss * 100))),
      usedtx: false,
      signal: "voice",
      application: "voip",
    },
  };
}

async function pumpAudio(readable) {
  const reader = readable.getReader();
  for (;;) {
    let next;
    try { next = await reader.read(); } catch (_) { return; }
    if (next.done) return;
    const data = next.value;
    if (state.closing) {
      data.close();
      continue;
    }
    const client = state.session?.client;
    if (!client || !state.audioStream || !state.sealer.ready) {
      data.close();
      continue;
    }
    if (!state.audioEncoder || state.audioEncoder.state === "closed") {
      state.audioBitrate = client.audioBitrate(state.audioStream) || 32000;
      state.audioEncoder = new AudioEncoder({
        output: (chunk) => sendAudioChunk(chunk),
        error: () => { state.audioEncoder = null; },
      });
      state.audioEncoder.configure(audioEncoderConfig(state.audioBitrate, data.sampleRate, 1));
      state.audioConfiguredAt = nowMs();
      state.audioRate = data.sampleRate;
    } else if (nowMs() - state.audioConfiguredAt > AUDIO_RECONFIGURE_MS) {
      const want = client.audioBitrate(state.audioStream) || state.audioBitrate;
      const loss = client.stats().loss || 0;
      if (Math.abs(want - state.audioBitrate) >= 4000 || Math.abs(loss - state.audioLoss) >= 0.05) {
        state.audioBitrate = want;
        state.audioLoss = loss;
        state.audioEncoder.configure(audioEncoderConfig(want, state.audioRate, 1));
      }
      state.audioConfiguredAt = nowMs();
    }
    try {
      if (data.numberOfChannels > 1) {
        const mono = new Float32Array(data.numberOfFrames);
        data.copyTo(mono, { planeIndex: 0, format: "f32-planar" });
        const single = new AudioData({ format: "f32-planar", sampleRate: data.sampleRate, numberOfFrames: data.numberOfFrames, numberOfChannels: 1, timestamp: data.timestamp, data: mono });
        data.close();
        state.audioEncoder.encode(single);
        single.close();
      } else {
        state.audioEncoder.encode(data);
        data.close();
      }
    } catch (_) {
      try { data.close(); } catch (_) {}
    }
  }
}

function sendAudioChunk(chunk) {
  const bytes = new Uint8Array(chunk.byteLength);
  chunk.copyTo(bytes);
  state.sealer.seal(bytes, CLEAR_AUDIO).then((sealed) => {
    const client = state.session?.client;
    if (!sealed || !client || !state.audioStream) return;
    try {
      client.sendAudio(state.audioStream, sealed, nowUs());
      state.counters.audioSent += 1;
      state.counters.audioBytesSent += sealed.length;
    } catch (_) {}
  });
}

function tierFor(enc, bitrate, frame) {
  let pick = TIERS[TIERS.length - 1];
  for (const tier of TIERS) {
    if (tier.width <= enc.maxWidth && bitrate >= tier.from) {
      pick = tier;
      break;
    }
  }
  const w = frame.displayWidth || pick.width;
  const h = frame.displayHeight || pick.height;
  const width = Math.min(pick.width, w - (w % 2));
  const height = Math.max(2, Math.round((width * h) / w / 2) * 2);
  return { width, height };
}

function configureEncoder(enc, bitrate, tier) {
  const fps = state.config.fps || 24;
  const config = {
    codec: CODECS[state.codec].encode,
    width: tier.width,
    height: tier.height,
    bitrate,
    framerate: fps,
    latencyMode: "realtime",
    scalabilityMode: "L1T3",
  };
  if (CODECS[state.codec].avc) config.avc = CODECS[state.codec].avc;
  const resized = enc.width !== tier.width || enc.height !== tier.height;
  if (!enc.encoder || enc.encoder.state === "closed") {
    enc.encoder = new VideoEncoder({
      output: (chunk, meta) => sendVideoChunk(enc, chunk, meta),
      error: () => { enc.encoder = null; enc.keyNext = true; },
    });
  }
  enc.encoder.configure(config);
  enc.width = tier.width;
  enc.height = tier.height;
  enc.bitrate = bitrate;
  enc.configuredAt = nowMs();
  if (resized) enc.keyNext = true;
}

function scaled(enc, frame) {
  if (frame.displayWidth === enc.width && frame.displayHeight === enc.height) return frame;
  if (!enc.canvas || enc.canvas.width !== enc.width || enc.canvas.height !== enc.height) {
    enc.canvas = new OffscreenCanvas(enc.width, enc.height);
    enc.context = enc.canvas.getContext("2d", { alpha: false });
  }
  enc.context.drawImage(frame, 0, 0, enc.width, enc.height);
  return new VideoFrame(enc.canvas, { timestamp: frame.timestamp });
}

function encodeVideo(frame) {
  const client = state.session?.client;
  if (!client || !state.sealer.ready || state.cameraOff) return;
  for (const enc of state.encodings) {
    if (client.sendingRepresentation(enc.stream) !== RepVideo) continue;
    const bitrate = client.videoBitrate(enc.stream);
    if (bitrate <= 0) continue;
    const tier = tierFor(enc, bitrate, frame);
    const stale = !enc.encoder || enc.encoder.state === "closed" || tier.width !== enc.width || tier.height !== enc.height;
    const drift = Math.abs(bitrate - enc.bitrate) > RECONFIGURE_RATIO * enc.bitrate;
    if (stale || (drift && nowMs() - enc.configuredAt >= RECONFIGURE_MS)) configureEncoder(enc, bitrate, tier);
    if (enc.encoder.encodeQueueSize > MAX_ENCODE_QUEUE) continue;
    const input = scaled(enc, frame);
    const keyFrame = enc.keyNext;
    enc.keyNext = false;
    try {
      enc.encoder.encode(input, { keyFrame });
    } catch (_) {
      enc.keyNext = true;
    }
    if (input !== frame) input.close();
  }
}

function sendVideoChunk(enc, chunk, meta) {
  const bytes = new Uint8Array(chunk.byteLength);
  chunk.copyTo(bytes);
  const keyframe = chunk.type === "key";
  const layer = keyframe ? 0 : Math.max(0, Math.min(2, meta?.svc?.temporalLayerId ?? 0));
  state.sealer.seal(bytes, CLEAR_VIDEO).then((sealed) => {
    const client = state.session?.client;
    if (!sealed || !client) return;
    try {
      client.sendVideoFrame(enc.stream, new OutFrame({ data: sealed, keyframe, layer }), nowUs());
      state.counters.videoFramesSent += 1;
      state.counters.videoBytesSent += sealed.length;
      if (keyframe) state.counters.keyframesSent += 1;
    } catch (_) {
      enc.keyNext = true;
    }
  });
}

async function pumpVideo(readable) {
  const reader = readable.getReader();
  for (;;) {
    let next;
    try { next = await reader.read(); } catch (_) { return; }
    if (next.done) return;
    const frame = next.value;
    try {
      if (!state.closing) encodeVideo(frame);
    } finally {
      frame.close();
    }
  }
}

function sendFace(data, reference) {
  const client = state.session?.client;
  if (!client || !state.sealer.ready) return;
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  const targets = [...state.faceStreams].filter((stream) => client.sendingRepresentation(stream) === RepFace);
  if (!targets.length) return;
  for (const stream of targets) {
    state.sealer.seal(bytes, CLEAR_VIDEO).then((sealed) => {
      const current = state.session?.client;
      if (!sealed || !current) return;
      try {
        current.sendVideoFrame(stream, new OutFrame({ data: sealed, face: true, keyframe: reference }), nowUs());
        if (reference) state.counters.referencesSent += 1;
        else state.counters.faceFramesSent += 1;
      } catch (_) {}
    });
  }
}

function deadAfterMs(client) {
  if (!state.standby || state.standby.closed) return LONE_DEAD_MS;
  const rtt = (client?.stats().rttUs || 0) / 1000;
  return Math.max(MIN_DEAD_MS, Math.min(MAX_DEAD_MS, 3 * rtt + 200));
}

function watchPrimary() {
  const link = state.session;
  if (!link || state.moving) return;
  const t = nowMs();
  if (state.lastWatch && t - state.lastWatch > STALL_MS) link.lastHeard = Math.max(link.lastHeard, t - TICK_MS);
  state.lastWatch = t;
  let reason = "";
  if (link.closed) reason = "closed";
  else if (link.heard && t - link.lastSent < SENT_RECENT_MS && t - Math.max(link.lastHeard, link.sendingSince || 0) > deadAfterMs(link.client)) reason = "silent";
  if (!reason) return;
  if (state.standby && !state.standby.closed) {
    void promote(reason);
  } else if (!state.closing) {
    state.closing = true;
    post({ type: "state", state: "failed", reason });
  }
}

async function promote(reason) {
  state.moving = true;
  const deadAt = nowMs();
  const old = state.session;
  const link = state.standby;
  state.standby = null;
  state.counters.deadReason = reason;
  state.counters.deadAtWall = Date.now();
  post({ type: "state", state: "moving", reason });
  try { old.wt.close({ closeCode: 0, reason: "moved" }); } catch (_) {}
  resetRemotes();
  state.session = link;
  state.moved = true;
  const client = attachClient(link, link.endpoint.ticket);
  publishAll(client);
  for (const enc of state.encodings) enc.keyNext = true;
  if (state.audioEncoder) {
    try { state.audioEncoder.close(); } catch (_) {}
    state.audioEncoder = null;
  }
  state.counters.failovers += 1;
  state.counters.failoverMs = Math.round(nowMs() - deadAt);
  state.moving = false;
  post({ type: "state", state: "moved", reason });
}

async function armStandby(endpoint) {
  try {
    const link = await openSession(endpoint);
    const raw = append(null, new Join({ ticket: endpoint.ticket }));
    await link.control.write(raw);
    state.standby = link;
    void readControl(link);
    void readDatagrams(link);
    post({ type: "standby", ready: true });
  } catch (error) {
    post({ type: "standby", ready: false, error: String(error?.message || error) });
  }
}

function standbyPing() {
  const link = state.standby;
  if (!link || link.closed) return;
  if (!link.pingAt || nowMs() - link.pingAt >= STANDBY_PING_MS) {
    link.pingAt = nowMs();
    link.datagrams.write(PADDING.slice()).catch(() => {});
  }
}

function loop() {
  if (state.closing) return;
  const link = state.session;
  const client = link?.client;
  if (client) {
    try {
      client.tick(nowUs());
      handleEvents(client);
      pollVideo(client);
      pollAuth(client);
      if (nowMs() - state.lastAuthFlush >= AUTH_EVERY_MS) {
        state.lastAuthFlush = nowMs();
        void flushAuth(client);
      }
    } catch (error) {
      post({ type: "log", text: "tick " + String(error?.stack || error) });
    }
  }
  pullDue();
  watchPrimary();
  standbyPing();
  state.timer = setTimeout(loop, TICK_MS);
}

async function start(message) {
  if (state.started) return;
  state.started = true;
  state.config = message.config || {};
  state.access = message.access;
  state.codec = state.config.codec && CODECS[state.config.codec] ? state.config.codec : await pickCodec();
  if (message.audioPort) {
    state.audioPort = message.audioPort;
    state.audioPort.onmessage = (event) => {
      if (event.data?.t === "stats") {
        state.counters.playoutUnderruns = event.data.underruns;
        state.counters.playoutTrimmed = event.data.trimmed;
      }
    };
  }
  if (message.audio) void pumpAudio(message.audio);
  if (message.video) void pumpVideo(message.video);
  let link;
  try {
    link = await openSession({ url: state.access.url, hashes: state.access.hashes, ticket: state.access.ticket });
  } catch (error) {
    post({ type: "state", state: "failed", reason: "connect", text: String(error?.message || error) });
    return;
  }
  state.session = link;
  void readControl(link);
  void readDatagrams(link);
  const client = attachClient(link, state.access.ticket);
  publishAll(client);
  post({ type: "state", state: "connected", codec: state.codec });
  loop();
  if (state.access.standby?.url && state.access.standby?.ticket) void armStandby(state.access.standby);
}

async function pickCodec() {
  try {
    const support = await VideoEncoder.isConfigSupported({
      codec: CODECS.h264.encode, width: 640, height: 360, bitrate: 600000, framerate: 24,
      latencyMode: "realtime", scalabilityMode: "L1T3", avc: CODECS.h264.avc,
    });
    const decode = await VideoDecoder.isConfigSupported({ codec: CODECS.h264.decode });
    if (support.supported && decode.supported) return "h264";
  } catch (_) {}
  return "vp8";
}

function remoteStats() {
  const inbound = [];
  for (const remote of state.remotes.values()) {
    if (remote.kind === KindAudio) {
      const target = state.session?.client?.remotes?.get?.(remote.stream)?.audio?.targetUs?.() || 0;
      inbound.push({
        peer: remote.peer,
        kind: "audio",
        ssrc: remote.stream,
        totalSamplesReceived: remote.pulled * FRAME_SAMPLES,
        concealedSamples: remote.concealed * FRAME_SAMPLES,
        concealmentEvents: remote.concealEvents,
        jitterBufferDelay: remote.jbDelay || 0,
        jitterBufferEmittedCount: remote.pulled,
        sealDrops: remote.sealDrops,
        firstPlayWall: remote.firstPlayWall,
        statuses: remote.statuses || [],
        buffer: { ...(state.session?.client?.remotes?.get?.(remote.stream)?.audio?.stats || {}) },
        targetMs: target / 1000,
      });
    } else if (remote.kind === KindVideo) {
      inbound.push({
        peer: remote.peer,
        kind: "video",
        ssrc: remote.stream,
        bytesReceived: remote.bytes,
        framesReceived: remote.frames,
        framesDecoded: remote.decoded,
        framesDropped: remote.dropped,
        keyFramesDecoded: remote.keyDecoded,
        frameWidth: remote.width,
        frameHeight: remote.height,
        freezeCount: remote.freezes,
        totalFreezesDuration: remote.freezeMs / 1000,
        sealDrops: remote.sealDrops,
        representation: remote.rep === RepFace ? "face" : "video",
      });
    }
  }
  return inbound;
}

function statsSample() {
  const client = state.session?.client;
  const s = client ? client.stats() : null;
  const top = state.encodings[state.encodings.length - 1];
  return {
    inbound: remoteStats(),
    outbound: [
      { kind: "audio", ssrc: state.audioStream, packetsSent: state.counters.audioSent, bytesSent: state.counters.audioBytesSent, targetBitrate: state.audioBitrate },
      {
        kind: "video",
        ssrc: state.videoTrack,
        framesSent: state.counters.videoFramesSent,
        bytesSent: state.counters.videoBytesSent,
        frameHeight: top?.height || 0,
        frameWidth: top?.width || 0,
        targetBitrate: client && top ? client.videoBitrate(top.stream) : 0,
        keyFramesEncoded: state.counters.keyframesSent,
        pliCount: s?.keyframeRequests || 0,
        representation: client && state.videoTrack ? ["off", "video", "face"][client.sendingRepresentation(state.videoTrack)] : "off",
      },
    ],
    transport: s ? {
      currentRoundTripTime: s.rttUs / 1e6,
      availableOutgoingBitrate: s.targetBitrate,
      loss: s.loss,
      sentPackets: s.sentPackets,
      receivedPackets: s.receivedPackets,
      recoveredPackets: s.recoveredPackets,
    } : {},
    engine: s ? { ...s } : null,
    counters: { ...state.counters },
    codec: state.codec,
    moved: state.moved,
    standby: Boolean(state.standby && !state.standby.closed),
  };
}

function leave() {
  if (state.closing && !state.session) return;
  state.closing = true;
  clearTimeout(state.timer);
  const link = state.session;
  try { link?.client?.leave("bye", nowUs()); } catch (_) {}
  setTimeout(() => {
    for (const l of [link, state.standby]) {
      try { l?.wt.close({ closeCode: 0, reason: "bye" }); } catch (_) {}
    }
    for (const enc of state.encodings) {
      try { enc.encoder?.close(); } catch (_) {}
    }
    try { state.audioEncoder?.close(); } catch (_) {}
    resetRemotes();
    post({ type: "state", state: "left" });
  }, 50);
}

self.onmessage = (event) => {
  const message = event.data || {};
  switch (message.op) {
    case "start":
      void start(message);
      break;
    case "send-key":
      void state.sealer.setKey(message.key).catch((error) => post({ type: "log", text: "send-key " + error }));
      break;
    case "key":
      void state.opener.setKey(message.key).catch((error) => post({ type: "log", text: "key " + error }));
      break;
    case "forget":
      state.opener.forget(message.peer);
      break;
    case "mute":
      state.muted = Boolean(message.audio);
      state.cameraOff = Boolean(message.video);
      break;
    case "face":
      sendFace(message.data, false);
      break;
    case "reference":
      sendFace(message.data, true);
      break;
    case "auth-status":
      post({ type: "auth-status", peer: message.peer, status: state.opener.status(message.peer) });
      break;
    case "stats":
      post({ type: "stats", id: message.id, sample: statsSample() });
      break;
    case "leave":
      leave();
      break;
    default:
      break;
  }
};

void concatBytes;
void KindVideo;
