import { WIDTH, HEIGHT, FPS, paintStamp, readStamp } from "/web/stamp.js";

const params = new URLSearchParams(location.search);
const name = params.get("name");
const until = Number(params.get("until"));
const adapterUrl = params.get("adapter");
const origin = performance.timeOrigin;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const post = (path, body) => fetch(path + "?name=" + encodeURIComponent(name), {
  method: "POST",
  body: typeof body === "string" ? body : JSON.stringify(body),
});
const log = (text) => post("/bench/log", String(text)).catch(() => {});
const MAX_DELAYS = 4000;
const GAP_MS = 500;

const realGetUserMedia = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);

async function stampedVideoTrack() {
  const camera = document.createElement("video");
  camera.muted = true;
  camera.autoplay = true;
  camera.playsInline = true;
  camera.srcObject = await realGetUserMedia({ video: { width: WIDTH, height: HEIGHT, frameRate: FPS } });
  await camera.play().catch(() => {});
  const canvas = document.createElement("canvas");
  canvas.width = WIDTH;
  canvas.height = HEIGHT;
  const context = canvas.getContext("2d");
  const noise = context.createImageData(128, 72);
  let seed = 1;
  let tick = 0;
  setInterval(() => {
    context.drawImage(camera, 0, 0, WIDTH, HEIGHT);
    for (let i = 0; i < noise.data.length; i += 4) {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      const value = seed >> 23;
      noise.data[i] = value;
      noise.data[i + 1] = (value * 7) & 255;
      noise.data[i + 2] = 255 - value;
      noise.data[i + 3] = 255;
    }
    tick += 1;
    context.putImageData(noise, 40 + ((tick * 6) % (WIDTH - 208)), 120 + ((tick * 3) % (HEIGHT - 200)));
    paintStamp(context, Date.now());
  }, 1000 / FPS);
  return canvas.captureStream(FPS).getVideoTracks()[0];
}

navigator.mediaDevices.getUserMedia = async (constraints) => {
  const tracks = [];
  if (constraints?.audio) tracks.push(...(await realGetUserMedia({ audio: constraints.audio })).getAudioTracks());
  if (constraints?.video) tracks.push(await stampedVideoTrack());
  return new MediaStream(tracks);
};

const traces = {};
const watched = new Set();
let firstVideo = 0;
let firstAudio = 0;
const probe = document.createElement("canvas");
const probeContext = probe.getContext("2d", { willReadFrequently: true });

function watchVideo(peer, track) {
  const video = document.createElement("video");
  video.muted = true;
  video.autoplay = true;
  video.playsInline = true;
  video.width = 160;
  video.height = 90;
  video.srcObject = new MediaStream([track]);
  document.body.appendChild(video);
  video.play().catch(() => {});
  const trace = traces[peer] || (traces[peer] = {
    first: 0, last: 0, frames: 0, gapFreezes: 0, gapFreezeMs: 0, stamps: 0, unreadable: 0, delays: [],
  });
  const onFrame = () => {
    const now = performance.now();
    if (!firstVideo) firstVideo = now;
    if (!trace.first) trace.first = origin + now;
    if (trace.last) {
      const gap = origin + now - trace.last;
      if (gap > GAP_MS) {
        trace.gapFreezes += 1;
        trace.gapFreezeMs += gap;
      }
    }
    trace.last = origin + now;
    trace.frames += 1;
    const delay = readStamp(probe, probeContext, video, Date.now());
    if (delay === null) trace.unreadable += 1;
    else {
      trace.stamps += 1;
      if (trace.delays.length < MAX_DELAYS) trace.delays.push(delay);
    }
    video.requestVideoFrameCallback(onFrame);
  };
  video.requestVideoFrameCallback(onFrame);
}

function watchAudio(track) {
  const audio = document.createElement("audio");
  audio.autoplay = true;
  audio.srcObject = new MediaStream([track]);
  document.body.appendChild(audio);
  audio.play().catch(() => {});
}

function attach(session) {
  for (const { peer, stream } of session.remote()) {
    for (const track of stream.getTracks()) {
      if (watched.has(track.id)) continue;
      watched.add(track.id);
      if (track.kind === "video") watchVideo(peer, track);
      else watchAudio(track);
    }
  }
}

function heardAudio(sample) {
  return sample.inbound.some((entry) => entry.kind === "audio"
    && (entry.totalSamplesReceived || 0) - (entry.concealedSamples || 0) > 0);
}

try {
  const adapter = await import(adapterUrl);
  const session = await adapter.join({
    name,
    log,
    identify: (key) => post("/bench/identify", key),
  });
  const timeline = [];
  const selves = new Set();
  let nextSample = 0;
  while (Date.now() < until) {
    attach(session);
    const self = session.self();
    if (self) selves.add(self);
    if (!firstAudio || Date.now() >= nextSample) {
      const sample = await session.stats();
      sample.t = Date.now();
      if (!firstAudio && heardAudio(sample)) firstAudio = performance.now();
      if (sample.t >= nextSample) {
        sample.state = session.state();
        timeline.push(sample);
        nextSample = sample.t + 1000;
      }
    }
    await sleep(firstAudio ? 100 : 50);
  }
  const final = await session.stats();
  final.t = Date.now();
  await post("/bench/report", {
    name,
    ua: navigator.userAgent,
    self: session.self(),
    selves: [...selves],
    state: session.state(),
    origin,
    firstVideo,
    firstAudio,
    timeline,
    final,
    traces,
  });
} catch (error) {
  await log("failed " + (error && error.stack || error));
  await post("/bench/report", { name, error: String(error && error.stack || error) });
}
