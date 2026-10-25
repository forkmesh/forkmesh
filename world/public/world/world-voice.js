import { createGroupCalls } from "/chat-group-calls.js";
import { createFmmLocal } from "/fmm/fmm-local.js";
import { AVATAR_FACE_ID, avatarFaceImage } from "/chat-call-face-custom.js";
import { CALL_FACE_STORAGE_KEY } from "/chat-call-faces.js";
import { callVoiceChoices, callVoicesSupported, rememberVoiceChoice, savedVoiceChoice } from "/chat-call-voice.js";
import { createCallQualityMeter, diagnoseCall, diagnosticsEnvironment, gradeCallQuality, rtcStatsReader } from "/call-quality-meter.js";
import { createSpeechMeter } from "/call-speech-meter.js";
import { RIG_REACTIONS, RIG_SEND_HZ, encodeRig } from "/call-avatar-rig-wire.js";
import { VOICE_CHART_HEIGHT, VOICE_CHART_SAMPLE_MS, VOICE_CHART_WIDTH, drawVoiceChart, pushVoiceSample, voiceChartSummary } from "./world-voice-chart.js";
import { WORLD_VOICE_NAME_MAX, WORLD_VOICE_ROOM_RE, bubbleTag, cleanVoiceRoomName, randomHex, voiceRoomsFromPlayers, worldVoiceRoomLabel } from "./world-voice-rooms.js";

export const WORLD_VOICE_HANDS_KEY = "forkmesh.world.voiceHands";
export const WORLD_VOICE_VIDEO_KEY = "forkmesh.world.voiceVideo";
export const WORLD_VOICE_CHART_KEY = "forkmesh.world.voiceChart";
export const WORLD_VOICE_REACTIONS = Object.freeze(["thumbs_up", "clap", "heart", "laugh", "fire", "open_palm"]);

const RIG_INTERVAL_MS = 1000 / RIG_SEND_HZ;
const AUDIO_RIG_MS = 250;
const AUDIO_RIG_IDLE_MS = 2500;
const ANCHOR_RESEND_MS = 3000;
const WORDS_PRESENCE_MS = 5000;
const PERF_MS = 1000;
const BUBBLE_SYNC_MS = 1000;
const INVITE_TTL_MS = 120000;

function element(documentRef, tag, className = "", text = "") {
  const node = documentRef.createElement(tag);
  if (className) node.className = className;
  if (text) node.textContent = text;
  return node;
}

function button(documentRef, label, onClick, { className = "world-voice-button", title = "", pressed = null } = {}) {
  const node = element(documentRef, "button", className, label);
  node.type = "button";
  if (title) {
    node.title = title;
    node.setAttribute("aria-label", title);
  }
  if (pressed !== null) node.setAttribute("aria-pressed", pressed ? "true" : "false");
  node.addEventListener("click", onClick);
  return node;
}

export function createWorldVoice(host) {
  const documentRef = host.documentRef || globalThis.document;
  const scope = host.scope || globalThis;
  const storage = host.storage || scope.localStorage;
  const state = {
    room: null,
    mic: false,
    cam: false,
    sendVideo: false,
    hands: true,
    chart: true,
    joining: false,
    error: "",
    invites: new Map(),
    knocks: new Map(),
    panelOpen: false,
    diagnosticsOpen: false,
    reportStatus: "",
    words: 0,
    talking: false,
    level: 0,
    lastWordsPresence: 0,
    lastRigSent: 0,
    lastAnchorsSent: 0,
    lastAudioRig: 0,
    anchors: null,
    anchorsFor: "",
    perf: { fps: 0, frameMs: 0, trackingMs: 0, trackingFps: 0, rttMs: null, lossPct: null, jitterMs: null, grade: "unknown" },
  };
  try {
    state.hands = storage?.getItem(WORLD_VOICE_HANDS_KEY) !== "off";
    state.chart = storage?.getItem(WORLD_VOICE_CHART_KEY) !== "off";
  } catch (_) {}
  const chartSamples = [];
  let chartSummaryAt = 0;
  let micStream = null;
  let camStream = null;
  let silentContext = null;
  let silentTrack = null;
  let capture = null;
  let speech = null;
  let meter = null;
  let destroyed = false;
  const audioElements = new Map();
  const timers = [];
  const frameTimes = [];
  let rafId = 0;

  const localMedia = createFmmLocal({
    storage,
    documentRef,
    scope,
    avatarImage: () => ownAvatarImage(),
    faceChoice: () => {
      try {
        return storage?.getItem(CALL_FACE_STORAGE_KEY) || AVATAR_FACE_ID;
      } catch (_) {
        return AVATAR_FACE_ID;
      }
    },
  });

  const calls = createGroupCalls({
    sendFrame: (channelKey, type, body) => Promise.resolve(host.send({ type: "voice-signal", room: channelKey, frame: { type, ...body } })),
    fetchTicket: (channelId) => voiceTicket(channelId),
    accountName: () => String(host.session()?.nodeName || "").trim().toLowerCase(),
    onChange: () => {
      syncAudio();
      render();
    },
    acquireMedia: (media) => callMedia(media),
    prepareLocal: localMedia,
    fmm: { enabled: false },
    scope,
  });

  function ownAvatarImage() {
    const png = String(host.session()?.avatarPng || "");
    if (!png) return "";
    return avatarFaceImage(png.startsWith("data:") ? png : `data:image/png;base64,${png}`);
  }

  function signedIn() {
    const session = host.session();
    return Boolean(session?.sessionToken && session?.nodeName);
  }

  function selfName() {
    return String(host.session()?.nodeName || "You");
  }

  async function voiceTicket(channelId) {
    const room = state.room;
    const response = await scope.fetch("/api/world/voice/ticket", {
      method: "POST",
      credentials: "same-origin",
      cache: "no-store",
      headers: host.apiHeaders({ "content-type": "application/json" }),
      body: JSON.stringify({ room: channelId, secret: room?.id === channelId ? room.secret || "" : "" }),
    });
    const body = await response.json().catch(() => ({}));
    if (!response.ok) {
      state.error = body.error === "sign_in_required"
        ? "Sign in to talk in the World."
        : body.error === "calls_unavailable"
          ? "Voice is not available on this server right now."
          : "Could not join the voice room.";
      throw new Error(body.error || "ticket_failed");
    }
    return body;
  }

  function silentAudioTrack() {
    if (silentTrack && silentTrack.readyState !== "ended") return silentTrack;
    const AudioContextClass = scope.AudioContext || scope.webkitAudioContext;
    if (!AudioContextClass) return null;
    silentContext ||= new AudioContextClass();
    silentTrack = silentContext.createMediaStreamDestination().stream.getAudioTracks()[0] || null;
    return silentTrack;
  }

  async function callMedia(media) {
    const tracks = [];
    const audio = state.mic && micStream?.getAudioTracks()[0]?.readyState === "live" ? micStream.getAudioTracks()[0] : silentAudioTrack();
    if (audio) tracks.push(audio);
    if (media === "video" && camStream?.getVideoTracks()[0]) tracks.push(camStream.getVideoTracks()[0]);
    return new scope.MediaStream(tracks);
  }

  function presence() {
    return {
      voice: state.room?.id || "",
      voiceName: state.room?.id?.startsWith("adhoc:") ? state.room.name || "" : "",
      mic: Boolean(state.room && state.mic),
      cam: Boolean(state.room && state.cam),
      words: state.words,
    };
  }

  function announce() {
    host.presenceNow();
  }

  function defaultRoom() {
    const space = String(host.chatSpace?.() || "");
    if (space && space !== "general" && WORLD_VOICE_ROOM_RE.test(`space:${space}`)) return { id: `space:${space}` };
    return { id: "world" };
  }

  async function join(room) {
    if (destroyed) return false;
    if (!signedIn()) {
      host.notify("Sign in to talk in the World.");
      return false;
    }
    const id = String(room?.id || "");
    if (!WORLD_VOICE_ROOM_RE.test(id) || id === "channel") return false;
    if (state.room?.id === id && calls.active) return true;
    if (calls.active) calls.leave();
    state.room = { id, name: cleanVoiceRoomName(room.name || ""), secret: String(room.secret || "") };
    state.error = "";
    state.joining = true;
    announce();
    render();
    await new Promise((resolve) => scope.setTimeout(resolve, 150));
    const media = state.sendVideo && camStream ? "video" : "audio";
    const result = await calls.join(id, id, media);
    state.joining = false;
    if (result?.error) {
      state.error = state.error || result.error;
      state.room = null;
      announce();
    } else if (!state.mic && calls.active && !calls.active.muted) {
      calls.toggleMute();
    }
    startMeter();
    syncBubbles();
    render();
    return !result?.error;
  }

  function leave() {
    if (calls.active) calls.leave();
    const wasBubble = state.room?.id?.startsWith("bubble:");
    state.room = null;
    state.joining = false;
    stopMeter();
    announce();
    syncAudio();
    syncBubbles();
    if (wasBubble) host.notify("You left the privacy bubble.");
    render();
  }

  async function rejoin() {
    if (!state.room) return;
    const room = state.room;
    calls.leave();
    await join(room);
  }

  async function openMic() {
    if (micStream?.getAudioTracks()[0]?.readyState === "live") return true;
    try {
      micStream = await scope.navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true },
      });
    } catch (_) {
      host.notify("Allow the microphone to talk in the World.");
      return false;
    }
    speech?.stop();
    speech = createSpeechMeter({
      track: micStream.getAudioTracks()[0],
      scope,
      onChange: (next) => {
        state.talking = next.talking && state.mic;
        state.level = next.level;
        state.words = Math.max(state.words, next.words);
        host.puppets()?.speech("self", { words: state.words, talking: state.talking, inVoice: Boolean(state.room) });
        if (Date.now() - state.lastWordsPresence > WORDS_PRESENCE_MS) {
          state.lastWordsPresence = Date.now();
          host.presenceSoon();
        }
      },
    });
    return true;
  }

  function closeMic() {
    speech?.stop();
    speech = null;
    micStream?.getTracks().forEach((track) => track.stop());
    micStream = null;
    state.talking = false;
  }

  async function toggleMic() {
    if (!signedIn()) {
      host.notify("Sign in to talk in the World.");
      return;
    }
    if (!state.mic) {
      if (!(await openMic())) return;
      state.mic = true;
      micStream.getAudioTracks().forEach((track) => { track.enabled = true; });
      if (!state.room) {
        await join(defaultRoom());
      } else if (calls.active) {
        const usingMic = calls.active.local?.getAudioTracks?.()[0] === micStream.getAudioTracks()[0];
        if (!usingMic) await rejoin();
        else if (calls.active.muted) calls.toggleMute();
      }
    } else {
      state.mic = false;
      state.talking = false;
      if (calls.active && !calls.active.muted) calls.toggleMute();
      else micStream?.getAudioTracks().forEach((track) => { track.enabled = false; });
    }
    announce();
    render();
  }

  async function startCapture() {
    try {
      camStream = await scope.navigator.mediaDevices.getUserMedia({
        video: { width: { ideal: 640 }, height: { ideal: 360 }, frameRate: { ideal: 24 } },
      });
    } catch (_) {
      host.notify("Allow the camera to move your avatar with your face and hands.");
      return false;
    }
    const module = await import("/call-avatar-rig.js");
    if (destroyed || !camStream) return false;
    capture = module.createAvatarCapture({
      stream: camStream,
      documentRef,
      scope,
      hands: state.hands,
      onFrame: (rig) => onCaptureFrame(rig),
      onGesture: (gesture) => react(gesture, { fromGesture: true }),
    });
    void ensureAnchors(module);
    let ready = false;
    try {
      ready = await capture.ready;
    } catch (error) {
      scope.console?.warn?.("World camera: face tracking failed to start", error);
    }
    if (!ready) {
      capture?.stop();
      capture = null;
      if (state.sendVideo && camStream) {
        host.notify("This browser cannot follow your face, so your camera sends video only.");
        return true;
      }
      host.notify("This browser cannot follow your face. Your avatar stays as it is.");
      stopCapture();
      return false;
    }
    return true;
  }

  function stopCapture() {
    capture?.stop();
    capture = null;
    camStream?.getTracks().forEach((track) => track.stop());
    camStream = null;
    host.send({ type: "rig", ...encodeRig({ yaw: 0, pitch: 0, roll: 0, hands: [], talking: state.talking, level: state.level, words: state.words }) });
    host.puppets()?.local("self", { yaw: 0, pitch: 0, roll: 0, deltas: null, hands: [], talking: state.talking, words: state.words });
  }

  async function ensureAnchors(module) {
    const image = ownAvatarImage();
    if (!image || !host.wearsPhotoFace?.()) {
      state.anchors = null;
      return;
    }
    if (state.anchorsFor === image) return;
    state.anchorsFor = image;
    const element = new scope.Image();
    element.src = image;
    try {
      await element.decode();
      state.anchors = await module.detectFaceAnchors(element, { documentRef });
    } catch (_) {
      state.anchors = null;
    }
  }

  async function toggleCamera() {
    if (!signedIn()) {
      host.notify("Sign in to use your camera in the World.");
      return;
    }
    if (!state.cam) {
      state.cam = true;
      render();
      if (!(await startCapture())) {
        state.cam = false;
        render();
        return;
      }
      if (state.sendVideo && state.room) await rejoin();
    } else {
      state.cam = false;
      stopCapture();
      if (state.sendVideo && state.room) await rejoin();
    }
    announce();
    render();
  }

  function onCaptureFrame(frame) {
    if (destroyed) return;
    const rig = frame.tracked
      ? { ...frame, talking: state.talking, level: state.level, words: state.words }
      : { yaw: 0, pitch: 0, roll: 0, deltas: null, hands: frame.hands || [], talking: state.talking, level: state.level, words: state.words };
    host.puppets()?.local("self", state.anchors ? { ...rig, anchors: state.anchors } : rig);
    const now = scope.performance.now();
    if (now - state.lastRigSent < RIG_INTERVAL_MS) return;
    state.lastRigSent = now;
    const withAnchors = state.anchors && now - state.lastAnchorsSent > ANCHOR_RESEND_MS;
    if (withAnchors) state.lastAnchorsSent = now;
    host.send({ type: "rig", ...encodeRig({ ...rig, anchors: withAnchors ? state.anchors : null }) });
  }

  function audioRigTick() {
    if (destroyed || capture || !state.room) return;
    const now = scope.performance.now();
    const due = state.talking ? AUDIO_RIG_MS : AUDIO_RIG_IDLE_MS;
    if (now - state.lastAudioRig < due) return;
    state.lastAudioRig = now;
    host.send({ type: "rig", ...encodeRig({ yaw: 0, pitch: 0, roll: 0, hands: [], talking: state.talking, level: state.level, words: state.words }) });
  }

  function react(kind, { fromGesture = false } = {}) {
    if (!RIG_REACTIONS[kind] || kind === "fist" || kind === "point_up") return;
    const target = host.puppets()?.currentSpeaker(host.selfId()) || "";
    host.send({ type: "interaction", kind: "reaction", reaction: kind, target });
    host.world()?.playAvatarReaction?.("self", target || "self", RIG_REACTIONS[kind].emoji);
    host.popReaction?.(RIG_REACTIONS[kind].emoji, `You · ${RIG_REACTIONS[kind].label}`);
    const name = target ? host.players().get(target)?.name || "the speaker" : "";
    host.notify(`${RIG_REACTIONS[kind].emoji} ${fromGesture ? "Gesture seen: " : ""}${RIG_REACTIONS[kind].label}${name ? ` sent to ${name}` : ""}.`);
  }

  async function startBubble(peerId) {
    if (!signedIn()) {
      host.notify("Sign in to start a privacy bubble.");
      return;
    }
    if (!state.room?.id?.startsWith("bubble:")) {
      const secret = randomHex(16, scope);
      const tag = await bubbleTag(secret, scope);
      if (!(await join({ id: `bubble:${tag}`, secret, name: "Privacy bubble" }))) return;
    }
    if (peerId) invite(peerId);
  }

  function invite(peerId) {
    if (!state.room?.id?.startsWith("bubble:") || !peerId) return false;
    const sent = host.send({ type: "interaction", kind: "bubble-invite", target: String(peerId), room: state.room.id, secret: state.room.secret });
    const name = host.players().get(String(peerId))?.name || "them";
    if (sent) host.notify(`Invited ${name} into your privacy bubble.`);
    return sent;
  }

  function knock(peerId) {
    const sent = host.send({ type: "interaction", kind: "bubble-knock", target: String(peerId) });
    if (sent) host.notify("Asked to join their privacy bubble.");
    return sent;
  }

  async function acceptInvite(fromId) {
    const invitation = state.invites.get(fromId);
    state.invites.delete(fromId);
    if (!invitation) return;
    if (!state.mic && (await openMic())) state.mic = true;
    await join({ id: invitation.room, secret: invitation.secret, name: "Privacy bubble" });
  }

  function declineInvite(fromId) {
    state.invites.delete(fromId);
    host.send({ type: "interaction", kind: "bubble-decline", target: fromId });
    render();
  }

  function createRoom(name) {
    const clean = cleanVoiceRoomName(name) || "Voice room";
    return join({ id: `adhoc:${randomHex(8, scope)}`, name: clean });
  }

  function handle(message) {
    if (!message || typeof message !== "object") return false;
    if (message.type === "voice-signal") {
      if (!state.room || message.room !== state.room.id || !message.frame) return true;
      calls.handleFrame({ ...message.frame, sender: String(message.sender || "") }, state.room.id);
      return true;
    }
    if (message.type !== "interaction") return false;
    const from = String(message.from || "");
    const name = host.players().get(from)?.name || "Someone";
    if (message.kind === "bubble-invite" && WORLD_VOICE_ROOM_RE.test(String(message.room || ""))) {
      state.invites.set(from, { room: String(message.room), secret: String(message.secret || ""), name, at: Date.now() });
      host.notify(`${name} invited you into a privacy bubble. Open voice to join.`);
      state.panelOpen = true;
      render();
      return true;
    }
    if (message.kind === "bubble-knock") {
      if (!state.room?.id?.startsWith("bubble:")) return true;
      state.knocks.set(from, { name, at: Date.now() });
      host.notify(`${name} would like to join your privacy bubble.`);
      state.panelOpen = true;
      render();
      return true;
    }
    if (message.kind === "bubble-decline") {
      host.notify(`${name} did not join the bubble.`);
      return true;
    }
    return false;
  }

  function syncAudio() {
    const tiles = calls.active?.tiles || new Map();
    for (const [peer, tile] of tiles) {
      let audio = audioElements.get(peer);
      if (!audio) {
        audio = documentRef.createElement("audio");
        audio.autoplay = true;
        audio.dataset.worldVoicePeer = peer;
        host.audioHost().append(audio);
        audioElements.set(peer, audio);
      }
      if (audio.srcObject !== tile.stream) {
        audio.srcObject = tile.stream;
        void audio.play?.().catch?.(() => {});
      }
    }
    for (const [peer, audio] of audioElements) {
      if (tiles.has(peer)) continue;
      audio.srcObject = null;
      audio.remove();
      audioElements.delete(peer);
    }
  }

  function syncBubbles() {
    host.syncBubbles();
  }

  function startMeter() {
    if (meter) return;
    meter = createCallQualityMeter({
      readStats: rtcStatsReader(() => [calls.active?.pc].filter(Boolean)),
      tracks: () => ({
        mic: micStream?.getAudioTracks()[0] || null,
        peers: [...(calls.active?.tiles?.values?.() || [])].flatMap((tile) => tile.stream?.getAudioTracks?.() || []),
      }),
      face: () => (capture ? capture.stats() : null),
      scope,
    });
  }

  function stopMeter() {
    meter?.stop();
    meter = null;
  }

  function trackFrames(time) {
    frameTimes.push(time);
    while (frameTimes.length && time - frameTimes[0] > 1000) frameTimes.shift();
    rafId = scope.requestAnimationFrame?.(trackFrames) || 0;
  }

  function updatePerf() {
    const latest = meter?.latest || null;
    const captureStats = capture?.stats() || null;
    const fps = frameTimes.length;
    const frameMs = fps ? 1000 / fps : 0;
    state.perf = {
      fps,
      frameMs,
      trackingMs: captureStats ? captureStats.faceMs + (captureStats.handsOn ? captureStats.handMs / 2 : 0) : 0,
      trackingFps: captureStats?.fps || 0,
      tracked: Boolean(captureStats?.tracked),
      hands: captureStats?.hands || 0,
      rttMs: latest?.rttMs ?? null,
      lossPct: latest?.lossPct ?? null,
      jitterMs: latest?.jitterMs ?? null,
      grade: perfGrade(fps, captureStats, latest),
    };
    renderPerf();
  }

  function perfGrade(fps, captureStats, latest) {
    const call = latest ? gradeCallQuality(latest) : "unknown";
    const slowFrames = fps && fps < 24;
    const slowTracking = captureStats && captureStats.faceMs > 50;
    if (call === "poor" || (fps && fps < 15)) return "poor";
    if (call === "fair" || slowFrames || slowTracking) return "fair";
    if (call === "good" || fps >= 24) return "good";
    return "unknown";
  }

  function diagnosticsReport() {
    const players = host.players();
    const diagnosis = meter ? diagnoseCall(meter.samples, { video: state.sendVideo, sending: state.mic }) : null;
    return {
      kind: "world-voice-diagnostics",
      at: new Date().toISOString(),
      room: state.room ? { kind: state.room.id.split(":")[0], members: voiceRoomsFromPlayers(players, { room: state.room }).find((room) => room.id === state.room.id)?.members.length || 1 } : null,
      mic: state.mic,
      camera: state.cam,
      sendVideo: state.sendVideo,
      hands: state.hands,
      engine: calls.active?.engine || "",
      callState: calls.active?.state || "",
      callError: state.error || calls.lastError || "",
      perf: state.perf,
      capture: capture?.stats() || null,
      words: state.words,
      samples: (meter?.samples || []).slice(-20),
      diagnosis,
      environment: diagnosticsEnvironment(scope),
      world: host.diagnostics?.() || null,
    };
  }

  function base64(text) {
    const bytes = new TextEncoder().encode(text);
    let raw = "";
    for (let i = 0; i < bytes.length; i += 0x8000) raw += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return scope.btoa(raw);
  }

  async function sendDiagnostics(note = "") {
    const report = diagnosticsReport();
    const issues = report.diagnosis?.issues?.map((issue) => issue.title).join("; ") || "none detected";
    const message = [
      "World voice/video diagnostic report.",
      note ? `What I noticed: ${String(note).slice(0, 600)}` : "",
      `Room: ${report.room ? report.room.kind : "none"} · mic ${report.mic ? "on" : "off"} · camera ${report.camera ? "on" : "off"} · engine ${report.engine || "none"} · state ${report.callState || "none"}`,
      `Frames ${report.perf.fps} fps · tracking ${report.perf.trackingMs.toFixed(1)} ms at ${report.perf.trackingFps} fps · RTT ${report.perf.rttMs ?? "?"} ms · loss ${report.perf.lossPct ?? "?"}% · jitter ${report.perf.jitterMs ?? "?"} ms · grade ${report.perf.grade}`,
      `Detected issues: ${issues}`,
      report.callError ? `Last error: ${report.callError}` : "",
    ].filter(Boolean).join("\n");
    state.reportStatus = "Sending…";
    render();
    try {
      const response = await scope.fetch("/api/bug-reports", {
        method: "POST",
        credentials: "same-origin",
        headers: host.apiHeaders({ "content-type": "application/json" }),
        body: JSON.stringify({
          message,
          severity: report.perf.grade === "poor" ? "high" : "normal",
          source: "web",
          screen: "world-voice",
          platform: String(scope.navigator?.platform || "").slice(0, 40),
          appVersion: String(host.version?.() || "").slice(0, 60),
          device: String(scope.navigator?.userAgent || "").slice(0, 120),
          attachments: [{ name: "world-voice-diagnostics.json", contentType: "application/json", data: base64(JSON.stringify(report, null, 2)) }],
        }),
      });
      state.reportStatus = response.ok
        ? "Diagnostic report sent. Thank you."
        : response.status === 429 ? "Too many reports this hour. Try again later." : "The report could not be sent.";
    } catch (_) {
      state.reportStatus = "The report could not be sent.";
    }
    render();
    return state.reportStatus;
  }

  function setHands(on) {
    state.hands = Boolean(on);
    try {
      storage?.setItem(WORLD_VOICE_HANDS_KEY, state.hands ? "on" : "off");
    } catch (_) {}
    capture?.setHands(state.hands);
    render();
  }

  async function setSendVideo(on) {
    state.sendVideo = Boolean(on);
    if (state.room && camStream) await rejoin();
    render();
  }

  function setVoiceChoice(choice) {
    rememberVoiceChoice(choice, storage);
    if (state.room) void rejoin();
  }

  function live() {
    return Boolean(state.room || state.mic || state.cam);
  }

  function sampleChart() {
    if (destroyed) return;
    const figure = host.chartElement?.();
    const visible = live() && state.chart;
    if (figure && figure.hidden === visible) figure.hidden = !visible;
    if (!live()) {
      chartSamples.length = 0;
      return;
    }
    const captureStats = capture?.stats() || null;
    pushVoiceSample(chartSamples, {
      at: Date.now(),
      fps: frameTimes.length,
      level: state.mic ? state.level : 0,
      trackingMs: captureStats ? captureStats.faceMs + (captureStats.handsOn ? captureStats.handMs / 2 : 0) : null,
      rttMs: meter?.latest?.rttMs ?? null,
    });
    if (!visible || !figure) return;
    const canvas = figure.querySelector("canvas");
    if (!canvas) return;
    const ratio = Math.min(2, Math.max(1, Number(scope.devicePixelRatio) || 1));
    if (canvas.width !== Math.round(VOICE_CHART_WIDTH * ratio)) {
      canvas.width = Math.round(VOICE_CHART_WIDTH * ratio);
      canvas.height = Math.round(VOICE_CHART_HEIGHT * ratio);
    }
    drawVoiceChart(canvas.getContext("2d"), chartSamples, { ratio });
    const now = Date.now();
    if (now - chartSummaryAt > 2000) {
      chartSummaryAt = now;
      const summary = `Live diagnostics, last 30 seconds: ${voiceChartSummary(chartSamples)}.`;
      canvas.setAttribute("aria-label", summary);
      figure.title = `${summary} Click for details and a diagnostic report.`;
    }
  }

  function setChart(on) {
    state.chart = Boolean(on);
    try {
      storage?.setItem(WORLD_VOICE_CHART_KEY, state.chart ? "on" : "off");
    } catch (_) {}
    sampleChart();
    render();
  }

  function renderPerf() {
    const pill = host.perfElement?.();
    if (!pill) return;
    const visible = Boolean(state.room || state.mic || state.cam);
    pill.hidden = !visible;
    if (!visible) return;
    const perf = state.perf;
    pill.dataset.grade = perf.grade;
    const parts = [`${perf.fps} fps`];
    if (state.cam) parts.push(`${Math.round(perf.trackingMs)} ms`);
    if (perf.rttMs !== null && perf.rttMs !== undefined) parts.push(`${Math.round(perf.rttMs)} ms rtt`);
    const text = pill.querySelector("[data-world-voice-perf-text]");
    if (text) text.textContent = parts.join(" · ");
    pill.title = `Voice and video performance: ${parts.join(", ")}. Click for details and a diagnostic report.`;
  }

  function memberRow(member) {
    const row = element(documentRef, "li", "world-voice-member");
    const speaking = host.puppets()?.speakers?.().find((entry) => entry.id === member.id);
    row.dataset.state = member.self ? (state.talking ? "talking" : state.mic ? "recent" : "quiet") : speaking?.state || "quiet";
    row.append(element(documentRef, "span", "world-voice-member-dot"));
    row.append(element(documentRef, "span", "world-voice-member-name", member.self ? `${member.name} (you)` : member.name));
    const flags = [];
    if (!member.mic) flags.push("muted");
    if (member.cam) flags.push("camera");
    if (flags.length) row.append(element(documentRef, "span", "world-voice-member-flags", flags.join(" · ")));
    const words = member.self ? state.words : speaking?.words ?? member.words;
    row.append(element(documentRef, "span", "world-voice-member-words", `${words} words`));
    if (!member.self && state.room && !state.room.id.startsWith("bubble:")) {
      row.append(button(documentRef, "Bubble", () => void startBubble(member.id), { className: "world-voice-mini", title: `Start a privacy bubble with ${member.name}` }));
    }
    return row;
  }

  function render() {
    renderPerf();
    host.syncButtons?.({ mic: state.mic, cam: state.cam, room: state.room?.id || "", joining: state.joining });
    const panel = host.panelElement?.();
    if (!panel) return;
    panel.hidden = !state.panelOpen;
    if (!state.panelOpen) return;
    panel.replaceChildren();
    const head = element(documentRef, "header", "world-voice-head");
    head.append(element(documentRef, "strong", "", state.room ? worldVoiceRoomLabel(state.room, host.players()) : "Voice and video"));
    head.append(button(documentRef, "×", () => {
      state.panelOpen = false;
      render();
    }, { className: "world-voice-close", title: "Close voice panel" }));
    panel.append(head);
    if (!signedIn()) {
      panel.append(element(documentRef, "p", "world-voice-note", "Sign in to talk, use your camera and join rooms."));
      return;
    }
    const status = state.joining ? "Joining…" : calls.active?.state === "reconnecting" ? "Reconnecting…" : state.room ? (state.room.id === "world" ? "Everyone in the World can hear this room." : state.room.id.startsWith("bubble:") ? "Only people inside this bubble can hear you." : state.room.id.startsWith("adhoc:") ? "Only people in this room can hear you." : "Everyone in this World channel can hear you.") : "Not in a voice room.";
    panel.append(element(documentRef, "p", "world-voice-note", status));
    if (state.error) panel.append(element(documentRef, "p", "world-voice-error", state.error));
    for (const [from, invitation] of state.invites) {
      if (Date.now() - invitation.at > INVITE_TTL_MS) {
        state.invites.delete(from);
        continue;
      }
      const box = element(documentRef, "div", "world-voice-invite");
      box.append(element(documentRef, "span", "", `${invitation.name} invited you into a privacy bubble.`));
      box.append(button(documentRef, "Join", () => void acceptInvite(from), { className: "world-voice-button world-voice-primary" }));
      box.append(button(documentRef, "Decline", () => declineInvite(from)));
      panel.append(box);
    }
    for (const [from, knockEntry] of state.knocks) {
      if (Date.now() - knockEntry.at > INVITE_TTL_MS || !state.room?.id?.startsWith("bubble:")) {
        state.knocks.delete(from);
        continue;
      }
      const box = element(documentRef, "div", "world-voice-invite");
      box.append(element(documentRef, "span", "", `${knockEntry.name} would like to join your bubble.`));
      box.append(button(documentRef, "Let in", () => {
        state.knocks.delete(from);
        invite(from);
        render();
      }, { className: "world-voice-button world-voice-primary" }));
      box.append(button(documentRef, "Not now", () => {
        state.knocks.delete(from);
        host.send({ type: "interaction", kind: "bubble-decline", target: from });
        render();
      }));
      panel.append(box);
    }
    const controls = element(documentRef, "div", "world-voice-controls");
    controls.append(button(documentRef, state.mic ? "Mute" : state.room ? "Unmute" : "Talk", () => void toggleMic(), { pressed: state.mic, title: state.mic ? "Mute your microphone" : "Turn on your microphone" }));
    controls.append(button(documentRef, state.cam ? "Camera off" : "Camera on", () => void toggleCamera(), { pressed: state.cam, title: "Your camera moves your avatar's face, head and hands; no video leaves your device unless you also send video" }));
    if (state.room) controls.append(button(documentRef, "Leave", leave, { title: "Leave this voice room" }));
    else controls.append(button(documentRef, "Join #general", () => void join({ id: "world" }), { className: "world-voice-button world-voice-primary", title: "Join the whole-World voice room" }));
    panel.append(controls);
    const options = element(documentRef, "div", "world-voice-options");
    const handsLabel = element(documentRef, "label", "world-voice-check");
    const handsBox = element(documentRef, "input");
    handsBox.type = "checkbox";
    handsBox.checked = state.hands;
    handsBox.addEventListener("change", () => setHands(handsBox.checked));
    handsLabel.append(handsBox, documentRef.createTextNode(" Hand gestures send reactions"));
    options.append(handsLabel);
    const chartLabel = element(documentRef, "label", "world-voice-check");
    const chartBox = element(documentRef, "input");
    chartBox.type = "checkbox";
    chartBox.checked = state.chart;
    chartBox.addEventListener("change", () => setChart(chartBox.checked));
    chartLabel.append(chartBox, documentRef.createTextNode(" Show the live diagnostics chart"));
    options.append(chartLabel);
    const videoLabel = element(documentRef, "label", "world-voice-check");
    const videoBox = element(documentRef, "input");
    videoBox.type = "checkbox";
    videoBox.checked = state.sendVideo;
    videoBox.addEventListener("change", () => void setSendVideo(videoBox.checked));
    videoLabel.append(videoBox, documentRef.createTextNode(" Also send video (your avatar's face by default)"));
    options.append(videoLabel);
    if (callVoicesSupported(scope)) {
      const voiceLabel = element(documentRef, "label", "world-voice-select");
      voiceLabel.append(documentRef.createTextNode("Voice "));
      const select = element(documentRef, "select");
      const current = savedVoiceChoice(storage);
      for (const [value, label] of callVoiceChoices()) {
        const option = element(documentRef, "option", "", label);
        option.value = value;
        option.selected = value === current;
        select.append(option);
      }
      select.addEventListener("change", () => setVoiceChoice(select.value));
      voiceLabel.append(select);
      options.append(voiceLabel);
    }
    panel.append(options);
    const reactions = element(documentRef, "div", "world-voice-reactions");
    reactions.setAttribute("role", "group");
    reactions.setAttribute("aria-label", "Send a reaction to whoever is speaking");
    for (const kind of WORLD_VOICE_REACTIONS) {
      reactions.append(button(documentRef, RIG_REACTIONS[kind].emoji, () => react(kind), { className: "world-voice-reaction", title: `${RIG_REACTIONS[kind].label} to the speaker` }));
    }
    panel.append(reactions);
    const rooms = voiceRoomsFromPlayers(host.players(), { ...state, name: selfName() });
    const current = state.room ? rooms.find((room) => room.id === state.room.id) : null;
    if (current) {
      const list = element(documentRef, "ul", "world-voice-members");
      current.members.forEach((member) => list.append(memberRow(member)));
      panel.append(list);
    }
    if (state.room?.id?.startsWith("bubble:")) {
      const nearby = host.nearbyPlayers?.(12) || [];
      const inviteRow = element(documentRef, "div", "world-voice-nearby");
      inviteRow.append(element(documentRef, "span", "world-voice-note", nearby.length ? "Invite someone nearby:" : "Walk up to someone to invite them in."));
      for (const peer of nearby) {
        if (current?.members.some((member) => member.id === peer.id)) continue;
        inviteRow.append(button(documentRef, peer.name, () => invite(peer.id), { className: "world-voice-mini", title: `Invite ${peer.name} into the bubble` }));
      }
      panel.append(inviteRow);
    }
    const adhoc = element(documentRef, "form", "world-voice-new-room");
    const nameInput = element(documentRef, "input");
    nameInput.placeholder = "New voice room name";
    nameInput.maxLength = WORLD_VOICE_NAME_MAX;
    nameInput.setAttribute("aria-label", "New voice room name");
    adhoc.append(nameInput, button(documentRef, "Start room", () => {}, { title: "Start a voice room only the people who join can hear" }));
    adhoc.addEventListener("submit", (event) => {
      event.preventDefault();
      void createRoom(nameInput.value);
    });
    adhoc.querySelector("button").type = "submit";
    panel.append(adhoc);
    const perf = element(documentRef, "section", "world-voice-perf-detail");
    const p = state.perf;
    perf.append(element(documentRef, "strong", "", `Performance · ${p.grade}`));
    perf.append(element(documentRef, "p", "", `World ${p.fps} fps (${p.frameMs.toFixed(1)} ms a frame)${state.cam ? ` · face and hands ${p.trackingMs.toFixed(1)} ms at ${p.trackingFps} fps${p.tracked ? "" : " · no face found"}` : ""}`));
    perf.append(element(documentRef, "p", "", `Call ${calls.active?.state || "idle"}${p.rttMs !== null && p.rttMs !== undefined ? ` · RTT ${Math.round(p.rttMs)} ms` : ""}${p.lossPct !== null && p.lossPct !== undefined ? ` · loss ${p.lossPct.toFixed(1)}%` : ""}${p.jitterMs !== null && p.jitterMs !== undefined ? ` · jitter ${Math.round(p.jitterMs)} ms` : ""}`));
    perf.append(button(documentRef, "Send diagnostic report", () => void sendDiagnostics(), { className: "world-voice-button", title: "Send a bug report with these measurements to the ForkMesh team" }));
    if (state.reportStatus) perf.append(element(documentRef, "p", "world-voice-note", state.reportStatus));
    panel.append(perf);
  }

  function openPanel(open = !state.panelOpen) {
    state.panelOpen = Boolean(open);
    render();
  }

  timers.push(scope.setInterval(audioRigTick, 100));
  timers.push(scope.setInterval(updatePerf, PERF_MS));
  timers.push(scope.setInterval(sampleChart, VOICE_CHART_SAMPLE_MS));
  timers.push(scope.setInterval(syncBubbles, BUBBLE_SYNC_MS));
  rafId = scope.requestAnimationFrame?.(trackFrames) || 0;

  return {
    presence,
    handle,
    join,
    leave,
    toggleMic,
    toggleCamera,
    openPanel,
    startBubble,
    invite,
    knock,
    createRoom,
    react,
    sendDiagnostics,
    diagnosticsReport,
    setHands,
    setChart,
    chartSamples: () => chartSamples.slice(),
    setSendVideo,
    rooms: () => voiceRoomsFromPlayers(host.players(), { ...state, name: selfName() }),
    get room() {
      return state.room;
    },
    get state() {
      return { mic: state.mic, cam: state.cam, room: state.room?.id || "", words: state.words, talking: state.talking, perf: state.perf };
    },
    render,
    destroy() {
      destroyed = true;
      const figure = host.chartElement?.();
      if (figure) figure.hidden = true;
      leave();
      closeMic();
      if (capture || camStream) stopCapture();
      stopMeter();
      for (const timer of timers) scope.clearInterval(timer);
      if (rafId) scope.cancelAnimationFrame?.(rafId);
      void silentContext?.close?.();
    },
  };
}
