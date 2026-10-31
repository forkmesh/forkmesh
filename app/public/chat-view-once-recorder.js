import { VIEW_ONCE_KINDS, viewOnceDuration, viewOnceLabel } from "./chat-view-once.js";

const VIDEO_MIME_CANDIDATES = [
  "video/mp4;codecs=avc1.42E01E,mp4a.40.2",
  "video/mp4;codecs=avc1,mp4a.40.2",
  "video/mp4",
  "video/webm;codecs=vp8,opus",
  "video/webm;codecs=vp9,opus",
  "video/webm",
];
const VOICE_MIME_CANDIDATES = [
  "audio/mp4;codecs=mp4a.40.2",
  "audio/mp4",
  "audio/webm;codecs=opus",
  "audio/ogg;codecs=opus",
  "audio/webm",
];
const VIDEO_BITS_PER_SECOND = 900_000;
const AUDIO_BITS_PER_SECOND = 48_000;
const VOICE_BITS_PER_SECOND = 32_000;
const HOLD_THRESHOLD_MS = 450;

const filterStages = { face: null, voice: null };

export function registerViewOnceFilterStages({ face = null, voice = null } = {}) {
  filterStages.face = typeof face === "function" ? face : null;
  filterStages.voice = typeof voice === "function" ? voice : null;
}

export function viewOnceFilterStages() {
  return { face: Boolean(filterStages.face), voice: Boolean(filterStages.voice) };
}

export function viewOnceRecordingSupported(kind, scope = globalThis) {
  return Boolean(
    VIEW_ONCE_KINDS[kind] &&
    scope.MediaRecorder &&
    scope.navigator?.mediaDevices?.getUserMedia &&
    scope.isSecureContext !== false,
  );
}

export function pickViewOnceMime(kind, recorder = globalThis.MediaRecorder) {
  const candidates = kind === "video" ? VIDEO_MIME_CANDIDATES : VOICE_MIME_CANDIDATES;
  if (!recorder || typeof recorder.isTypeSupported !== "function") return "";
  return candidates.find((type) => recorder.isTypeSupported(type)) || "";
}

export async function openViewOnceCapture(kind, {
  filters = {},
  mediaDevices = globalThis.navigator?.mediaDevices,
  MediaStreamImpl = globalThis.MediaStream,
} = {}) {
  const raw = await mediaDevices.getUserMedia({
    audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true },
    video: kind === "video"
      ? {
        facingMode: "user",
        width: { ideal: 640 },
        height: { ideal: 480 },
        frameRate: { ideal: 24, max: 30 },
      }
      : false,
  });
  const stages = [];
  let voiceStage = null;
  let videoTrack = raw.getVideoTracks()[0] || null;
  let audioTrack = raw.getAudioTracks()[0] || null;
  let failing = "";
  try {
    if (audioTrack && filters.voice && filterStages.voice) {
      failing = "voice";
      const stage = await filterStages.voice(audioTrack, filters.voice);
      if (stage?.track) {
        stages.push(stage);
        voiceStage = stage;
        audioTrack = stage.track;
      }
    }
    if (kind === "video" && videoTrack && filters.face && filterStages.face) {
      failing = "face";
      const stage = await filterStages.face(raw, filters.face);
      if (stage?.track) {
        stages.push(stage);
        videoTrack = stage.track;
      }
    }
  } catch (error) {
    if (error && typeof error === "object" && !error.stage) error.stage = failing;
    for (const stage of stages) {
      try { stage.stop?.(); } catch (_) {}
    }
    for (const track of raw.getTracks()) track.stop();
    throw error;
  }
  const tracks = [videoTrack, audioTrack].filter(Boolean);
  const stream = new MediaStreamImpl(tracks);
  return {
    stream,
    raw,
    filtered: stages.length > 0,
    voiceStage,
    stop() {
      for (const stage of stages) {
        try { stage.stop?.(); } catch (_) {}
      }
      for (const track of stream.getTracks()) track.stop();
      for (const track of raw.getTracks()) track.stop();
    },
  };
}

export function recordViewOnce(stream, kind, {
  MediaRecorderImpl = globalThis.MediaRecorder,
  maxMs = VIEW_ONCE_KINDS[kind].maxDurationMs,
  onTick = () => {},
  now = () => performance.now(),
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
} = {}) {
  const mimeType = pickViewOnceMime(kind, MediaRecorderImpl);
  const options = kind === "video"
    ? { videoBitsPerSecond: VIDEO_BITS_PER_SECOND, audioBitsPerSecond: AUDIO_BITS_PER_SECOND }
    : { audioBitsPerSecond: VOICE_BITS_PER_SECOND };
  if (mimeType) options.mimeType = mimeType;
  const recorder = new MediaRecorderImpl(stream, options);
  const chunks = [];
  let startedAt = 0;
  let elapsed = 0;
  let timer = null;
  let settle;
  let fail;
  const done = new Promise((resolve, reject) => {
    settle = resolve;
    fail = reject;
  });
  recorder.addEventListener("dataavailable", (event) => {
    if (event.data && event.data.size) chunks.push(event.data);
  });
  recorder.addEventListener("error", (event) => {
    if (timer !== null) clearIntervalImpl(timer);
    fail(event.error || new Error("recording_failed"));
  });
  recorder.addEventListener("stop", () => {
    if (timer !== null) clearIntervalImpl(timer);
    timer = null;
    elapsed = Math.min(maxMs, Math.max(elapsed, now() - startedAt));
    const type = (recorder.mimeType || mimeType || (kind === "video" ? "video/webm" : "audio/webm"))
      .toLowerCase();
    settle({
      blob: new Blob(chunks, { type }),
      mime: type,
      durationMs: Math.round(elapsed),
    });
  });
  function stop() {
    if (recorder.state !== "inactive") {
      elapsed = now() - startedAt;
      recorder.stop();
    }
    return done;
  }
  recorder.start(1000);
  startedAt = now();
  timer = setIntervalImpl(() => {
    elapsed = now() - startedAt;
    onTick(Math.min(elapsed, maxMs), maxMs);
    if (elapsed >= maxMs) stop();
  }, 200);
  return { stop, done, recorder };
}

function makeButton(className, text, label) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = className;
  button.textContent = text;
  button.setAttribute("aria-label", label);
  return button;
}

function levelMeter(stream, canvas) {
  const AudioContextImpl = globalThis.AudioContext || globalThis.webkitAudioContext;
  if (!AudioContextImpl || !stream.getAudioTracks().length) return () => {};
  let context;
  try {
    context = new AudioContextImpl();
  } catch (_) {
    return () => {};
  }
  const source = context.createMediaStreamSource(stream);
  const analyser = context.createAnalyser();
  analyser.fftSize = 512;
  source.connect(analyser);
  const samples = new Uint8Array(analyser.fftSize);
  const history = [];
  const draw = canvas.getContext("2d");
  let frame = 0;
  function tick() {
    analyser.getByteTimeDomainData(samples);
    let peak = 0;
    for (const value of samples) peak = Math.max(peak, Math.abs(value - 128));
    history.push(peak / 128);
    if (history.length > 80) history.shift();
    if (draw) {
      const { width, height } = canvas;
      draw.clearRect(0, 0, width, height);
      draw.fillStyle = getComputedStyle(canvas).color || "#4f8cff";
      const bar = width / 80;
      history.forEach((level, index) => {
        const h = Math.max(2, level * height);
        draw.fillRect(index * bar, (height - h) / 2, Math.max(1, bar - 1), h);
      });
    }
    frame = requestAnimationFrame(tick);
  }
  frame = requestAnimationFrame(tick);
  return () => {
    cancelAnimationFrame(frame);
    try { source.disconnect(); } catch (_) {}
    context.close().catch(() => {});
  };
}

export function openViewOnceRecorder({
  kind,
  recipient = "",
  faces = [],
  backgrounds = [],
  voices = [],
  defaults = {},
  resolveVoice = (value) => value,
  onSend,
  onClose = () => {},
}) {
  const limits = VIEW_ONCE_KINDS[kind];
  const label = viewOnceLabel(kind);
  const overlay = document.createElement("div");
  overlay.className = "chat-view-once-viewer chat-view-once-recorder";
  overlay.setAttribute("role", "dialog");
  overlay.setAttribute("aria-modal", "true");
  overlay.setAttribute(
    "aria-label",
    recipient ? `Record a ${label.toLowerCase()} for @${recipient}` : `Record a ${label.toLowerCase()}`,
  );
  const panel = document.createElement("div");
  panel.className = "chat-view-once-panel";
  const heading = document.createElement("h2");
  heading.className = "chat-view-once-heading";
  heading.textContent = recipient ? `${label} to @${recipient}` : label;
  const stage = document.createElement("div");
  stage.className = "chat-view-once-stage";
  const preview = document.createElement(kind === "video" ? "video" : "canvas");
  preview.className = kind === "video" ? "chat-view-once-preview" : "chat-view-once-wave";
  if (kind === "video") {
    preview.muted = true;
    preview.playsInline = true;
    preview.setAttribute("playsinline", "");
    preview.autoplay = true;
    preview.setAttribute("aria-label", "Camera preview");
  } else {
    preview.width = 320;
    preview.height = 72;
    preview.setAttribute("role", "img");
    preview.setAttribute("aria-label", "Microphone level");
  }
  stage.append(preview);
  const timer = document.createElement("p");
  timer.className = "chat-view-once-timer";
  timer.setAttribute("role", "timer");
  timer.setAttribute("aria-live", "off");
  timer.textContent = `0:00 / ${viewOnceDuration(limits.maxDurationMs)}`;
  const status = document.createElement("p");
  status.className = "chat-view-once-note";
  status.setAttribute("role", "status");
  status.setAttribute("aria-live", "polite");
  status.textContent = kind === "voice"
    ? "Tap to start, or hold and release to send. It disappears after it is played once."
    : "It disappears after it is watched once.";
  const choice = {
    face: String(defaults.face || ""),
    background: String(defaults.background || ""),
    backgroundImage: String(defaults.backgroundImage || ""),
    voice: String(defaults.voice || ""),
  };
  const activeFilters = {};
  const filterRow = document.createElement("div");
  filterRow.className = "chat-view-once-filters";
  let monitorBox = null;
  let monitorLabel = null;
  const selects = [];

  function computeFilters() {
    const face = kind === "video" && filterStages.face && (choice.face || choice.background)
      ? { faceId: choice.face, backgroundId: choice.background, backgroundImage: choice.backgroundImage }
      : null;
    const voice = filterStages.voice ? String(resolveVoice(choice.voice, choice.face) || "") : "";
    activeFilters.face = face;
    activeFilters.voice = voice;
    if (monitorLabel) {
      monitorLabel.hidden = !voice;
      if (!voice) monitorBox.checked = false;
    }
  }

  function addPicker(name, label, options, value, onChange) {
    if (!options.length) return;
    const wrap = document.createElement("label");
    wrap.textContent = `${name} `;
    const select = document.createElement("select");
    select.className = `chat-view-once-${name.toLowerCase()}-select`;
    select.setAttribute("aria-label", label);
    for (const [id, text] of options) {
      const option = document.createElement("option");
      option.value = id;
      option.textContent = text;
      select.append(option);
    }
    select.value = options.some(([id]) => id === value) ? value : options[0][0];
    onChange(select.value);
    select.addEventListener("change", () => {
      onChange(select.value);
      computeFilters();
      if (!recording && !starting) {
        releaseCapture();
        if (kind === "video") previewCamera();
      }
    });
    selects.push(select);
    wrap.append(select);
    filterRow.append(wrap);
  }

  if (kind === "video" && filterStages.face) {
    addPicker("Face", "Face for this video message", faces, choice.face, (value) => {
      choice.face = value;
    });
    addPicker("Background", "Background for this video message", backgrounds, choice.background, (value) => {
      choice.background = value;
    });
  }
  if (filterStages.voice) {
    addPicker("Voice", "Voice for this message", voices, choice.voice, (value) => {
      choice.voice = value;
    });
    if (voices.length) {
      monitorLabel = document.createElement("label");
      monitorBox = document.createElement("input");
      monitorBox.type = "checkbox";
      monitorBox.className = "chat-view-once-monitor";
      monitorBox.setAttribute("aria-label", "Hear yourself while recording. Use headphones.");
      monitorLabel.append(monitorBox, document.createTextNode(" Hear yourself (headphones)"));
      filterRow.append(monitorLabel);
      monitorBox.addEventListener("change", () => {
        capture?.voiceStage?.monitor?.(monitorBox.checked);
      });
    }
  }
  filterRow.hidden = !selects.length;
  computeFilters();
  const controls = document.createElement("div");
  controls.className = "chat-view-once-controls";
  const record = makeButton("chat-view-once-record", "Record", `Start recording ${label.toLowerCase()}`);
  const stopButton = makeButton("chat-view-once-stop", "Stop", "Stop recording");
  stopButton.hidden = true;
  const discard = makeButton("chat-view-once-discard", "Discard", "Discard this recording");
  const rerecord = makeButton("chat-view-once-rerecord", "Re-record", "Record again");
  const send = makeButton("chat-view-once-send", "Send", `Send ${label.toLowerCase()}`);
  const cancel = makeButton("chat-view-once-close", "Cancel", "Cancel and close");
  for (const button of [discard, rerecord, send]) button.hidden = true;
  controls.append(record, stopButton, rerecord, discard, send, cancel);
  panel.append(heading, stage, filterRow, timer, status, controls);
  overlay.append(panel);

  let capture = null;
  let recording = null;
  let review = null;
  let reviewUrl = "";
  let stopMeter = () => {};
  let closed = false;
  let pressStartedAt = 0;
  let lastPointerDownAt = -Infinity;
  let starting = false;
  let stopWhenStarted = false;
  let sending = false;

  function unavailableText(error) {
    if (error?.stage === "voice" && activeFilters.voice) {
      return "The voice changer could not start here. Choose \u201cYour own voice\u201d or try again.";
    }
    if (activeFilters.face) {
      return "The face or background could not start here. Choose \u201cYour own face\u201d and \u201cNo background\u201d, or try again.";
    }
    if (activeFilters.voice) {
      return "The voice changer could not start here. Choose \u201cYour own voice\u201d or try again.";
    }
    return kind === "video"
      ? "The camera or microphone is unavailable."
      : "The microphone is unavailable.";
  }

  function previewCamera() {
    ensureCapture().then(() => {
      if (!closed) status.textContent = "It disappears after it is watched once.";
    }).catch((error) => {
      if (!closed && !error?.superseded) status.textContent = unavailableText(error);
    });
  }

  function setTimer(ms) {
    timer.textContent = `${viewOnceDuration(ms)} / ${viewOnceDuration(limits.maxDurationMs)}`;
  }

  function clearReview() {
    if (reviewUrl) URL.revokeObjectURL(reviewUrl);
    reviewUrl = "";
    review = null;
    const player = stage.querySelector(".chat-view-once-review");
    if (player) {
      player.removeAttribute("src");
      player.remove();
    }
  }

  function releaseCapture() {
    captureGeneration += 1;
    capturing = null;
    stopMeter();
    stopMeter = () => {};
    if (capture) capture.stop();
    capture = null;
    if (kind === "video") preview.srcObject = null;
  }

  function close() {
    if (closed) return;
    closed = true;
    if (recording) recording.stop().catch(() => {});
    releaseCapture();
    clearReview();
    overlay.remove();
    document.removeEventListener("keydown", onKey, true);
    onClose();
  }

  function onKey(event) {
    if (event.key === "Escape" && !sending) {
      event.preventDefault();
      close();
    }
  }

  let capturing = null;
  let captureGeneration = 0;

  function ensureCapture() {
    if (capture) return Promise.resolve(capture);
    if (capturing) return capturing;
    const generation = ++captureGeneration;
    capturing = openCapture(generation).finally(() => {
      if (generation === captureGeneration) capturing = null;
    });
    return capturing;
  }

  async function openCapture(generation) {
    status.textContent = activeFilters.face
      ? "Starting the camera and loading the face and background…"
      : kind === "video" ? "Starting the camera…" : "Starting the microphone…";
    const stale = () => closed || generation !== captureGeneration;
    const superseded = () => Object.assign(new Error("superseded"), { superseded: true });
    let opened;
    try {
      opened = await openViewOnceCapture(kind, { filters: { ...activeFilters } });
    } catch (error) {
      throw stale() ? superseded() : error;
    }
    if (stale()) {
      opened.stop();
      throw superseded();
    }
    capture = opened;
    if (monitorBox?.checked) capture.voiceStage?.monitor?.(true);
    if (kind === "video") {
      preview.hidden = false;
      preview.srcObject = capture.stream;
      preview.play?.().catch(() => {});
    } else {
      stopMeter = levelMeter(capture.stream, preview);
    }
    return capture;
  }

  function showReview(result) {
    review = result;
    reviewUrl = URL.createObjectURL(result.blob);
    const player = document.createElement(kind === "video" ? "video" : "audio");
    player.className = "chat-view-once-review";
    player.controls = true;
    player.src = reviewUrl;
    player.setAttribute("controlsList", "nodownload");
    player.setAttribute("aria-label", `Play back your ${label.toLowerCase()}`);
    if (kind === "video") {
      player.playsInline = true;
      preview.hidden = true;
    }
    stage.append(player);
    setTimer(result.durationMs);
    status.textContent = "Play it back, then send it, record it again or discard it.";
    record.hidden = true;
    stopButton.hidden = true;
    rerecord.hidden = false;
    discard.hidden = false;
    send.hidden = false;
    send.focus();
  }

  async function startRecording() {
    if (recording || review || closed || starting) return;
    starting = true;
    try {
      const active = await ensureCapture();
      if (closed) return;
      recording = recordViewOnce(active.stream, kind, {
        onTick: (ms) => setTimer(ms),
      });
      starting = false;
      if (stopWhenStarted) {
        stopWhenStarted = false;
        stopRecording();
      }
      for (const select of selects) select.disabled = true;
      record.hidden = true;
      stopButton.hidden = false;
      stopButton.focus();
      status.textContent = "Recording…";
      overlay.dataset.recording = "true";
      const result = await recording.done;
      recording = null;
      delete overlay.dataset.recording;
      for (const select of selects) select.disabled = false;
      releaseCapture();
      if (closed) return;
      if (!result.blob.size) {
        status.textContent = "Nothing was recorded. Try again.";
        record.hidden = false;
        stopButton.hidden = true;
        return;
      }
      if (result.blob.size > limits.maxBytes) {
        status.textContent = "That recording is too large to send. Record a shorter one.";
        record.hidden = false;
        stopButton.hidden = true;
        return;
      }
      showReview(result);
      if (kind === "voice" && overlay.dataset.holdSend === "true") {
        delete overlay.dataset.holdSend;
        await sendNow();
      }
    } catch (error) {
      starting = false;
      recording = null;
      for (const select of selects) select.disabled = false;
      releaseCapture();
      if (closed) return;
      status.textContent = unavailableText(error);
      record.hidden = false;
      stopButton.hidden = true;
    }
  }

  function stopRecording() {
    if (recording) recording.stop().catch(() => {});
  }

  async function sendNow() {
    if (!review || sending) return;
    sending = true;
    for (const button of [send, rerecord, discard, cancel]) button.disabled = true;
    status.textContent = "Encrypting and sending…";
    try {
      await onSend({ ...review, kind }, (fraction) => {
        status.textContent = `Sending… ${Math.round(fraction * 100)}%`;
      });
      sending = false;
      close();
    } catch (error) {
      sending = false;
      for (const button of [send, rerecord, discard, cancel]) button.disabled = false;
      status.textContent = error?.code === "media_quota_count" || error?.code === "media_quota_bytes"
        ? "You have too many unopened messages waiting. Try again once some are opened."
        : "It could not be sent. Try again.";
    }
  }

  record.addEventListener("pointerdown", (event) => {
    if (kind !== "voice" || event.button !== 0) return;
    pressStartedAt = performance.now();
    lastPointerDownAt = pressStartedAt;
    void startRecording();
  });
  record.addEventListener("click", () => {
    if (kind === "voice" && performance.now() - lastPointerDownAt < 1500) return;
    void startRecording();
  });
  function endPress() {
    if (!pressStartedAt) return;
    const held = performance.now() - pressStartedAt;
    pressStartedAt = 0;
    if (held < HOLD_THRESHOLD_MS) return;
    overlay.dataset.holdSend = "true";
    if (recording) stopRecording();
    else stopWhenStarted = true;
  }
  overlay.addEventListener("pointerup", endPress);
  overlay.addEventListener("pointercancel", endPress);
  stopButton.addEventListener("click", stopRecording);
  rerecord.addEventListener("click", () => {
    clearReview();
    rerecord.hidden = true;
    discard.hidden = true;
    send.hidden = true;
    record.hidden = false;
    setTimer(0);
    void startRecording();
  });
  discard.addEventListener("click", () => {
    clearReview();
    rerecord.hidden = true;
    discard.hidden = true;
    send.hidden = true;
    record.hidden = false;
    setTimer(0);
    status.textContent = "Discarded. Record a new one or cancel.";
    record.focus();
  });
  send.addEventListener("click", () => void sendNow());
  cancel.addEventListener("click", close);
  document.addEventListener("keydown", onKey, true);
  document.body.append(overlay);
  if (kind === "video") previewCamera();
  record.focus();
  return { element: overlay, close, start: startRecording, stop: stopRecording };
}
