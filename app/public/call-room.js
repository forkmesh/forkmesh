import { createGroupCalls, groupCallEncryptionMode } from "./chat-group-calls.js";
import { createCallQualityMeter, mountCallQualityWidget, rtcStatsReader } from "./call-quality-meter.js";
import {
  cleanMeetingName,
  deriveMeetingKeys,
  meetingEnvelope,
  meetingFrame,
  openMeetingBox,
  parseMeetingLink,
  sealMeetingBox,
} from "./call-meeting-crypto.js";
import {
  TRANSCRIBE_LANGUAGES,
  createTranscriber,
  installOfflineTranscription,
  offlineTranscriptionStatus,
  transcribeLanguage,
  transcriptFileName,
  transcriptText,
} from "./call-transcribe.js";

const API = "/api/booking/-/call/";
const NAME_KEY = "forkmesh.meeting.name";
const LANG_KEY = "forkmesh.meeting.transcriptLanguage";
const POLL_MS = 1500;
const POLL_IDLE_MS = 4000;
const ROOM_REFRESH_MS = 30000;
const PEER_TTL_MS = 60000;
const TOAST_MS = 5000;
const ICONS = {
  mic: '<path d="M12 3a3 3 0 0 1 3 3v6a3 3 0 0 1-6 0V6a3 3 0 0 1 3-3z"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3"/>',
  micOff: '<path d="M9 9v3a3 3 0 0 0 5.1 2.1M15 9.3V6a3 3 0 0 0-5.7-1.3"/><path d="M5 11a7 7 0 0 0 11.7 5.2M19 11a7 7 0 0 1-.6 2.8M12 18v3M3 3l18 18"/>',
  cam: '<rect x="3" y="6" width="13" height="12" rx="2.5"/><path d="m16 10.5 5-3v9l-5-3"/>',
  camOff: '<path d="M16 16v1.5a.5.5 0 0 1-.5.5H5.5A2.5 2.5 0 0 1 3 15.5v-7A2.5 2.5 0 0 1 5.5 6H6m4 0h3.5A2.5 2.5 0 0 1 16 8.5V12l5-3v9"/><path d="M3 3l18 18"/>',
  effects: '<path d="M12 3l1.8 4.2L18 9l-4.2 1.8L12 15l-1.8-4.2L6 9l4.2-1.8z"/><path d="M19 15l.9 2.1L22 18l-2.1.9L19 21l-.9-2.1L16 18l2.1-.9z"/>',
  transcript: '<path d="M5 4h14a1 1 0 0 1 1 1v14a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1z"/><path d="M8 9h8M8 13h8M8 17h5"/>',
  people: '<circle cx="9" cy="8" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0"/><path d="M16 4.6a3.5 3.5 0 0 1 0 6.8M18.5 20a6.5 6.5 0 0 0-3-5.5"/>',
  leave: '<path d="M3.5 14.5c4.6-4.6 12.4-4.6 17 0l-2.3 2.3a1 1 0 0 1-1.3.1l-2.3-1.7a1 1 0 0 1-.4-.9l.2-1.7a9 9 0 0 0-5.8 0l.2 1.7a1 1 0 0 1-.4.9l-2.3 1.7a1 1 0 0 1-1.3-.1z"/>',
  lock: '<rect x="5" y="10.5" width="14" height="10" rx="2.5"/><path d="M8 10.5V8a4 4 0 0 1 8 0v2.5"/>',
};
const MESSAGES = {
  room: ["This meeting link isn't complete", "Open the whole link from your booking email. It ends with a long code after #."],
  key: ["This link is missing its private key", "The part after # is the key that encrypts the meeting. Open the full link from your booking email."],
  room_not_found: ["This meeting doesn't exist", "The link may be mistyped, or the host removed their booking page."],
  room_cancelled: ["This meeting was cancelled", "The room is closed. Check your email for a new time."],
  room_ended: ["This meeting has ended", "The room closes two hours after the meeting's end time."],
  left: ["You left the meeting", "Your camera and microphone are off."],
  dropped: ["The call ended", "The connection to the meeting was lost."],
  unavailable: ["The meeting can't load right now", "Check your connection and refresh the page."],
};

const $ = (name) => document.querySelector(`[data-mr="${name}"]`);
const link = parseMeetingLink(location.search, location.hash);
const channelKey = "meeting:" + link.room;
const encryption = groupCallEncryptionMode(globalThis);
const state = {
  keys: null,
  info: null,
  m: null,
  support: null,
  stream: null,
  stage: null,
  voice: null,
  voiceUnmount: null,
  faceId: "",
  faceImage: "",
  backgroundId: "",
  backgroundImage: "",
  outfitId: "",
  micOn: true,
  camOn: true,
  hasCamera: false,
  name: "",
  seq: 0,
  pollTimer: null,
  refreshTimer: null,
  peers: new Map(),
  tiles: new Map(),
  transcriber: null,
  trLang: "",
  trState: "",
  side: "",
  quality: null,
  joining: false,
  ended: false,
};

const groupCalls = createGroupCalls({
  sendFrame,
  fetchTicket: () => api("/ticket", { method: "POST", body: "{}" }),
  accountName: () => state.name.toLowerCase(),
  onChange: () => renderCall(),
  fmm: { enabled: false },
  acquireMedia: async () => state.stream,
  prepareLocal: async (local, media) => {
    const audio = state.voice?.track() || local.getAudioTracks()[0] || null;
    const video = media === "video" ? (state.stage?.track || local.getVideoTracks()[0] || null) : null;
    return { stream: new MediaStream([audio, video].filter(Boolean)), stage: state.stage, stop() {} };
  },
});

function icon(name) {
  return `<svg viewBox="0 0 24 24" aria-hidden="true">${ICONS[name] || ""}</svg>`;
}

function control(button, name, label) {
  button.innerHTML = icon(name) + `<span>${label}</span>`;
  button.setAttribute("aria-label", label);
}

async function api(path, init = {}) {
  const response = await fetch(API + link.room + path, {
    credentials: "omit",
    cache: "no-store",
    headers: init.body ? { "content-type": "application/json" } : {},
    ...init,
  });
  let data = {};
  try {
    data = await response.json();
  } catch (_) {
    data = {};
  }
  if (!response.ok) {
    const error = new Error(String(data.error || "request_failed"));
    error.status = response.status;
    error.data = data;
    throw error;
  }
  return data;
}

function show(phase) {
  for (const name of ["message", "lobby", "call"]) $(name).hidden = name !== phase;
  document.body.dataset.phase = phase;
}

function message(kind, extra = null, detail = "") {
  const [title, fallback] = MESSAGES[kind] || MESSAGES.unavailable;
  const text = detail || fallback;
  $("message-title").textContent = title;
  $("message-text").textContent = text;
  const box = $("message");
  box.querySelector(".mr-message-actions")?.remove();
  if (extra) box.append(extra);
  show("message");
}

function timeLabel(ms, withDay = true) {
  const options = withDay
    ? { weekday: "long", day: "numeric", month: "long", hour: "numeric", minute: "2-digit" }
    : { hour: "numeric", minute: "2-digit" };
  return new Intl.DateTimeFormat(undefined, options).format(new Date(ms));
}

function zoneLabel() {
  return (Intl.DateTimeFormat().resolvedOptions().timeZone || "").replace(/_/g, " ");
}

function renderLobby() {
  const info = state.info;
  if (!info) return;
  $("top-title").textContent = info.title;
  $("title").textContent = info.title;
  $("host").textContent = "With " + info.host;
  $("when").textContent = `${timeLabel(info.startAt)} – ${timeLabel(info.endAt, false)} · ${zoneLabel()}`;
  const status = $("status");
  const open = info.state === "open";
  if (info.state === "early") {
    status.textContent = `The room opens at ${timeLabel(info.opensAt, false)}, 30 minutes before the meeting. Set up your camera and effects meanwhile.`;
  } else if (open) {
    status.textContent = "The room is open.";
  } else {
    status.textContent = "";
  }
  const name = $("name");
  const join = $("join");
  const error = $("join-error");
  if (!encryption) {
    error.hidden = false;
    error.textContent = "This browser can't encrypt calls end to end, so it can't join. Open the link in a current Chrome, Edge, Safari or Firefox.";
  }
  join.disabled = !open || !encryption || !state.stream || state.joining || !cleanMeetingName(name.value);
  join.textContent = state.joining ? "Joining…" : "Join meeting";
  renderLobbyDevices();
}

function renderLobbyDevices() {
  const mic = $("lobby-mic");
  const cam = $("lobby-cam");
  control(mic, state.micOn ? "mic" : "micOff", state.micOn ? "Mute" : "Unmute");
  mic.setAttribute("aria-pressed", state.micOn ? "true" : "false");
  control(cam, state.camOn ? "cam" : "camOff", state.camOn ? "Turn camera off" : "Turn camera on");
  cam.setAttribute("aria-pressed", state.camOn ? "true" : "false");
  cam.disabled = !state.hasCamera;
  mic.disabled = !state.stream;
  $("preview").hidden = !state.stream || !state.camOn || !state.hasCamera;
  $("preview-empty").hidden = Boolean(state.stream && state.camOn && state.hasCamera);
  if (state.stream && !state.camOn) $("preview-text").textContent = "Your camera is off.";
}

function setTrackState() {
  for (const track of [...(state.stream?.getAudioTracks() || []), state.voice?.track()].filter(Boolean)) {
    track.enabled = state.micOn;
  }
  for (const track of [...(state.stream?.getVideoTracks() || []), state.stage?.track].filter(Boolean)) {
    track.enabled = state.camOn;
  }
}

async function loadEffects() {
  const [faces, custom, backgrounds, voice, studio, outfits] = await Promise.all([
    import("./chat-call-faces.js"),
    import("./chat-call-face-custom.js"),
    import("./chat-call-backgrounds.js"),
    import("./chat-call-voice.js"),
    import("./call-studio.js"),
    import("./chat-call-outfits.js"),
  ]);
  state.m = { faces, custom, backgrounds, voice, studio, outfits };
  state.support = studio.studioSupport(globalThis);
  state.faceId = state.support.faces ? studio.savedStudioFace() : "";
  state.faceImage = studio.studioFaceImage(state.faceId);
  const saved = state.support.backgrounds ? backgrounds.savedCallBackground() : { id: "", image: "" };
  state.backgroundId = saved.id || "";
  state.backgroundImage = saved.image || "";
  state.outfitId = state.support.backgrounds ? outfits.savedCallOutfit() : "";
  buildEffectOptions();
}

function options(select, rows, value) {
  select.replaceChildren(...rows.map(([id, label]) => {
    const option = document.createElement("option");
    option.value = id;
    option.textContent = label;
    return option;
  }));
  select.value = value;
}

function buildEffectOptions() {
  const { studio, backgrounds, outfits } = state.m;
  options($("face"), studio.studioFaceChoices(), state.faceId);
  options($("background"), [["", "None"], ...backgrounds.CALL_BACKGROUNDS.map((item) => [item.id, item.custom ? "Your picture…" : item.label])], state.backgroundId);
  options($("outfit"), [["", "Your own clothes"], ...outfits.CALL_OUTFITS.map((item) => [item.id, item.label])], state.outfitId);
  $("face").disabled = !state.support.faces;
  $("background").disabled = !state.support.backgrounds;
  $("outfit").disabled = !state.support.backgrounds;
  if (!state.support.faces && !state.support.backgrounds) {
    $("effects-note").textContent = "Faces and backgrounds need WebGL, which this browser has turned off.";
  }
}

function chooseFace(id) {
  const { studio, custom } = state.m;
  if (id === custom.CUSTOM_FACE_ID && !studio.studioFaceImage(id)) {
    $("face-file").click();
    $("face").value = state.faceId;
    return;
  }
  state.faceId = studio.rememberStudioFace(id);
  state.faceImage = studio.studioFaceImage(state.faceId);
  state.stage?.setFace(state.faceId, state.faceImage || "");
  state.voice?.setFace(state.faceId);
  if (state.stage && state.faceId === custom.CUSTOM_FACE_ID) {
    studio.watchStudioFace(state.stage, {
      onFailed: () => {
        lobbyStatus("No face was found in that photo. Try a clear, front-facing one.");
        chooseFace("");
      },
    });
  }
  $("face").value = state.faceId;
}

function chooseBackground(id, image = "") {
  const { backgrounds } = state.m;
  const background = backgrounds.callBackgroundById(id);
  if (id && !background) return;
  if (background?.custom && !image) {
    const saved = backgrounds.savedCallBackground();
    if (saved.id === background.id && saved.image) {
      image = saved.image;
    } else {
      $("background-file").click();
      $("background").value = state.backgroundId;
      return;
    }
  }
  backgrounds.rememberCallBackground(background?.id || "", image);
  state.backgroundId = background?.id || "";
  state.backgroundImage = background?.custom ? image : "";
  state.stage?.setBackground(state.backgroundId, state.backgroundImage);
  $("background").value = state.backgroundId;
}

function chooseOutfit(id) {
  const { outfits } = state.m;
  const outfit = outfits.callOutfitById(id);
  outfits.rememberCallOutfit(outfit?.id || "");
  state.outfitId = outfit?.id || "";
  state.stage?.setOutfit(state.outfitId);
  $("outfit").value = state.outfitId;
}

function lobbyStatus(text) {
  $("status").textContent = text;
}

async function openMedia() {
  const { studio, faces, voice } = state.m;
  let stream = null;
  try {
    stream = await studio.openStudioMedia({ video: true });
    state.hasCamera = stream.getVideoTracks().length > 0;
  } catch (error) {
    try {
      stream = await studio.openStudioMedia({ video: false });
      state.hasCamera = false;
      state.camOn = false;
      $("preview-text").textContent = studio.studioMediaError(error, { video: true });
    } catch (second) {
      $("preview-text").textContent = studio.studioMediaError(second, { video: false });
      renderLobby();
      return;
    }
  }
  state.stream = stream;
  if (state.hasCamera && (state.support.faces || state.support.backgrounds)) {
    try {
      state.stage = faces.createFaceStage({
        cameraStream: stream,
        faceId: state.support.faces ? state.faceId : "",
        faceImage: state.support.faces ? state.faceImage : "",
        backgroundId: state.support.backgrounds ? state.backgroundId : "",
        backgroundImage: state.support.backgrounds ? state.backgroundImage : "",
        outfitId: state.support.backgrounds ? state.outfitId : "",
      });
      if (!state.stage?.track) state.stage = null;
    } catch (_) {
      state.stage = null;
    }
  }
  const preview = $("preview");
  preview.srcObject = new MediaStream([state.stage?.track || stream.getVideoTracks()[0]].filter(Boolean));
  preview.play?.()?.catch?.(() => {});
  if (state.support.voices) {
    try {
      state.voice = await studio.startStudioVoice({ micStream: stream, faceId: state.faceId });
      state.voiceUnmount = voice.mountVoicePreviewControls($("voice"), state.voice, { documentRef: document });
    } catch (_) {
      state.voice = null;
    }
  }
  setTrackState();
  renderLobby();
}

function stopMedia() {
  try {
    state.voiceUnmount?.();
  } catch (_) {}
  try {
    state.voice?.stop({ keepStream: true });
  } catch (_) {}
  try {
    state.stage?.stop();
  } catch (_) {}
  for (const track of state.stream?.getTracks() || []) {
    try {
      track.stop();
    } catch (_) {}
  }
  state.voice = null;
  state.stage = null;
  state.stream = null;
}

async function refreshRoom() {
  try {
    const data = await api("");
    state.info = data.room;
  } catch (error) {
    if (error.status === 404) {
      stop("room_not_found");
      return;
    }
    if (!state.info) message("unavailable");
    return;
  }
  if (state.info.state === "cancelled" || state.info.state === "ended") {
    stop("room_" + state.info.state);
    return;
  }
  if (!groupCalls.active) renderLobby();
}

async function sendFrame(_key, type, body) {
  if (!state.keys) return;
  const active = groupCalls.active;
  const box = await sealMeetingBox(state.keys, meetingEnvelope(type, body, {
    name: state.name,
    transcribing: Boolean(state.transcriber?.running),
    camera: Boolean(active && !active.cameraOff),
    microphone: Boolean(active && !active.muted),
  }));
  try {
    await api("/signal", { method: "POST", body: JSON.stringify({ box }) });
  } catch (error) {
    if (error.status === 409) stop(error.data?.error || "room_ended");
  }
}

function sendStatus() {
  const active = groupCalls.active;
  if (!active?.peer) return;
  void sendFrame(channelKey, "meeting-status", { peer: active.peer });
}

function toast(text) {
  const item = document.createElement("p");
  item.className = "mr-toast";
  item.textContent = text;
  $("toasts").append(item);
  setTimeout(() => item.remove(), TOAST_MS);
}

function notePeer(frame) {
  const known = state.peers.get(frame.peer);
  if (frame.leaving) {
    if (known) toast(`${known.name} left`);
    state.peers.delete(frame.peer);
    return;
  }
  if (!known) toast(`${frame.name} joined`);
  else if (frame.transcribing && !known.transcribing) toast(`${frame.name} started a transcript on their device`);
  state.peers.set(frame.peer, {
    name: frame.name,
    transcribing: frame.transcribing,
    cam: frame.camera,
    mic: frame.microphone,
    at: Date.now(),
  });
}

async function receive(box) {
  const frame = meetingFrame(await openMeetingBox(state.keys, box));
  const active = groupCalls.active;
  if (!frame || !active || frame.peer === active.peer) return;
  notePeer(frame);
  if (frame.frame) groupCalls.handleFrame(frame.frame, channelKey);
  renderCall();
}

async function poll() {
  state.pollTimer = null;
  if (state.ended) return;
  if (!groupCalls.active) {
    if (state.joining) state.pollTimer = setTimeout(poll, 200);
    return;
  }
  let wait = POLL_MS;
  try {
    const data = await api("/signal" + (state.seq ? "?after=" + state.seq : ""));
    for (const frame of data.frames || []) {
      state.seq = Math.max(state.seq, Number(frame.seq) || 0);
      await receive(String(frame.box || ""));
    }
    state.seq = Math.max(state.seq, Number(data.seq) || 0);
    if (!(data.frames || []).length && document.hidden) wait = POLL_IDLE_MS;
  } catch (error) {
    if (error.status === 409) {
      stop(error.data?.error || "room_ended");
      return;
    }
    wait = POLL_IDLE_MS;
  }
  if (groupCalls.active && !state.ended) state.pollTimer = setTimeout(poll, wait);
}

async function join() {
  const name = cleanMeetingName($("name").value);
  if (!name || state.joining || groupCalls.active) return;
  state.name = name;
  try {
    localStorage.setItem(NAME_KEY, name);
  } catch (_) {}
  state.joining = true;
  $("join-error").hidden = true;
  renderLobby();
  state.voice?.monitor(false);
  state.seq = 0;
  const media = state.hasCamera ? "video" : "audio";
  const joined = groupCalls.join(channelKey, link.room, media);
  clearTimeout(state.pollTimer);
  state.pollTimer = setTimeout(poll, 0);
  const result = await joined;
  state.joining = false;
  if (result?.error) {
    $("join-error").hidden = false;
    $("join-error").textContent = result.error;
    renderLobby();
    return;
  }
  if (!state.micOn) groupCalls.toggleMute();
  if (media === "video" && !state.camOn) groupCalls.toggleCamera();
  moveEffects(true);
  show("call");
  renderCall();
}

function stop(kind, detail = "") {
  if (state.ended) return;
  state.ended = true;
  clearTimeout(state.pollTimer);
  clearInterval(state.refreshTimer);
  state.transcriber?.stop();
  groupCalls.leave();
  stopMedia();
  state.quality?.widget?.destroy();
  state.quality?.meter.stop();
  state.quality = null;
  let actions = null;
  if (kind === "left" || kind === "dropped") {
    actions = document.createElement("div");
    actions.className = "mr-message-actions";
    const again = document.createElement("button");
    again.type = "button";
    again.className = "mr-button";
    again.textContent = "Rejoin";
    again.addEventListener("click", () => location.reload());
    actions.append(again);
    if (state.transcriber?.lines.length) {
      const save = document.createElement("button");
      save.type = "button";
      save.className = "mr-button";
      save.textContent = "Download transcript";
      save.addEventListener("click", downloadTranscript);
      actions.append(save);
    }
  }
  message(kind, actions, detail);
}

function peerName(peer) {
  return state.peers.get(peer)?.name || groupCalls.active?.tiles.get(peer)?.name || "Guest";
}

function tileFor(key, stream, label, self) {
  let entry = state.tiles.get(key);
  if (!entry) {
    const wrap = document.createElement("figure");
    wrap.className = "mr-tile";
    const video = document.createElement("video");
    video.autoplay = true;
    video.playsInline = true;
    video.muted = self;
    const caption = document.createElement("figcaption");
    const badge = document.createElement("span");
    badge.className = "mr-badge";
    wrap.append(video, caption, badge);
    entry = { wrap, video, caption, badge, stream: null };
    state.tiles.set(key, entry);
  }
  if (entry.stream !== stream) {
    entry.stream = stream;
    entry.video.srcObject = stream;
    entry.video.play?.()?.catch?.(() => {});
  }
  entry.caption.textContent = label;
  entry.wrap.classList.toggle("is-self", self);
  const hasVideo = (stream?.getVideoTracks?.() || []).some((track) => track.readyState !== "ended" && track.enabled !== false);
  entry.wrap.classList.toggle("is-novideo", !hasVideo);
  entry.wrap.dataset.initial = (label || "?").trim().charAt(0).toUpperCase();
  return entry;
}

function syncQuality(active) {
  if (state.quality && state.quality.call === active) return;
  state.quality?.widget?.destroy();
  state.quality?.meter.stop();
  state.quality = null;
  if (!active) return;
  const current = () => groupCalls.active === active;
  const rtc = rtcStatsReader(() => (current() && active.pc ? [active.pc] : []));
  const meter = createCallQualityMeter({
    readStats: () => rtc(),
    tracks: () => ({
      mic: current() ? (active.outgoing?.stream || active.local)?.getAudioTracks?.()[0] || null : null,
      peers: current() ? [...active.tiles.values()].flatMap((tile) => tile.stream?.getAudioTracks?.() || []) : [],
    }),
    face: () => (current() && active.outgoing?.stage?.face?.() ? active.outgoing.stage.stats || null : null),
  });
  const widget = mountCallQualityWidget($("quality"), meter, {
    video: () => current() && active.media === "video",
    sending: () => current() && !active.cameraOff,
    peerLabel: "Others",
  });
  state.quality = { call: active, meter, widget };
}

function prunePeers() {
  const now = Date.now();
  for (const [peer, entry] of state.peers) {
    if (now - entry.at > PEER_TTL_MS && !groupCalls.active?.tiles.has(peer)) state.peers.delete(peer);
  }
}

function renderCall() {
  const active = groupCalls.active;
  if (!active) {
    if (document.body.dataset.phase === "call" && !state.ended) {
      stop("dropped", groupCalls.lastError || "");
    }
    return;
  }
  if (document.body.dataset.phase !== "call") return;
  syncQuality(active);
  prunePeers();
  const lock = $("lock-label");
  lock.textContent = active.state === "live" ? "End-to-end encrypted" : active.state === "reconnecting" ? "Reconnecting…" : "Connecting…";
  const keep = new Set(["self"]);
  const tiles = [tileFor("self", active.outgoing?.stream || active.local, state.name + " (you)", true)];
  if (state.transcriber?.running) tiles[0].badge.textContent = "Transcribing";
  else tiles[0].badge.textContent = "";
  tiles[0].wrap.classList.toggle("is-muted", active.muted);
  for (const [peer, tile] of active.tiles) {
    keep.add(peer);
    const entry = tileFor(peer, tile.stream, peerName(peer), false);
    const known = state.peers.get(peer);
    entry.badge.textContent = known?.transcribing ? "Transcribing on their device" : "";
    entry.wrap.classList.toggle("is-muted", known ? !known.mic : false);
    tiles.push(entry);
  }
  for (const [key, entry] of state.tiles) {
    if (!keep.has(key)) {
      entry.video.srcObject = null;
      state.tiles.delete(key);
    }
  }
  const grid = $("grid");
  grid.dataset.count = String(tiles.length);
  const wanted = tiles.map((entry) => entry.wrap);
  if (wanted.length !== grid.children.length || wanted.some((node, index) => grid.children[index] !== node)) {
    grid.replaceChildren(...wanted);
  }
  control($("mute"), active.muted ? "micOff" : "mic", active.muted ? "Unmute" : "Mute");
  $("mute").setAttribute("aria-pressed", active.muted ? "true" : "false");
  control($("camera"), active.cameraOff ? "camOff" : "cam", active.cameraOff ? "Camera on" : "Camera off");
  $("camera").setAttribute("aria-pressed", active.cameraOff ? "true" : "false");
  $("camera").hidden = active.media !== "video";
  control($("open-effects"), "effects", "Effects");
  control($("open-transcript"), "transcript", state.transcriber?.running ? "Transcribing" : "Transcript");
  $("open-transcript").setAttribute("aria-pressed", state.transcriber?.running ? "true" : "false");
  control($("open-people"), "people", `People (${tiles.length})`);
  control($("leave"), "leave", "Leave");
  renderPeople(active);
  syncTranscriptSources(active);
}

function renderPeople(active) {
  const rows = [{ name: state.name + " (you)", note: active.muted ? "Muted" : "", tr: Boolean(state.transcriber?.running) }];
  for (const peer of active.tiles.keys()) {
    const known = state.peers.get(peer);
    rows.push({ name: peerName(peer), note: known && !known.mic ? "Muted" : "", tr: Boolean(known?.transcribing) });
  }
  $("people").replaceChildren(...rows.map((row) => {
    const item = document.createElement("li");
    const name = document.createElement("span");
    name.textContent = row.name;
    const note = document.createElement("span");
    note.className = "mr-muted";
    note.textContent = [row.note, row.tr ? "Transcribing on their device" : ""].filter(Boolean).join(" · ");
    item.append(name, note);
    return item;
  }));
}

function openSide(tab) {
  const side = $("side");
  if (state.side === tab) {
    state.side = "";
    side.hidden = true;
    return;
  }
  state.side = tab;
  side.hidden = false;
  for (const button of document.querySelectorAll("[data-mr-tab]")) {
    button.setAttribute("aria-selected", button.dataset.mrTab === tab ? "true" : "false");
  }
  for (const panel of document.querySelectorAll("[data-mr-panel]")) panel.hidden = panel.dataset.mrPanel !== tab;
  if (tab === "transcript") void checkTranscription();
}

function moveEffects(inCall) {
  const effects = $("effects");
  const target = inCall ? document.querySelector('[data-mr-panel="effects"]') : document.querySelector(".mr-preview");
  if (effects.parentElement !== target) target.append(effects);
}

function transcriptStatus(text) {
  $("tr-status").textContent = text;
}

async function checkTranscription() {
  const lang = state.trLang;
  const status = await offlineTranscriptionStatus(lang);
  if (lang !== state.trLang) return;
  state.trState = status;
  const toggle = $("tr-toggle");
  const install = $("tr-install");
  install.hidden = status !== "downloadable";
  toggle.disabled = status !== "available" && !state.transcriber?.running;
  if (status === "unsupported") {
    transcriptStatus("This browser can't turn speech into text on the device, so the transcript is off here. A recent desktop Chrome or Edge can.");
  } else if (status === "downloadable") {
    transcriptStatus("Download the speech model for this language once. After that the transcript runs offline, on this device.");
  } else if (status === "downloading") {
    transcriptStatus("Downloading the speech model…");
    setTimeout(() => void checkTranscription(), 3000);
  } else if (status === "unavailable") {
    transcriptStatus("Offline speech recognition isn't available for this language here. Pick another language.");
  } else if (!state.transcriber?.running) {
    transcriptStatus("Ready. Speech is turned into text on this device. No audio or text leaves it.");
  }
  renderTranscript();
}

function ensureTranscriber() {
  if (state.transcriber) return state.transcriber;
  state.transcriber = createTranscriber({
    lang: state.trLang,
    onChange: renderTranscript,
    onError: (text) => {
      transcriptStatus(text);
      renderCall();
      sendStatus();
    },
  });
  const own = state.transcriber.remoteAudio ? state.stream?.getAudioTracks()[0] || null : null;
  state.transcriber.addSource("self", () => state.name || "You", own);
  return state.transcriber;
}

function syncTranscriptSources(active) {
  const transcriber = state.transcriber;
  if (!transcriber || !transcriber.remoteAudio) return;
  const wanted = new Set(["self"]);
  for (const [peer, tile] of active.tiles) {
    const track = tile.stream?.getAudioTracks?.()[0];
    if (!track) continue;
    wanted.add(peer);
    transcriber.addSource(peer, () => peerName(peer), track);
  }
  for (const id of transcriber.ids()) {
    if (!wanted.has(id)) transcriber.removeSource(id);
  }
}

function toggleTranscript() {
  if (state.transcriber?.running) {
    state.transcriber.stop();
    transcriptStatus("Stopped. The transcript stays on this device until you close the page.");
  } else {
    if (state.trState !== "available") return;
    const transcriber = ensureTranscriber();
    if (!transcriber.start()) {
      transcriptStatus("The transcript could not start in this browser.");
      return;
    }
    if (groupCalls.active) syncTranscriptSources(groupCalls.active);
    transcriptStatus(transcriber.remoteAudio
      ? "Transcribing everyone on this device. Others see that you are."
      : "Transcribing your own voice on this device; this browser can't transcribe the others. Others see that you are.");
  }
  sendStatus();
  renderCall();
  renderTranscript();
}

function renderTranscript() {
  const transcriber = state.transcriber;
  const running = Boolean(transcriber?.running);
  $("tr-toggle").textContent = running ? "Stop transcript" : "Start transcript";
  $("tr-toggle").disabled = !running && state.trState !== "available";
  $("tr-lang").disabled = running;
  const list = $("tr-lines");
  const lines = transcriber ? transcriber.lines : [];
  const live = transcriber ? transcriber.live() : [];
  const rows = lines.slice(-400).map((line) => row(line.speaker, line.text, line.at, false))
    .concat(live.map((line) => row(line.speaker, line.text, Date.now(), true)));
  const atBottom = list.scrollHeight - list.scrollTop - list.clientHeight < 24;
  list.replaceChildren(...rows);
  if (atBottom) list.scrollTop = list.scrollHeight;
  for (const name of ["tr-copy", "tr-download", "tr-clear"]) $(name).disabled = !lines.length;
}

function row(speaker, text, at, interim) {
  const item = document.createElement("li");
  item.classList.toggle("is-interim", interim);
  const time = document.createElement("time");
  time.textContent = timeLabel(at, false);
  const who = document.createElement("strong");
  who.textContent = speaker;
  const words = document.createElement("span");
  words.textContent = text;
  item.append(time, who, words);
  return item;
}

function transcriptBody() {
  const title = state.info ? `${state.info.title} with ${state.info.host}, ${timeLabel(state.info.startAt)}` : "Meeting";
  return transcriptText(state.transcriber?.lines || [], { title });
}

function downloadTranscript() {
  const blob = new Blob([transcriptBody()], { type: "text/plain;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = transcriptFileName(state.info?.title || "meeting");
  document.body.append(anchor);
  anchor.click();
  anchor.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function wire() {
  $("lock").addEventListener("click", () => {
    const trust = $("trust");
    trust.hidden = !trust.hidden;
    $("lock").setAttribute("aria-expanded", trust.hidden ? "false" : "true");
  });
  $("trust-more").addEventListener("click", () => $("lock").click());
  $("name").addEventListener("input", renderLobby);
  $("name").addEventListener("keydown", (event) => {
    if (event.key === "Enter") void join();
  });
  $("join").addEventListener("click", () => void join());
  $("lobby-mic").addEventListener("click", () => {
    state.micOn = !state.micOn;
    setTrackState();
    renderLobbyDevices();
  });
  $("lobby-cam").addEventListener("click", () => {
    state.camOn = !state.camOn;
    setTrackState();
    renderLobbyDevices();
  });
  $("face").addEventListener("change", (event) => chooseFace(event.target.value));
  $("background").addEventListener("change", (event) => chooseBackground(event.target.value));
  $("outfit").addEventListener("change", (event) => chooseOutfit(event.target.value));
  $("face-file").addEventListener("change", async (event) => {
    const file = event.target.files?.[0];
    event.target.value = "";
    if (!file) return;
    try {
      await state.m.studio.studioFaceFromFile(file);
      chooseFace(state.m.custom.CUSTOM_FACE_ID);
    } catch (_) {
      lobbyStatus("That photo could not be used. Try another one.");
    }
  });
  $("background-file").addEventListener("change", async (event) => {
    const file = event.target.files?.[0];
    event.target.value = "";
    if (!file) return;
    try {
      const image = await state.m.studio.studioBackgroundFromFile(file);
      chooseBackground(state.m.backgrounds.CUSTOM_BACKGROUND_ID, image);
    } catch (_) {
      lobbyStatus("That picture could not be used. Try another one.");
    }
  });
  $("mute").addEventListener("click", () => {
    groupCalls.toggleMute();
    sendStatus();
  });
  $("camera").addEventListener("click", () => {
    groupCalls.toggleCamera();
    sendStatus();
  });
  $("open-effects").addEventListener("click", () => openSide("effects"));
  $("open-transcript").addEventListener("click", () => openSide("transcript"));
  $("open-people").addEventListener("click", () => openSide("people"));
  $("side-close").addEventListener("click", () => openSide(state.side));
  for (const button of document.querySelectorAll("[data-mr-tab]")) {
    button.addEventListener("click", () => {
      if (state.side !== button.dataset.mrTab) openSide(button.dataset.mrTab);
    });
  }
  $("leave").addEventListener("click", () => stop("left"));
  $("tr-toggle").addEventListener("click", toggleTranscript);
  $("tr-install").addEventListener("click", async () => {
    $("tr-install").disabled = true;
    transcriptStatus("Downloading the speech model…");
    const ok = await installOfflineTranscription(state.trLang);
    $("tr-install").disabled = false;
    if (!ok) transcriptStatus("The speech model could not be downloaded. Try again.");
    void checkTranscription();
  });
  options($("tr-lang"), TRANSCRIBE_LANGUAGES, state.trLang);
  $("tr-lang").addEventListener("change", (event) => {
    state.trLang = event.target.value;
    try {
      localStorage.setItem(LANG_KEY, state.trLang);
    } catch (_) {}
    if (state.transcriber && !state.transcriber.running) {
      const lines = state.transcriber.lines;
      state.transcriber = null;
      if (lines.length) transcriptStatus("Language changed. Copy or download the transcript so far before starting again.");
    }
    void checkTranscription();
  });
  $("tr-copy").addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(transcriptBody());
      transcriptStatus("Copied.");
    } catch (_) {
      transcriptStatus("Copying was blocked. Use Download instead.");
    }
  });
  $("tr-download").addEventListener("click", downloadTranscript);
  $("tr-clear").addEventListener("click", () => {
    state.transcriber?.clear();
    renderTranscript();
  });
  window.addEventListener("pagehide", () => {
    if (groupCalls.active) groupCalls.leave();
    stopMedia();
  });
  window.addEventListener("beforeunload", (event) => {
    if (groupCalls.active && state.transcriber?.lines.length) {
      event.preventDefault();
      event.returnValue = "";
    }
  });
}

async function start() {
  try {
    state.name = cleanMeetingName(localStorage.getItem(NAME_KEY) || "");
    state.trLang = localStorage.getItem(LANG_KEY) || transcribeLanguage();
  } catch (_) {
    state.trLang = transcribeLanguage();
  }
  if (!TRANSCRIBE_LANGUAGES.some(([code]) => code === state.trLang)) state.trLang = transcribeLanguage();
  $("name").value = state.name;
  wire();
  if (link.error) {
    $("lock").hidden = true;
    message(link.error);
    return;
  }
  try {
    state.keys = await deriveMeetingKeys(link.key, link.room);
  } catch (_) {
    message("key");
    return;
  }
  $("code").textContent = state.keys.code.digits;
  $("code-words").textContent = state.keys.code.words.join(" · ");
  await refreshRoom();
  if (state.ended || !state.info) return;
  show("lobby");
  renderLobby();
  state.refreshTimer = setInterval(() => {
    if (!groupCalls.active) void refreshRoom();
  }, ROOM_REFRESH_MS);
  try {
    await loadEffects();
  } catch (_) {
    $("effects-note").textContent = "Effects could not load. You can still join.";
    state.m = state.m || null;
  }
  if (state.m) await openMedia();
  else {
    try {
      state.stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: true });
      state.hasCamera = state.stream.getVideoTracks().length > 0;
      $("preview").srcObject = state.stream;
    } catch (_) {
      $("preview-text").textContent = "Allow the camera and microphone for this page, then refresh.";
    }
    renderLobby();
  }
}

void start();
