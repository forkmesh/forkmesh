export const FMM_WORKER_URL = "/fmm/fmm-worker.js";
export const FMM_WORKLET_URL = "/fmm/fmm-playout-worklet.js";
export const FMM_FACESTREAM_URL = "/chat-call-facestream.js";

const FACE_FPS = 30;
const MAX_PENDING_FRAMES = 2;
const FACE_VIEW = { width: 320, height: 400 };

export function fmmSupported(scope = globalThis) {
  return typeof scope.WebTransport === "function"
    && typeof scope.VideoEncoder === "function"
    && typeof scope.VideoDecoder === "function"
    && typeof scope.AudioEncoder === "function"
    && typeof scope.AudioDecoder === "function"
    && typeof scope.MediaStreamTrackProcessor === "function"
    && typeof scope.MediaStreamTrackGenerator === "function"
    && typeof scope.AudioWorkletNode === "function"
    && typeof scope.Worker === "function";
}

export function fmmUsable(access, scope = globalThis) {
  const block = access?.fmm;
  return Boolean(block && block.url && block.ticket && Array.isArray(block.hashes) && block.hashes.length && fmmSupported(scope));
}

export async function startFmmCall({
  access,
  local,
  media = "video",
  scope = globalThis,
  workerUrl = FMM_WORKER_URL,
  workletUrl = FMM_WORKLET_URL,
  faceStreamUrl = FMM_FACESTREAM_URL,
  stage = null,
  simulcast = 1,
  codec = "",
  playout = "context",
  onEvent = () => {},
  onChange = () => {},
}) {
  const documentRef = scope.document;
  const block = access.fmm;
  const audioTrack = local.getAudioTracks()[0] || null;
  const cameraTrack = media === "video" ? (stage?.track || local.getVideoTracks()[0] || null) : null;
  const tiles = new Map();
  const streams = new Map();
  const auth = new Map();
  const statsWaiters = new Map();
  let statsId = 0;
  let state = "connecting";
  let peer = "";
  let closed = false;
  let faceModule = null;
  let faceSender = null;
  let faceTimer = null;
  let faceStreamsOn = new Set();
  let audioElement = null;

  const context = new scope.AudioContext({ sampleRate: 48000, latencyHint: "interactive" });
  await context.audioWorklet.addModule(workletUrl);
  const playoutNode = new scope.AudioWorkletNode(context, "fmm-playout", { numberOfInputs: 0, outputChannelCount: [1] });
  if (playout === "element" && documentRef) {
    const destination = context.createMediaStreamDestination();
    playoutNode.connect(destination);
    audioElement = documentRef.createElement("audio");
    audioElement.autoplay = true;
    audioElement.srcObject = destination.stream;
    audioElement.play?.()?.catch?.(() => {});
  } else {
    playoutNode.connect(context.destination);
  }
  if (context.state !== "running") context.resume().catch(() => {});
  const channel = new scope.MessageChannel();
  playoutNode.port.postMessage({ port: channel.port2 }, [channel.port2]);

  const worker = new scope.Worker(workerUrl, { type: "module", name: "forkmesh-fmm" });
  const transfer = [channel.port1];
  const message = {
    op: "start",
    access: block,
    audioPort: channel.port1,
    config: {
      video: Boolean(cameraTrack),
      simulcast,
      codec,
      fps: 24,
      face: false,
      client: stage ? {} : { faceDownBitrate: 1, faceUpBitrate: 2 },
    },
  };
  if (audioTrack) {
    const processor = new scope.MediaStreamTrackProcessor({ track: audioTrack });
    message.audio = processor.readable;
    transfer.push(processor.readable);
  }
  if (cameraTrack) {
    const processor = new scope.MediaStreamTrackProcessor({ track: cameraTrack });
    message.video = processor.readable;
    transfer.push(processor.readable);
  }

  function tileFor(who) {
    let tile = tiles.get(who);
    if (!tile) {
      tile = { peer: who, stream: new scope.MediaStream(), name: "", rep: "video", auth: auth.get(who) || "pending", generator: null, writer: null, pending: 0, faceView: null, faceTrack: null };
      tiles.set(who, tile);
    }
    return tile;
  }

  function videoTrackFor(tile) {
    if (tile.generator) return;
    tile.generator = new scope.MediaStreamTrackGenerator({ kind: "video" });
    tile.writer = tile.generator.writable.getWriter();
    tile.stream.addTrack(tile.generator);
  }

  function writeFrame(tile, frame) {
    if (!tile.writer || tile.pending >= MAX_PENDING_FRAMES) {
      frame.close();
      return;
    }
    tile.pending += 1;
    tile.writer.write(frame).catch(() => {}).finally(() => { tile.pending -= 1; });
  }

  async function faceViewFor(tile) {
    if (tile.faceView) return tile.faceView;
    faceModule = faceModule || await import(faceStreamUrl);
    tile.faceView = faceModule.createFaceStreamView({ documentRef, ...FACE_VIEW });
    const track = tile.faceView.canvas.captureStream?.(FACE_FPS)?.getVideoTracks?.()[0] || null;
    tile.faceTrack = track;
    return tile.faceView;
  }

  function showRep(tile, rep) {
    tile.rep = rep;
    const showFace = rep === "face" && tile.faceTrack;
    for (const track of tile.stream.getVideoTracks()) tile.stream.removeTrack(track);
    if (showFace) tile.stream.addTrack(tile.faceTrack);
    else if (tile.generator) tile.stream.addTrack(tile.generator);
    onChange();
  }

  async function startFaceFeed() {
    if (faceTimer || !stage?.landmarks) return;
    faceModule = faceModule || await import(faceStreamUrl);
    const settings = local.getVideoTracks()[0]?.getSettings?.() || {};
    faceSender = faceSender || faceModule.createFaceStreamSender({ width: settings.width || 640, height: settings.height || 360, fps: FACE_FPS, documentRef });
    faceTimer = scope.setInterval(() => {
      const marks = stage.landmarks();
      const points = marks?.live;
      if (!points || points.length !== 68) return;
      const packet = faceSender.frame({ time: Math.round(scope.performance.now()), points, reading: stage.expression?.() || null });
      if (packet) worker.postMessage({ op: "face", data: packet }, [packet.buffer]);
    }, 1000 / FACE_FPS);
  }

  function stopFaceFeed() {
    if (faceTimer) scope.clearInterval(faceTimer);
    faceTimer = null;
  }

  async function sendReference(maxBytes) {
    if (!stage?.landmarks || !faceSender) return;
    const points = stage.landmarks()?.live;
    const source = stage.canvas;
    if (!points || points.length !== 68 || !source) return;
    try {
      const packet = await faceSender.reference({ time: Math.round(scope.performance.now()), source, points });
      if (packet && packet.length <= maxBytes) worker.postMessage({ op: "reference", data: packet }, [packet.buffer]);
    } catch (_) {}
  }

  worker.onmessage = (event) => {
    const m = event.data || {};
    switch (m.type) {
      case "joined":
        peer = m.peer;
        state = "live";
        onEvent({ type: "joined", peer: m.peer, moved: m.moved });
        onChange();
        break;
      case "state":
        if (m.state === "failed") state = "failed";
        else if (m.state === "moving") state = "reconnecting";
        else if (m.state === "moved") state = "live";
        else if (m.state === "left") state = "left";
        onEvent({ type: "state", state: m.state, reason: m.reason || "", text: m.text || "" });
        onChange();
        break;
      case "error":
        onEvent({ type: "error", code: m.code, text: m.text });
        break;
      case "track":
        streams.set(m.stream, m.peer);
        videoTrackFor(tileFor(m.peer));
        onChange();
        break;
      case "track-removed": {
        const tile = tiles.get(m.peer);
        streams.delete(m.stream);
        if (tile) {
          tile.writer?.close?.().catch?.(() => {});
          tile.generator?.stop?.();
          tile.faceView?.dispose?.();
          tiles.delete(m.peer);
        }
        onChange();
        break;
      }
      case "video": {
        const tile = tiles.get(m.peer);
        if (tile && !closed) writeFrame(tile, m.frame);
        else m.frame.close();
        break;
      }
      case "face": {
        const tile = tileFor(m.peer);
        void faceViewFor(tile).then((view) => {
          if (tile.rep === "face" && !tile.stream.getVideoTracks().includes(tile.faceTrack)) showRep(tile, "face");
          return view.receive(m.data);
        }).catch(() => {});
        break;
      }
      case "representation": {
        const tile = tileFor(m.peer);
        if (m.rep === "face") void faceViewFor(tile).then(() => showRep(tile, "face")).catch(() => {});
        else showRep(tile, m.rep);
        break;
      }
      case "auth": {
        auth.set(m.peer, m.state);
        const tile = tiles.get(m.peer);
        if (tile) tile.auth = m.state;
        onEvent({ type: "auth", peer: m.peer, state: m.state });
        onChange();
        break;
      }
      case "send-face":
        if (m.on) faceStreamsOn.add(m.stream);
        else faceStreamsOn.delete(m.stream);
        if (faceStreamsOn.size) void startFaceFeed();
        else stopFaceFeed();
        break;
      case "face-key":
        faceSender?.requestKey?.();
        break;
      case "reference-request":
        void sendReference(m.maxBytes);
        break;
      case "stats": {
        const resolve = statsWaiters.get(m.id);
        statsWaiters.delete(m.id);
        resolve?.(m.sample);
        break;
      }
      case "standby":
        onEvent({ type: "standby", ready: m.ready, error: m.error || "" });
        break;
      case "log":
        onEvent({ type: "log", text: m.text });
        break;
      default:
        break;
    }
  };
  worker.postMessage(message, transfer);

  return {
    get state() { return state; },
    get peer() { return peer; },
    get tiles() { return tiles; },
    get context() { return context; },
    setSendKey(key) { worker.postMessage({ op: "send-key", key }); },
    setPeerKey(key) { worker.postMessage({ op: "key", key }); },
    forget(who) {
      worker.postMessage({ op: "forget", peer: who });
      auth.delete(who);
    },
    authState(who) { return auth.get(who) || "pending"; },
    mute(audioOff, videoOff) { worker.postMessage({ op: "mute", audio: Boolean(audioOff), video: Boolean(videoOff) }); },
    stats() {
      const id = ++statsId;
      return new Promise((resolve) => {
        statsWaiters.set(id, resolve);
        worker.postMessage({ op: "stats", id });
        scope.setTimeout(() => {
          if (statsWaiters.delete(id)) resolve(null);
        }, 2000);
      });
    },
    leave() {
      if (closed) return;
      closed = true;
      stopFaceFeed();
      worker.postMessage({ op: "leave" });
      scope.setTimeout(() => worker.terminate(), 300);
      for (const tile of tiles.values()) {
        tile.writer?.close?.().catch?.(() => {});
        tile.generator?.stop?.();
        tile.faceView?.dispose?.();
      }
      tiles.clear();
      try { playoutNode.disconnect(); } catch (_) {}
      if (audioElement) audioElement.srcObject = null;
      context.close().catch(() => {});
    },
  };
}
