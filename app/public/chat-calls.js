import {
  CALL_FACES,
  CALL_FACE_STORAGE_KEY,
  callBackgroundsSupported,
  callFaceById,
  callFacesSupported,
  createFaceStage,
} from "./chat-call-faces.js";
import {
  AVATAR_FACE_ID,
  CUSTOM_FACE_ID,
  customFaceFromFile,
  faceImageFor,
  photoFaceId,
  rememberCustomFaceImage,
  savedCustomFaceImage,
  stageFaceFor,
} from "./chat-call-face-custom.js";
import {
  CALL_BACKGROUNDS,
  callBackgroundById,
  callBackgroundThumbUrl,
  customBackgroundFromFile,
  rememberCallBackground,
  savedCallBackground,
} from "./chat-call-backgrounds.js";
import {
  createVoicePreview,
  mountVoicePreviewControls,
} from "./chat-call-voice.js";
import {
  callVoiceChoices,
  callVoiceLabel,
  callVoicesSupported,
  createVoiceStage,
  normalizeVoiceChoice,
  rememberCallerPitch,
  rememberVoiceChoice,
  resolveCallVoice,
  savedCallerPitch,
  savedVoiceChoice,
} from "./chat-call-voice.js";
import {
  CALL_AUDIO_CONSTRAINTS,
  CALL_ROUTE_CHECK_MS,
  CALL_TEST_CLIP_MS,
  callMediaConstraints,
  callRouteLabel,
  preferCallAudioCodecs,
  readCallRoute,
  recordTestClip,
  tuneCallSdp,
  tuneCallSenders,
} from "./chat-call-quality.js";
import { createCallTones } from "./chat-call-tones.js";
import {
  createCallQualityMeter,
  mountCallQualityWidget,
  rtcStatsReader,
} from "./call-quality-meter.js";

const RING_TIMEOUT_MS = 45000;
const INVITE_RESEND_MS = 3000;
const INVITE_STALE_MS = 9000;
const ACCEPT_RESEND_MS = 2000;
const ACCEPT_RESENDS = 8;
const OFFER_RESEND_MS = 3000;
const OFFER_RESENDS = 5;
const CONNECT_TIMEOUT_MS = 30000;
const DROP_RESTART_MS = 2000;
const RESTART_EVERY_MS = 6000;
const DROP_GIVE_UP_MS = 30000;
const HEALTH_MS = 1000;
const AUDIO_SILENT_TICKS = 4;
const VIDEO_STALL_TICKS = 2;
const VIDEO_MIN_FRAMES = 2;
const STATS_SHARE_MS = 800;
const NOTICE_MS = 5000;
const ENDED_MS = 8000;
const VOICE_READY_MS = 2500;
const VIBRATE_PATTERN = [600, 300, 600, 1500];
const VIBRATE_EVERY_MS = 3000;
const TITLE_FLASH_MS = 1000;
const LIVE_TEXT = "Connected · end-to-end encrypted";
const RECONNECTING_TEXT = "Reconnecting…";
const LOCAL_CORNER_KEY = "fm.call.previewCorner";
const LOCAL_CORNERS = new Set(["top-left", "top-right", "bottom-left", "bottom-right"]);
const MINI_POSITION_KEY = "fm.call.miniPosition";
const MINI_MARGIN = 8;
const MINI_EDGE = 16;
const MINI_TAP_PX = 6;
const DECLINE_TEXT = Object.freeze({
  busy: "is on another call. Try again later.",
  media: "could not open their camera or microphone.",
  declined: "declined the call.",
});

export const CALL_FRAME_TYPES = new Set([
  "call-invite",
  "call-accept",
  "call-decline",
  "call-offer",
  "call-answer",
  "call-ice",
  "call-bye",
]);

export function callsSupported(globalRef = globalThis) {
  return Boolean(
    globalRef.isSecureContext &&
    globalRef.RTCPeerConnection &&
    globalRef.navigator?.mediaDevices?.getUserMedia,
  );
}

export function askCallNotifications(NotificationImpl = globalThis.Notification) {
  if (NotificationImpl?.permission !== "default" || typeof NotificationImpl.requestPermission !== "function") return;
  try {
    Promise.resolve(NotificationImpl.requestPermission()).catch(() => {});
  } catch (_) {}
}

function setText(element, text) {
  if (element && element.textContent !== text) element.textContent = text;
}

function setHidden(element, hidden) {
  if (element && element.hidden !== hidden) element.hidden = hidden;
}

function toggleClass(element, name, on) {
  element?.classList?.toggle?.(name, Boolean(on));
}

function clamp(value, low, high) {
  return Math.min(Math.max(value, low), Math.max(low, high));
}

function initialOf(name) {
  const letter = String(name || "").replace(/^@/, "").trim().charAt(0);
  return letter ? letter.toUpperCase() : "?";
}

export function createChatCalls({
  iceServers = [],
  selfId = "",
  accountName = () => "",
  sendSignal = () => Promise.resolve(false),
  partnerFor = () => "",
  onMissed = () => {},
  onActiveChange = () => {},
  requestStart = null,
  getUserMediaImpl = (constraints) =>
    globalThis.navigator.mediaDevices.getUserMedia(constraints),
  RTCPeerConnectionImpl = globalThis.RTCPeerConnection,
  documentRef = globalThis.document,
  windowRef = globalThis,
  storage = globalThis.localStorage,
  avatarImage = () => "",
  facesSupported = () => callFacesSupported(documentRef),
  backgroundsSupported = () => callBackgroundsSupported(documentRef),
  createFaceStageImpl = createFaceStage,
  createVoicePreviewImpl = createVoicePreview,
  voicesSupported = () => callVoicesSupported(globalThis),
  createVoiceStageImpl = createVoiceStage,
  voiceModuleUrl = new URL("./chat-call-voice-worklet.js", import.meta.url).href,
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
  tones = createCallTones({ setIntervalImpl, clearIntervalImpl }),
  NotificationImpl = globalThis.Notification,
  navigatorRef = globalThis.navigator,
  recordTestClipImpl = recordTestClip,
  readRouteImpl = readCallRoute,
  preferAudioCodecsImpl = preferCallAudioCodecs,
  createQualityMeterImpl = createCallQualityMeter,
  URLImpl = globalThis.URL,
  now = () => Date.now(),
} = {}) {
  const panel = documentRef.querySelector("#chat-call-panel");
  const peerLabel = documentRef.querySelector("#chat-call-peer");
  const stateLabel = documentRef.querySelector("#chat-call-state");
  const videosBox = documentRef.querySelector("#chat-call-videos");
  const remoteVideo = documentRef.querySelector("#chat-call-remote");
  const localVideo = documentRef.querySelector("#chat-call-local");
  const placeholder = documentRef.querySelector("#chat-call-placeholder");
  const placeholderAvatar = documentRef.querySelector("#chat-call-avatar");
  const placeholderText = documentRef.querySelector("#chat-call-placeholder-text");
  const soundBtn = documentRef.querySelector("#chat-call-sound");
  const noticeLabel = documentRef.querySelector("#chat-call-notice");
  const backBtn = documentRef.querySelector("#chat-call-minimize");
  const expandBtn = documentRef.querySelector("#chat-call-expand");
  const muteBtn = documentRef.querySelector("#chat-call-mute");
  const cameraBtn = documentRef.querySelector("#chat-call-camera");
  const hangupBtn = documentRef.querySelector("#chat-call-hangup");
  const incomingEl = documentRef.querySelector("#chat-call-incoming");
  const incomingLabel = documentRef.querySelector("#chat-call-incoming-label");
  const acceptBtn = documentRef.querySelector("#chat-call-accept");
  const declineBtn = documentRef.querySelector("#chat-call-decline");
  const faceRow = documentRef.querySelector("#chat-call-face-row");
  const faceSelect = documentRef.querySelector("#chat-call-face");
  const faceFile = documentRef.querySelector("#chat-call-face-file");
  const voiceRow = documentRef.querySelector("#chat-call-voice-row");
  const voiceSelect = documentRef.querySelector("#chat-call-voice");
  const voiceState = documentRef.querySelector("#chat-call-voice-state");
  const backgroundRow = documentRef.querySelector("#chat-call-background-row");
  const backgroundOptions = documentRef.querySelector("#chat-call-background-options");
  const backgroundFile = documentRef.querySelector("#chat-call-background-file");
  const previewSlot = documentRef.querySelector("#chat-call-preview-slot");
  const startBtn = documentRef.querySelector("#chat-call-start");
  const routeLabel = documentRef.querySelector("#chat-call-route");
  const testRow = documentRef.querySelector("#chat-call-test-row");
  const testBtn = documentRef.querySelector("#chat-call-test");
  const testPlayer = documentRef.querySelector("#chat-call-test-player");
  const incomingKind = documentRef.querySelector("#chat-call-incoming-kind");
  const qualityHost = panel?.querySelector?.(".chat-call-head") || null;

  let call = null;
  let invite = null;
  let ringAlert = null;
  let quality = null;
  let ended = null;

  function timer(name, fn, ms, every = false) {
    const owner = call;
    if (!owner) return;
    clearTimer(name, owner);
    const entry = { every, id: null };
    const run = () => {
      if (call !== owner || owner.timers[name] !== entry) return;
      if (!every) delete owner.timers[name];
      fn();
    };
    entry.id = every ? setIntervalImpl(run, ms) : setTimeoutImpl(run, ms);
    owner.timers[name] = entry;
  }

  function clearTimer(name, owner = call) {
    const entry = owner?.timers?.[name];
    if (!entry) return;
    delete owner.timers[name];
    if (entry.every) clearIntervalImpl(entry.id);
    else clearTimeoutImpl(entry.id);
  }

  function clearCallTimers(owner = call) {
    for (const name of Object.keys(owner?.timers || {})) clearTimer(name, owner);
  }

  function callState(fields) {
    return {
      channelKey: "",
      callId: "",
      partner: "",
      media: "audio",
      role: "",
      phase: "",
      phaseText: "",
      peerId: "",
      pc: null,
      localStream: null,
      remoteStream: null,
      pendingIce: [],
      muted: false,
      cameraOff: false,
      stage: null,
      faceId: "",
      faceImage: "",
      stageFaceImage: "",
      backgroundId: "",
      backgroundImage: "",
      voiceChoice: savedVoiceChoice(storage),
      voiceStage: null,
      voiceStarting: null,
      voicePreview: null,
      voicePreviewUnmount: null,
      timers: {},
      route: null,
      testRecording: null,
      testUrl: "",
      testLeftMs: 0,
      offer: null,
      answerSdp: "",
      answered: null,
      offerQueue: null,
      negotiating: 0,
      restarting: false,
      everLive: false,
      drop: null,
      health: null,
      healthBusy: false,
      statsCache: null,
      remoteVideoLive: false,
      mini: false,
      notice: "",
      watchedMic: null,
      ...fields,
    };
  }

  function sharedStats(pc) {
    if (!pc || typeof pc.getStats !== "function") return pc;
    return {
      getStats() {
        const current = call;
        const at = now();
        const cached = current?.statsCache;
        if (cached && cached.pc === pc && at - cached.at < STATS_SHARE_MS) return cached.promise;
        const promise = Promise.resolve().then(() => pc.getStats());
        if (current) current.statsCache = { pc, at, promise };
        return promise;
      },
    };
  }

  function stopQuality() {
    if (!quality) return;
    quality.widget?.destroy();
    quality.meter.stop();
    quality = null;
  }

  function startQuality(pc = null) {
    if (!call || !qualityHost || !createQualityMeterImpl) return;
    if (quality?.call === call && (!pc || quality.pc === pc)) return;
    stopQuality();
    const current = call;
    const source = sharedStats(pc);
    const meter = createQualityMeterImpl({
      readStats: pc ? rtcStatsReader(() => (call === current && current.pc === pc ? [source] : [])) : null,
      tracks: () => ({
        mic: call === current ? outgoingTrack("audio") : null,
        peers: call === current ? current.remoteStream?.getAudioTracks?.() || [] : [],
      }),
      face: () => (call === current && current.faceId ? current.stage?.stats || null : null),
    });
    const widget = mountCallQualityWidget(qualityHost, meter, {
      documentRef,
      storage,
      video: () => call === current && current.media === "video",
      sending: () => call === current && !current.cameraOff,
      visible: () => call === current && !current.mini && !documentRef.hidden,
    });
    quality = { call: current, pc, meter, widget };
  }

  function newCallId() {
    return (globalThis.crypto?.randomUUID && globalThis.crypto.randomUUID()) ||
      String(Math.random()).slice(2) + Date.now();
  }

  function ownAccountFrame(plain) {
    const own = String(accountName() || "").trim().toLowerCase();
    return Boolean(own) &&
      String(plain?.sender || "").trim().toLowerCase() === own;
  }

  function setPhase(phase, text) {
    if (!call) return;
    call.phase = phase;
    call.phaseText = String(text || "");
    renderState();
    renderControls();
    syncTones();
  }

  function renderState() {
    if (!call || !stateLabel) return;
    setText(stateLabel, call.notice || (call.drop ? RECONNECTING_TEXT : call.phaseText));
  }

  function notify(text) {
    if (!call) return;
    call.notice = String(text || "");
    renderState();
    timer("notice", () => {
      call.notice = "";
      renderState();
    }, NOTICE_MS);
  }

  function syncTones() {
    if (invite && ringAlert?.callId === invite.callId) tones.ring();
    else if (call?.phase === "ringing-out") tones.ringback();
    else tones.stop();
  }

  function renderRoute() {
    if (!routeLabel) return;
    const text = call?.phase === "live" ? callRouteLabel(call.route) : "";
    setText(routeLabel, text);
    setHidden(routeLabel, !text);
    if (call?.route && routeLabel.getAttribute?.("data-route") !== call.route.route) {
      routeLabel.setAttribute?.("data-route", call.route.route);
    }
  }

  function watchRoute(pc) {
    if (!call || call.timers.route) return;
    const current = call;
    const source = sharedStats(pc);
    const check = () => {
      if (call !== current || current.pc !== pc) return;
      Promise.resolve()
        .then(() => readRouteImpl(source))
        .then((found) => {
          if (!found || call !== current || current.pc !== pc) return;
          const changed = found.route !== current.route?.route;
          current.route = found;
          if (changed) renderRoute();
        })
        .catch(() => {});
    };
    timer("route", check, CALL_ROUTE_CHECK_MS, true);
    check();
  }

  function renderControls() {
    if (!call || !panel) return;
    const ongoing = call.phase !== "preview";
    toggleClass(panel, "is-call", ongoing);
    toggleClass(panel, "is-mini", call.mini);
    setHidden(backBtn, !ongoing || call.mini);
    setHidden(expandBtn, !call.mini);
  }

  function renderRemote() {
    if (!call) return;
    const ongoing = call.phase !== "preview";
    const video = call.media === "video";
    const waiting = ongoing && (!video || !call.remoteVideoLive);
    setHidden(videosBox, ongoing ? false : !video);
    setHidden(placeholder, !waiting);
    toggleClass(panel, "is-waiting", waiting && video);
    if (!waiting) return;
    setText(placeholderAvatar, initialOf(call.partner));
    setText(placeholderText, video
      ? call.drop ? RECONNECTING_TEXT : `Waiting for @${call.partner}'s video…`
      : "@" + call.partner);
  }

  function setRemoteVideoLive(on) {
    if (!call || call.remoteVideoLive === on) return;
    call.remoteVideoLive = on;
    renderRemote();
  }

  function renderPanel() {
    if (!panel || !call) return;
    if (ended) dismissEnded();
    setText(peerLabel, "@" + call.partner);
    setHidden(remoteVideo, call.media !== "video");
    setHidden(cameraBtn, call.media !== "video");
    setHidden(faceRow, call.media !== "video" || !facesSupported());
    if (faceSelect) {
      const avatarOption = [...(faceSelect.options || [])].find((option) => option.value === AVATAR_FACE_ID);
      if (avatarOption) avatarOption.hidden = !faceImageFor(AVATAR_FACE_ID, { avatarImage });
      faceSelect.value = call.stage ? call.faceId || call.stage.face() || "" : "";
    }
    renderBackgrounds();
    const previewing = call.phase === "preview";
    toggleClass(panel, "is-preview", previewing);
    if (startBtn) {
      setHidden(startBtn, !previewing);
      setText(startBtn, call.media === "video" ? "Start video call" : "Start voice call");
    }
    setHidden(testRow, !previewing);
    renderTest();
    renderRoute();
    setText(hangupBtn, previewing ? "Close" : "Hang up");
    setHidden(previewSlot, !previewing);
    setText(muteBtn, call.muted ? "Unmute" : "Mute");
    muteBtn?.setAttribute?.("aria-pressed", call.muted ? "true" : "false");
    if (cameraBtn) setText(cameraBtn, call.cameraOff ? "Camera on" : "Camera off");
    toggleClass(panel, "is-video", call.media === "video");
    renderControls();
    renderRemote();
    renderVoice();
    if (voiceRow && call.phase === "preview") voiceRow.hidden = true;
    panel.hidden = false;
    startQuality();
  }

  function renderVoice() {
    if (!call) return;
    const supported = voicesSupported();
    setHidden(voiceRow, !supported);
    if (voiceSelect) voiceSelect.value = call.voiceChoice;
    setText(voiceState, "Voice: " + callVoiceLabel(call.voiceStage?.voice() || ""));
  }

  function voiceFace() {
    if (!call) return "";
    return call.media === "video" ? call.faceId || call.stage?.face() || "" : savedFace();
  }

  function audioSender() {
    return call?.pc?.getTransceivers?.().find(
      (transceiver) => transceiver.receiver?.track?.kind === "audio",
    )?.sender || null;
  }

  function sendAudio() {
    const track = outgoingTrack("audio");
    const sender = audioSender();
    if (sender && track && sender.track !== track) {
      Promise.resolve(sender.replaceTrack(track)).catch(() => {});
    }
  }

  function applyVoice() {
    if (!call) return Promise.resolve();
    const current = call;
    if (current.phase === "preview") {
      current.voicePreview?.setFace(current.faceId);
      return Promise.resolve();
    }
    const id = resolveCallVoice(current.voiceChoice, voiceFace());
    if (current.voiceStage) {
      current.voiceStage.setVoice(id);
      renderVoice();
      return Promise.resolve();
    }
    if (current.voiceStarting) return current.voiceStarting;
    if (!id || !voicesSupported()) {
      renderVoice();
      return Promise.resolve();
    }
    const mic = liveTrack("audio");
    if (!mic) return Promise.resolve();
    current.voiceStarting = Promise.resolve()
      .then(() => createVoiceStageImpl({
        sourceTrack: mic,
        voiceId: id,
        userPitchHz: savedCallerPitch(storage),
        moduleUrl: voiceModuleUrl,
        onStats: (stats) => rememberCallerPitch(stats?.userPitchHz, storage),
      }))
      .then((stage) => {
        if (call !== current || liveTrack("audio") !== mic) {
          stage.stop();
          return;
        }
        current.voiceStage = stage;
        stage.setVoice(resolveCallVoice(current.voiceChoice, voiceFace()));
        sendAudio();
      })
      .catch(() => {})
      .finally(() => {
        if (call === current) {
          current.voiceStarting = null;
          renderVoice();
        }
      });
    return current.voiceStarting;
  }

  function voiceReady() {
    const pending = call?.voiceStarting;
    if (!pending) return Promise.resolve();
    return Promise.race([
      pending,
      new Promise((resolve) => setTimeoutImpl(resolve, VOICE_READY_MS)),
    ]);
  }

  function hideIncoming() {
    if (invite?.staleTimer) clearTimeoutImpl(invite.staleTimer);
    invite = null;
    setHidden(incomingEl, true);
    stopRingAlert();
    syncTones();
  }

  function expireInvite(pending) {
    if (invite !== pending) return;
    if (pending.accepting) {
      pending.staleTimer = setTimeoutImpl(() => expireInvite(pending), INVITE_STALE_MS);
      return;
    }
    hideIncoming();
  }

  function startRingAlert(pending) {
    if (ringAlert?.callId === pending.callId) return;
    stopRingAlert();
    const title = `Incoming ${pending.media === "video" ? "video" : "voice"} call`;
    const body = `@${pending.partner} is calling you`;
    ringAlert = { callId: pending.callId, notification: null, vibrateTimer: null, titleTimer: null, title: documentRef.title };
    const vibrate = () => {
      try {
        navigatorRef?.vibrate?.(VIBRATE_PATTERN);
      } catch (_) {}
    };
    vibrate();
    ringAlert.vibrateTimer = setIntervalImpl(vibrate, VIBRATE_EVERY_MS);
    if (typeof ringAlert.title === "string") {
      let flip = false;
      ringAlert.titleTimer = setIntervalImpl(() => {
        flip = !flip;
        documentRef.title = flip ? `📞 ${body}` : ringAlert?.title || "";
      }, TITLE_FLASH_MS);
    }
    const away = Boolean(documentRef.hidden) || (typeof documentRef.hasFocus === "function" && !documentRef.hasFocus());
    if (away && NotificationImpl?.permission === "granted") {
      try {
        const notification = new NotificationImpl(title, {
          body,
          tag: `forkmesh-call-${pending.callId}`,
          requireInteraction: true,
          renotify: true,
          silent: false,
        });
        notification.onclick = () => {
          try {
            globalThis.focus?.();
          } catch (_) {}
          notification.close?.();
        };
        ringAlert.notification = notification;
      } catch (_) {}
    }
  }

  function stopRingAlert() {
    const current = ringAlert;
    ringAlert = null;
    if (!current) return;
    if (current.vibrateTimer) clearIntervalImpl(current.vibrateTimer);
    if (current.titleTimer) clearIntervalImpl(current.titleTimer);
    if (typeof current.title === "string") documentRef.title = current.title;
    try {
      navigatorRef?.vibrate?.(0);
    } catch (_) {}
    try {
      current.notification?.close?.();
    } catch (_) {}
  }

  function signal(type, extra) {
    if (!call) return Promise.resolve(false);
    return Promise.resolve(
      sendSignal(call.channelKey, type, { call: call.callId, ...extra }),
    ).catch(() => false);
  }

  function knownFace(id) {
    if (photoFaceId(id)) return faceImageFor(id, { storage, avatarImage }) ? id : "";
    return callFaceById(id)?.id || "";
  }

  function savedFace() {
    try {
      return knownFace(storage?.getItem(CALL_FACE_STORAGE_KEY) || "");
    } catch (_) {
      return "";
    }
  }

  function watchCustomFace() {
    const started = Date.now();
    const check = () => {
      if (!call || !photoFaceId(call.faceId) || !call.stage?.faceState) return;
      const state = call.stage.faceState();
      if (state === "failed") {
        if (stateLabel) {
          stateLabel.textContent = call.faceId === AVATAR_FACE_ID
            ? "No face found in your avatar. Pick another face or use a clear, front-facing profile photo."
            : "No face found in that photo. Try a clear, front-facing photo.";
        }
        rememberFace("");
        useFace("");
        renderPanel();
        return;
      }
      if (state === "loading" && Date.now() - started < 30000) setTimeoutImpl(check, 250);
    };
    setTimeoutImpl(check, 250);
  }

  function rememberFace(id) {
    try {
      if (id) storage?.setItem(CALL_FACE_STORAGE_KEY, id);
      else storage?.removeItem(CALL_FACE_STORAGE_KEY);
    } catch (_) {}
  }

  function liveTrack(kind) {
    if (!call?.localStream) return null;
    const tracks = call.localStream.getTracks().filter((value) => value.kind === kind);
    return tracks.find((value) => value.readyState !== "ended") || tracks[0] || null;
  }

  function outgoingTrack(kind) {
    if (!call) return null;
    if (kind === "video" && call.stage?.track) return call.stage.track;
    if (kind === "audio" && call.voiceStage?.track) return call.voiceStage.track;
    return liveTrack(kind);
  }

  function videoSender() {
    return call?.pc?.getTransceivers?.().find(
      (transceiver) => transceiver.receiver?.track?.kind === "video",
    )?.sender || null;
  }

  function useFace(id) {
    if (!call || call.media !== "video") return;
    const face = knownFace(id);
    if (face && !facesSupported()) return;
    call.faceId = face;
    call.faceImage = faceImageFor(face, { storage, avatarImage });
    refreshStage();
    if (photoFaceId(face)) watchCustomFace();
  }

  function useBackground(id, image = "") {
    if (!call || call.media !== "video") return;
    const background = callBackgroundById(id);
    if (background?.id && !backgroundsSupported()) return;
    call.backgroundId = background?.id || "";
    call.backgroundImage = background?.custom ? image : "";
    refreshStage();
  }

  function refreshStage() {
    if (!call || call.media !== "video") return;
    const wanted = Boolean(call.faceId || call.backgroundId);
    if (wanted && call.stage) {
      if (call.stage.face() !== stageFaceFor(call.faceId) || (photoFaceId(call.faceId) && call.stageFaceImage !== call.faceImage)) {
        call.stage.setFace(stageFaceFor(call.faceId), call.faceImage || "");
        call.stageFaceImage = call.faceImage || "";
      }
      if ((call.stage.background?.() || "") !== call.backgroundId || call.backgroundId === "custom") {
        call.stage.setBackground?.(call.backgroundId, call.backgroundImage);
      }
    } else if (wanted) {
      try {
        call.stage = createFaceStageImpl({
          cameraStream: call.localStream,
          faceId: stageFaceFor(call.faceId),
          faceImage: call.faceImage || "",
          backgroundId: call.backgroundId,
          backgroundImage: call.backgroundImage,
          documentRef,
        });
      } catch (_) {
        call.stage = null;
      }
    } else if (call.stage) {
      call.stage.stop();
      call.stage = null;
    }
    const track = outgoingTrack("video");
    if (track) track.enabled = !call.cameraOff;
    const sender = videoSender();
    if (sender && track && sender.track !== track) {
      Promise.resolve(sender.replaceTrack(track)).catch(() => {});
    }
    attachLocalPreview();
    renderPanel();
  }

  function renderBackgrounds() {
    if (!backgroundRow || !call) return;
    setHidden(backgroundRow, call.media !== "video" || !backgroundsSupported());
    if (backgroundRow.hidden) return;
    for (const button of backgroundOptions?.querySelectorAll?.("[data-background]") || []) {
      const on = button.getAttribute("data-background") === (call.backgroundId || "");
      button.setAttribute("aria-checked", on ? "true" : "false");
      button.classList.toggle("is-selected", on);
    }
  }

  function chooseBackground(id, image = "") {
    const background = callBackgroundById(id);
    if (!background) return;
    if (background.custom && !image) {
      const saved = savedCallBackground(storage);
      if (saved.id === background.id) image = saved.image;
      else {
        backgroundFile?.click?.();
        return;
      }
    }
    rememberCallBackground(background.id, image, storage);
    useBackground(background.id, image);
  }

  function buildBackgroundOptions() {
    if (!backgroundOptions || backgroundOptions.querySelector?.("[data-background]")) return;
    for (const background of CALL_BACKGROUNDS) {
      const button = documentRef.createElement("button");
      button.type = "button";
      button.className = "chat-call-background-option";
      button.setAttribute("role", "radio");
      button.setAttribute("data-background", background.id);
      button.setAttribute("aria-checked", "false");
      button.title = background.label;
      const swatch = documentRef.createElement("span");
      swatch.className = `chat-call-background-swatch is-${background.mode}${background.custom ? " is-custom" : ""}`;
      const thumb = callBackgroundThumbUrl(background);
      if (thumb) swatch.style.backgroundImage = `url("${thumb}")`;
      if (background.custom) {
        const saved = savedCallBackground(storage);
        if (saved.image) swatch.style.backgroundImage = `url("${saved.image}")`;
      }
      const name = documentRef.createElement("span");
      name.className = "chat-call-background-name";
      name.textContent = background.label;
      button.append(swatch, name);
      button.addEventListener("click", () => chooseBackground(background.id));
      backgroundOptions.append(button);
    }
  }

  function showEnded(partner, title, detail = "") {
    if (!panel || call) return;
    dismissEnded();
    ended = { timer: setTimeoutImpl(dismissEnded, ENDED_MS) };
    setText(peerLabel, "@" + partner);
    setText(stateLabel, title);
    setText(noticeLabel, detail);
    setHidden(noticeLabel, !detail);
    setText(hangupBtn, "Close");
    for (const name of ["is-preview", "is-video", "is-call", "is-mini", "is-waiting"]) toggleClass(panel, name, false);
    toggleClass(panel, "is-ended", true);
    panel.hidden = false;
  }

  function dismissEnded() {
    const current = ended;
    if (!current) return;
    ended = null;
    clearTimeoutImpl(current.timer);
    toggleClass(panel, "is-ended", false);
    setHidden(noticeLabel, true);
    if (!call && panel) panel.hidden = true;
  }

  function teardown() {
    if (!call) return;
    const current = call;
    stopQuality();
    clearCallTimers(current);
    clearTest();
    stopVoicePreview();
    try {
      current.stage?.stop();
    } catch (_) {}
    try {
      current.voiceStage?.stop();
    } catch (_) {}
    try {
      current.pc?.close();
    } catch (_) {}
    for (const track of current.localStream?.getTracks() || []) {
      try {
        track.stop();
      } catch (_) {}
    }
    current.watchedMic = null;
    call = null;
    syncTones();
    renderRoute();
    if (localVideo) localVideo.srcObject = null;
    if (remoteVideo) {
      remoteVideo.srcObject = null;
      remoteVideo.muted = false;
    }
    setHidden(soundBtn, true);
    setHidden(placeholder, true);
    setHidden(backBtn, true);
    setHidden(expandBtn, true);
    if (panel) {
      for (const name of ["is-call", "is-mini", "is-waiting", "is-dragging"]) toggleClass(panel, name, false);
      if (panel.style) Object.assign(panel.style, { left: "", top: "" });
      panel.hidden = true;
    }
    onActiveChange();
  }

  function endCall({ notifyPeer = false, reason = "" } = {}) {
    if (!call) return;
    if (notifyPeer && call.phase !== "preview") void signal("call-bye", reason ? { reason } : {});
    teardown();
  }

  function failCall(title, detail) {
    if (!call) return;
    const partner = call.partner;
    endCall({ notifyPeer: true });
    showEnded(partner, title, detail);
  }

  function hangUp() {
    if (!call) {
      dismissEnded();
      return;
    }
    if (call?.role === "caller" && call.phase === "ringing-out") {
      const missed = call;
      endCall({ notifyPeer: true, reason: "missed" });
      onMissed(missed.channelKey, missed.media);
      return;
    }
    endCall({ notifyPeer: true });
  }

  async function localMedia(media) {
    return getUserMediaImpl(callMediaConstraints(media));
  }

  function attachLocalPreview() {
    if (!call || !localVideo) return;
    const track = call.media === "video" ? outgoingTrack("video") : null;
    const shown = localVideo.srcObject?.getVideoTracks?.()[0] || null;
    if (track !== shown) localVideo.srcObject = track ? new MediaStream([track]) : null;
    setHidden(localVideo, call.media !== "video");
  }

  function watchMic() {
    const current = call;
    const mic = liveTrack("audio");
    if (!current || !mic || current.watchedMic === mic || typeof mic.addEventListener !== "function") return;
    current.watchedMic = mic;
    mic.addEventListener("ended", () => {
      if (call === current && current.watchedMic === mic) void recoverMic(mic);
    });
  }

  async function recoverMic(lost) {
    const current = call;
    if (!current || current.phase === "preview") return false;
    current.watchedMic = null;
    let stream = null;
    try {
      stream = await getUserMediaImpl({ audio: { ...CALL_AUDIO_CONSTRAINTS }, video: false });
    } catch (_) {
      if (call === current) notify("Your microphone stopped. Check that it is connected and allowed for this site.");
      return false;
    }
    const fresh = stream?.getAudioTracks?.()[0] || null;
    if (call !== current || !fresh) {
      for (const track of stream?.getTracks?.() || []) track.stop();
      return false;
    }
    current.localStream.removeTrack?.(lost);
    current.localStream.addTrack(fresh);
    fresh.enabled = !current.muted;
    if (current.voiceStage || current.voiceStarting) {
      await current.voiceStarting;
      if (call !== current) return false;
      const old = current.voiceStage;
      current.voiceStage = null;
      try {
        old?.stop();
      } catch (_) {}
      sendAudio();
      await applyVoice();
    } else {
      sendAudio();
    }
    if (call !== current) return false;
    watchMic();
    notify("Microphone reconnected");
    return true;
  }

  function attachRemote() {
    if (!call || !remoteVideo || remoteVideo.srcObject === call.remoteStream) return;
    remoteVideo.srcObject = call.remoteStream;
    playRemote();
  }

  function playRemote() {
    const current = call;
    if (!remoteVideo || !current) return;
    let played;
    try {
      played = remoteVideo.play?.();
    } catch (error) {
      played = Promise.reject(error);
    }
    Promise.resolve(played).then(() => {
      if (call === current && !remoteVideo.muted) setHidden(soundBtn, true);
    }, (error) => {
      if (call !== current || error?.name !== "NotAllowedError") return;
      remoteVideo.muted = true;
      setHidden(soundBtn, false);
      Promise.resolve(remoteVideo.play?.()).catch(() => {});
    });
  }

  function unmuteRemote() {
    if (!call || !remoteVideo) return;
    remoteVideo.muted = false;
    playRemote();
  }

  function watchRemoteTrack(track) {
    if (track?.kind !== "video" || typeof track.addEventListener !== "function") return;
    const current = call;
    track.addEventListener("mute", () => {
      if (call === current) setRemoteVideoLive(false);
    });
    track.addEventListener("unmute", () => {
      if (call === current) setRemoteVideoLive(true);
    });
    if (track.muted === false) setRemoteVideoLive(true);
  }

  function startConnectTimer() {
    timer("connect", () => {
      if (!call || call.everLive) return;
      failCall("Could not connect", "The call could not connect. Try again.");
    }, CONNECT_TIMEOUT_MS);
  }

  function sendAccept() {
    if (!call) return;
    const to = call.peerId;
    let resends = 0;
    void signal("call-accept", { to });
    timer("accept", () => {
      if (resends >= ACCEPT_RESENDS) {
        clearTimer("accept");
        return;
      }
      resends += 1;
      void signal("call-accept", { to });
    }, ACCEPT_RESEND_MS, true);
  }

  function sendOffer(sdp) {
    if (!call) return;
    const offer = { sdp, resends: 0 };
    call.offer = offer;
    void signal("call-offer", { to: call.peerId, sdp });
    timer("offer", () => {
      if (call.offer !== offer || offer.resends >= OFFER_RESENDS) {
        clearTimer("offer");
        return;
      }
      offer.resends += 1;
      void signal("call-offer", { to: call.peerId, sdp });
    }, OFFER_RESEND_MS, true);
  }

  function connectionDropped(pc, reason) {
    if (!call || call.pc !== pc) return;
    if (!call.drop) {
      call.drop = { reason };
      timer("giveUp", () => failCall("Call lost", "The connection dropped and did not come back. Try calling again."), DROP_GIVE_UP_MS);
      renderState();
      renderRemote();
    } else if (reason !== "media") {
      call.drop.reason = reason;
    }
    if (call.role !== "caller" || call.timers.restart) return;
    if (reason === "disconnected") {
      if (!call.timers.dropRestart) timer("dropRestart", () => void restartIce(pc), DROP_RESTART_MS);
    } else {
      void restartIce(pc);
    }
  }

  function connectionRecovered() {
    if (!call?.drop) return;
    call.drop = null;
    for (const name of ["giveUp", "dropRestart", "restart"]) clearTimer(name);
    renderState();
    renderRemote();
  }

  async function restartIce(pc) {
    const current = call;
    if (!current || current.pc !== pc || current.role !== "caller" || !current.drop) return false;
    clearTimer("dropRestart");
    if (!current.timers.restart) timer("restart", () => void restartIce(pc), RESTART_EVERY_MS, true);
    if (current.restarting) return false;
    current.restarting = true;
    try {
      if (typeof pc.restartIce === "function") {
        pc.restartIce();
        await pc.setLocalDescription();
      } else {
        await pc.setLocalDescription(await pc.createOffer({ iceRestart: true }));
      }
      if (call !== current || current.pc !== pc) return false;
      sendOffer(tuneCallSdp(pc.localDescription));
      return true;
    } catch (_) {
      return false;
    } finally {
      current.restarting = false;
    }
  }

  function watchHealth(pc) {
    if (!call || call.timers.health || typeof pc?.getStats !== "function") return;
    call.health = { packets: null, silent: 0, frames: null, still: 0 };
    timer("health", () => void checkHealth(pc), HEALTH_MS, true);
  }

  async function checkHealth(pc) {
    const current = call;
    if (!current || current.pc !== pc || current.healthBusy || !current.health) return;
    current.healthBusy = true;
    let report = null;
    try {
      report = await sharedStats(pc).getStats();
    } catch (_) {
    } finally {
      current.healthBusy = false;
    }
    if (call !== current || current.pc !== pc || !report?.forEach) return;
    let packets = null;
    let frames = null;
    report.forEach((entry) => {
      if (entry?.type !== "inbound-rtp") return;
      const kind = entry.kind || entry.mediaType;
      if (kind === "audio" && Number.isFinite(entry.packetsReceived)) packets = (packets || 0) + entry.packetsReceived;
      if (kind === "video" && Number.isFinite(entry.framesDecoded)) frames = (frames || 0) + entry.framesDecoded;
    });
    const health = current.health;
    if (packets !== null) {
      const grew = health.packets === null || packets > health.packets;
      health.packets = packets;
      health.silent = grew ? 0 : health.silent + 1;
      if (grew && current.drop?.reason === "media") connectionRecovered();
      if (health.silent >= AUDIO_SILENT_TICKS && current.phase === "live" && pc.connectionState === "connected") {
        connectionDropped(pc, "media");
      }
    }
    if (current.media === "video" && frames !== null) {
      const flowing = health.frames !== null && frames - health.frames >= VIDEO_MIN_FRAMES;
      health.frames = frames;
      health.still = flowing ? 0 : health.still + 1;
      if (flowing) setRemoteVideoLive(true);
      else if (health.still >= VIDEO_STALL_TICKS) setRemoteVideoLive(false);
    }
  }

  function createPeer() {
    const pc = new RTCPeerConnectionImpl({ iceServers });
    const current = call;
    current.pc = pc;
    current.remoteStream = new MediaStream();
    pc.onicecandidate = (event) => {
      if (event.candidate && call === current) {
        void signal("call-ice", { to: current.peerId, ice: event.candidate });
      }
    };
    pc.ontrack = (event) => {
      if (call !== current || current.pc !== pc) return;
      if (!current.remoteStream.getTracks().includes(event.track)) {
        current.remoteStream.addTrack(event.track);
        watchRemoteTrack(event.track);
      }
      attachRemote();
    };
    pc.onconnectionstatechange = () => {
      if (call !== current || current.pc !== pc) return;
      const state = pc.connectionState;
      if (state === "connected") {
        current.everLive = true;
        clearTimer("connect");
        if (current.health) current.health.silent = 0;
        if (current.drop?.reason !== "media") connectionRecovered();
        setPhase("live", LIVE_TEXT);
        renderRoute();
        watchRoute(pc);
        startQuality(pc);
        watchHealth(pc);
        void tuneCallSenders(pc);
      } else if (state === "failed" || state === "disconnected") {
        connectionDropped(pc, state);
      } else if (state === "closed") {
        endCall({ notifyPeer: false });
      }
    };
    return pc;
  }

  async function flushPendingIce() {
    if (!call?.pc?.remoteDescription) return;
    for (const candidate of call.pendingIce.splice(0)) {
      try {
        await call.pc.addIceCandidate(candidate);
      } catch (_) {}
    }
  }

  function useSaved() {
    const saved = savedCallBackground(storage);
    call.faceId = facesSupported() ? savedFace() : "";
    call.faceImage = faceImageFor(call.faceId, { storage, avatarImage });
    call.backgroundId = backgroundsSupported() ? saved.id : "";
    call.backgroundImage = saved.image;
    refreshStage();
  }

  function takePreview(media) {
    if (!call || call.phase !== "preview") return null;
    const kept = call;
    stopVoicePreview({ keepStream: true });
    if (kept.media !== (media === "video" ? "video" : "audio")) {
      teardown();
      return null;
    }
    clearCallTimers();
    clearTest();
    call = null;
    return kept;
  }

  function carryPreview(previewed) {
    if (call.media === "video" && previewed) {
      call.faceId = previewed.faceId;
      call.faceImage = previewed.faceImage || "";
      call.stageFaceImage = previewed.stageFaceImage || "";
      call.backgroundId = previewed.backgroundId;
      call.backgroundImage = previewed.backgroundImage;
      refreshStage();
    } else if (call.media === "video") {
      useSaved();
    }
  }

  function testTracks() {
    if (!call) return [];
    const audio = call.voicePreview?.track?.() || call.localStream.getAudioTracks()[0] || null;
    return [call.media === "video" ? outgoingTrack("video") : null, audio].filter(Boolean);
  }

  function clearTest() {
    const current = call;
    if (!current) return;
    const recording = current.testRecording;
    current.testRecording = null;
    if (recording) Promise.resolve(recording.stop()).catch(() => {});
    if (current.testUrl) {
      try {
        URLImpl?.revokeObjectURL?.(current.testUrl);
      } catch (_) {}
    }
    current.testUrl = "";
    current.testLeftMs = 0;
    if (testPlayer && !testPlayer.hidden) {
      try {
        testPlayer.pause?.();
      } catch (_) {}
      testPlayer.removeAttribute?.("src");
      testPlayer.hidden = true;
    }
  }

  function renderTest() {
    if (!testBtn || !call || call.phase !== "preview") return;
    if (call.testRecording) {
      setText(testBtn, `Stop test (${Math.ceil(call.testLeftMs / 1000)}s left)`);
      testBtn.setAttribute("aria-pressed", "true");
    } else {
      setText(testBtn, call.testUrl ? "Record again" : "Record a 10-second test clip");
      testBtn.setAttribute("aria-pressed", "false");
    }
  }

  async function recordTest() {
    if (!call || call.phase !== "preview") return false;
    const current = call;
    if (current.testRecording) {
      await current.testRecording.stop();
      return true;
    }
    clearTest();
    let recording;
    try {
      recording = recordTestClipImpl(testTracks(), {
        maxMs: CALL_TEST_CLIP_MS,
        onTick: (ms, max) => {
          if (call !== current) return;
          current.testLeftMs = Math.max(0, max - ms);
          renderTest();
        },
      });
    } catch (_) {
      if (stateLabel) stateLabel.textContent = "Recording a test clip is not available in this browser.";
      return false;
    }
    current.testRecording = recording;
    current.testLeftMs = CALL_TEST_CLIP_MS;
    if (stateLabel) stateLabel.textContent = "Recording a test clip · speak and look at the camera";
    renderTest();
    try {
      const clip = await recording.done;
      if (call !== current || current.testRecording !== recording) return false;
      current.testRecording = null;
      if (!clip?.blob?.size) throw new Error("empty_clip");
      current.testUrl = URLImpl.createObjectURL(clip.blob);
      if (testPlayer) {
        testPlayer.src = current.testUrl;
        testPlayer.classList.toggle("is-audio", !clip.video);
        testPlayer.hidden = false;
        Promise.resolve(testPlayer.play?.()).catch(() => {});
      }
      if (stateLabel) stateLabel.textContent = "Playing your test clip · this is how you will look and sound";
      return true;
    } catch (_) {
      if (call === current) {
        current.testRecording = null;
        if (stateLabel) stateLabel.textContent = "The test clip could not be recorded. Try again.";
      }
      return false;
    } finally {
      if (call === current) renderTest();
    }
  }

  function stopVoicePreview(options) {
    try {
      call?.voicePreview?.stop(options);
    } catch (_) {}
    try {
      call?.voicePreviewUnmount?.();
    } catch (_) {}
    if (call) {
      call.voicePreview = null;
      call.voicePreviewUnmount = null;
    }
  }

  async function preview(channelKey, partner, media = "video") {
    if (call || !channelKey || !partner) return false;
    askCallNotifications(NotificationImpl);
    const kind = media === "video" ? "video" : "audio";
    const localStream = await localMedia(kind);
    if (call) {
      for (const track of localStream.getTracks()) track.stop();
      return false;
    }
    dismissEnded();
    call = callState({
      channelKey,
      partner: String(partner),
      media: kind,
      role: "preview",
      phase: "preview",
      localStream,
    });
    if (kind === "video") useSaved();
    setPhase("preview", kind === "video"
      ? "Check your camera and voice · only you can see this"
      : "Check your microphone · only you can hear this");
    renderPanel();
    const previewing = call;
    Promise.resolve()
      .then(() => createVoicePreviewImpl({ micStream: localStream, faceId: call.faceId, storage }))
      .then((voice) => {
        if (call !== previewing) {
          voice?.stop({ keepStream: true });
          return;
        }
        call.voicePreview = voice;
        call.voicePreviewUnmount = mountVoicePreviewControls(previewSlot, voice, { documentRef });
      })
      .catch(() => {});
    onActiveChange();
    return true;
  }

  async function start(channelKey, partner, media) {
    const previewed = call?.phase === "preview" && call.channelKey === channelKey ? takePreview(media) : null;
    if (call || !channelKey || !partner) return false;
    const kind = media === "video" ? "video" : "audio";
    const localStream = previewed?.localStream || await localMedia(kind);
    if (call) {
      for (const track of localStream.getTracks()) track.stop();
      return false;
    }
    dismissEnded();
    call = callState({
      channelKey,
      callId: newCallId(),
      partner: String(partner),
      media: kind,
      role: "caller",
      phase: "ringing-out",
      localStream,
      stage: previewed?.stage || null,
    });
    carryPreview(previewed);
    void applyVoice();
    attachLocalPreview();
    watchMic();
    renderPanel();
    setPhase("ringing-out", `Calling @${call.partner}…`);
    const ring = () => void signal("call-invite", { media: kind });
    ring();
    timer("invite", ring, INVITE_RESEND_MS, true);
    timer("ring", hangUp, RING_TIMEOUT_MS);
    onActiveChange();
    return true;
  }

  async function acceptInvite() {
    const pending = invite;
    if (!pending || pending.accepting || (call && call.phase !== "preview")) return false;
    pending.accepting = true;
    const previewed = call?.phase === "preview" ? takePreview(pending.media) : null;
    let localStream = previewed?.localStream || null;
    try {
      localStream = localStream || await localMedia(pending.media);
    } catch (error) {
      pending.accepting = false;
      if (invite === pending) hideIncoming();
      void sendSignal(pending.channelKey, "call-decline", {
        call: pending.callId,
        to: pending.from,
        reason: "media",
      });
      showEnded(pending.partner, "Could not answer", pending.media === "video"
        ? "The camera or microphone is unavailable. Allow them for this site, then ask them to call again."
        : "The microphone is unavailable. Allow it for this site, then ask them to call again.");
      throw error;
    }
    pending.accepting = false;
    if (call || invite !== pending) {
      for (const track of localStream.getTracks()) track.stop();
      if (!call) showEnded(pending.partner, "Call ended", `@${pending.partner} hung up before the call connected.`);
      return false;
    }
    hideIncoming();
    call = callState({
      channelKey: pending.channelKey,
      callId: pending.callId,
      partner: pending.partner,
      media: pending.media,
      role: "callee",
      phase: "connecting",
      peerId: pending.from,
      localStream,
      stage: previewed?.stage || null,
    });
    carryPreview(previewed);
    void applyVoice();
    attachLocalPreview();
    watchMic();
    renderPanel();
    setPhase("connecting", "Connecting…");
    startConnectTimer();
    const accepted = call;
    await voiceReady();
    if (call !== accepted) return false;
    sendAccept();
    onActiveChange();
    return true;
  }

  function declineInvite() {
    const pending = invite;
    hideIncoming();
    if (!pending) return;
    void sendSignal(pending.channelKey, "call-decline", {
      call: pending.callId,
      to: pending.from,
      reason: "declined",
    });
  }

  function showIncoming(channelKey, plain) {
    const callId = String(plain.call);
    const from = String(plain.senderId);
    if (invite?.callId === callId && invite.from === from) {
      const pending = invite;
      if (pending.staleTimer) clearTimeoutImpl(pending.staleTimer);
      pending.staleTimer = setTimeoutImpl(() => expireInvite(pending), INVITE_STALE_MS);
      return;
    }
    const media = plain.media === "video" ? "video" : "audio";
    const partner = String(partnerFor(channelKey) || plain.sender || "peer");
    if (invite?.staleTimer) clearTimeoutImpl(invite.staleTimer);
    const pending = {
      channelKey,
      callId,
      from,
      partner,
      media,
      accepting: false,
      staleTimer: null,
    };
    invite = pending;
    pending.staleTimer = setTimeoutImpl(() => expireInvite(pending), INVITE_STALE_MS);
    if (incomingLabel) incomingLabel.textContent = `@${partner}`;
    if (incomingKind) {
      incomingKind.textContent = media === "video" ? "Incoming video call" : "Incoming voice call";
    }
    setHidden(incomingEl, false);
    startRingAlert(pending);
    syncTones();
  }

  async function handleCallerAccept(plain) {
    const from = String(plain.senderId);
    if (call.peerId) {
      if (from === call.peerId && call.offer) {
        void signal("call-offer", { to: call.peerId, sdp: call.offer.sdp });
      }
      return;
    }
    clearTimer("invite");
    clearTimer("ring");
    call.peerId = from;
    setPhase("connecting", "Connecting…");
    startConnectTimer();
    const accepted = call;
    await voiceReady();
    if (call !== accepted) return;
    const pc = createPeer();
    for (const kind of ["audio", "video"]) {
      const track = outgoingTrack(kind);
      pc.addTransceiver(track || kind, { direction: "sendrecv" });
    }
    preferAudioCodecsImpl(pc);
    try {
      await pc.setLocalDescription();
      if (call !== accepted) return;
      sendOffer(tuneCallSdp(pc.localDescription));
      await tuneCallSenders(pc);
    } catch (_) {
      if (call === accepted) endCall({ notifyPeer: true });
    }
  }

  async function handleCallerAnswer(plain) {
    const current = call;
    const pc = current.pc;
    const offer = current.offer;
    const text = String(plain.sdp?.sdp || "");
    if (!pc || !offer || pc.signalingState === "stable" || (text && text === current.answerSdp)) return;
    current.offer = null;
    clearTimer("offer");
    const first = !current.answerSdp && !current.everLive;
    try {
      await pc.setRemoteDescription(plain.sdp);
      if (call !== current) return;
      current.answerSdp = text;
      await flushPendingIce();
    } catch (_) {
      if (call === current && current.pc === pc && first) endCall({ notifyPeer: true });
    }
  }

  function handleCalleeOffer(plain) {
    const current = call;
    clearTimer("accept");
    current.negotiating += 1;
    current.offerQueue = Promise.resolve(current.offerQueue)
      .then(() => answerOffer(current, plain))
      .finally(() => {
        current.negotiating -= 1;
        if (call === current && !current.negotiating) return flushPendingIce();
        return null;
      });
    return current.offerQueue;
  }

  async function answerOffer(current, plain) {
    if (call !== current) return;
    const text = String(plain.sdp?.sdp || "");
    if (current.answered && text && text === current.answered.offer) {
      void signal("call-answer", { to: current.peerId, sdp: current.answered.answer });
      return;
    }
    const first = !current.pc;
    try {
      const pc = current.pc || createPeer();
      await pc.setRemoteDescription(plain.sdp);
      if (call !== current) return;
      for (const transceiver of pc.getTransceivers()) {
        if (transceiver.direction !== "sendrecv") transceiver.direction = "sendrecv";
        const track = outgoingTrack(transceiver.receiver.track.kind);
        if (track && transceiver.sender.track !== track) await transceiver.sender.replaceTrack(track);
      }
      if (first) preferAudioCodecsImpl(pc);
      await pc.setLocalDescription();
      if (call !== current) return;
      const answer = tuneCallSdp(pc.localDescription);
      current.answered = { offer: text, answer };
      await signal("call-answer", { to: current.peerId, sdp: answer });
      if (first) await tuneCallSenders(pc);
    } catch (_) {
      if (call === current && first) endCall({ notifyPeer: true });
    }
  }

  async function handleIce(candidate) {
    const current = call;
    const settled = current.role === "caller" ? !current.offer : !current.negotiating;
    if (!current.pc?.remoteDescription || !settled) {
      current.pendingIce.push(candidate);
      return;
    }
    try {
      await current.pc.addIceCandidate(candidate);
    } catch (_) {}
  }

  function handleDecline(plain) {
    const partner = call.partner;
    const reason = String(plain.reason || "declined");
    endCall({ notifyPeer: false });
    showEnded(partner, reason === "busy" ? "Busy" : "Call declined", `@${partner} ${DECLINE_TEXT[reason] || DECLINE_TEXT.declined}`);
  }

  async function handleFrame(plain, scope) {
    const type = String(plain?.type || "");
    if (!CALL_FRAME_TYPES.has(type)) return;
    const callId = String(plain.call || "");
    const from = String(plain.senderId || "");
    const self = String((typeof selfId === "function" ? selfId() : selfId) || "");
    if (!callId || !from || from === self) return;
    const fromOwnAccount = ownAccountFrame(plain);
    if (plain.to && plain.to !== self) {
      if (fromOwnAccount && invite?.callId === callId &&
          (type === "call-accept" || type === "call-decline")) {
        hideIncoming();
      }
      return;
    }
    if (type === "call-invite") {
      if (fromOwnAccount || call?.callId === callId) return;
      if (call && call.phase !== "preview") {
        void sendSignal(scope, "call-decline", {
          call: callId,
          to: from,
          reason: "busy",
        });
        return;
      }
      showIncoming(scope, plain);
      return;
    }
    if (type === "call-bye") {
      if (invite?.callId === callId) hideIncoming();
      if (call?.callId === callId) endCall({ notifyPeer: false });
      return;
    }
    if (!call || call.callId !== callId) return;
    if (type === "call-accept" && call.role === "caller") {
      await handleCallerAccept(plain);
    } else if (type === "call-decline" && call.role === "caller" && !call.peerId) {
      handleDecline(plain);
    } else if (type === "call-offer" && call.role === "callee" &&
        from === call.peerId) {
      await handleCalleeOffer(plain);
    } else if (type === "call-answer" && call.role === "caller" &&
        from === call.peerId && call.pc) {
      await handleCallerAnswer(plain);
    } else if (type === "call-ice" && from === call.peerId && plain.ice) {
      await handleIce(plain.ice);
    }
  }

  function toggleMute() {
    if (!call) return;
    call.muted = !call.muted;
    for (const track of call.localStream.getAudioTracks()) {
      track.enabled = !call.muted;
    }
    renderPanel();
  }

  function toggleCamera() {
    if (!call || call.media !== "video") return;
    call.cameraOff = !call.cameraOff;
    for (const track of call.localStream.getVideoTracks()) {
      track.enabled = !call.cameraOff;
    }
    if (call.stage?.track) call.stage.track.enabled = !call.cameraOff;
    renderPanel();
  }

  function viewport() {
    return {
      width: Number(windowRef?.innerWidth) || 0,
      height: Number(windowRef?.innerHeight) || 0,
    };
  }

  function savedMiniPosition() {
    try {
      const saved = JSON.parse(storage?.getItem(MINI_POSITION_KEY) || "null");
      return Number.isFinite(saved?.left) && Number.isFinite(saved?.top) ? saved : null;
    } catch (_) {
      return null;
    }
  }

  function moveMini(left, top, width, height) {
    const view = viewport();
    const x = Math.round(clamp(left, MINI_MARGIN, view.width - width - MINI_MARGIN));
    const y = Math.round(clamp(top, MINI_MARGIN, view.height - height - MINI_MARGIN));
    if (panel?.style) Object.assign(panel.style, { left: `${x}px`, top: `${y}px` });
    return { left: x, top: y };
  }

  function placeMini() {
    if (!call?.mini || !panel) return;
    const rect = panel.getBoundingClientRect?.() || { width: 0, height: 0 };
    const view = viewport();
    const saved = savedMiniPosition();
    moveMini(
      saved ? saved.left : view.width - rect.width - MINI_EDGE,
      saved ? saved.top : view.height - rect.height - MINI_EDGE,
      rect.width,
      rect.height,
    );
  }

  function setMini(on) {
    if (!call || !panel) return false;
    const next = Boolean(on) && call.phase !== "preview";
    if (call.mini === next) return next;
    call.mini = next;
    renderControls();
    if (next) placeMini();
    else if (panel.style) Object.assign(panel.style, { left: "", top: "" });
    return next;
  }

  function watchMiniDrag() {
    if (!panel?.addEventListener) return;
    let drag = null;
    panel.addEventListener("pointerdown", (event) => {
      if (!call?.mini || event.button > 0 || event.target?.closest?.("button")) return;
      const own = panel.getBoundingClientRect?.();
      if (!own) return;
      drag = {
        id: event.pointerId,
        x: event.clientX,
        y: event.clientY,
        dx: event.clientX - own.left,
        dy: event.clientY - own.top,
        width: own.width,
        height: own.height,
        moved: false,
        at: null,
      };
      panel.setPointerCapture?.(event.pointerId);
      event.preventDefault?.();
    });
    panel.addEventListener("pointermove", (event) => {
      if (!drag || event.pointerId !== drag.id) return;
      if (!drag.moved && Math.hypot(event.clientX - drag.x, event.clientY - drag.y) < MINI_TAP_PX) return;
      if (!drag.moved) {
        drag.moved = true;
        toggleClass(panel, "is-dragging", true);
      }
      drag.at = moveMini(event.clientX - drag.dx, event.clientY - drag.dy, drag.width, drag.height);
    });
    const drop = (event, tap) => {
      if (!drag || event.pointerId !== drag.id) return;
      const done = drag;
      drag = null;
      toggleClass(panel, "is-dragging", false);
      if (done.moved && done.at) {
        try {
          storage?.setItem(MINI_POSITION_KEY, JSON.stringify(done.at));
        } catch (_) {}
      } else if (tap) {
        setMini(false);
      }
    };
    panel.addEventListener("pointerup", (event) => drop(event, true));
    panel.addEventListener("pointercancel", (event) => drop(event, false));
    windowRef?.addEventListener?.("resize", () => {
      if (call?.mini) placeMini();
    });
  }

  function savedLocalCorner() {
    try {
      const corner = storage?.getItem(LOCAL_CORNER_KEY) || "";
      return LOCAL_CORNERS.has(corner) ? corner : "bottom-right";
    } catch (_) {
      return "bottom-right";
    }
  }

  function placeLocal(corner) {
    if (localVideo?.dataset) localVideo.dataset.corner = corner;
  }

  function watchLocalDrag() {
    if (!localVideo?.addEventListener) return;
    placeLocal(savedLocalCorner());
    let drag = null;
    localVideo.addEventListener("pointerdown", (event) => {
      if (!call || call.phase === "preview" || call.mini || localVideo.hidden) return;
      const box = localVideo.parentElement?.getBoundingClientRect?.();
      const own = localVideo.getBoundingClientRect?.();
      if (!box || !own) return;
      drag = {
        id: event.pointerId,
        box,
        dx: event.clientX - own.left,
        dy: event.clientY - own.top,
        width: own.width,
        height: own.height,
      };
      localVideo.setPointerCapture?.(event.pointerId);
      localVideo.classList?.add("is-dragging");
      event.preventDefault?.();
    });
    localVideo.addEventListener("pointermove", (event) => {
      if (!drag || event.pointerId !== drag.id) return;
      const left = Math.min(Math.max(event.clientX - drag.box.left - drag.dx, 0), drag.box.width - drag.width);
      const top = Math.min(Math.max(event.clientY - drag.box.top - drag.dy, 0), drag.box.height - drag.height);
      Object.assign(localVideo.style, { left: `${left}px`, top: `${top}px`, right: "auto", bottom: "auto" });
    });
    const drop = (event) => {
      if (!drag || event.pointerId !== drag.id) return;
      const own = localVideo.getBoundingClientRect();
      const x = own.left + own.width / 2 - drag.box.left;
      const y = own.top + own.height / 2 - drag.box.top;
      const corner = `${y < drag.box.height / 2 ? "top" : "bottom"}-${x < drag.box.width / 2 ? "left" : "right"}`;
      drag = null;
      localVideo.classList?.remove("is-dragging");
      Object.assign(localVideo.style, { left: "", top: "", right: "", bottom: "" });
      placeLocal(corner);
      try {
        storage?.setItem(LOCAL_CORNER_KEY, corner);
      } catch (_) {}
    };
    localVideo.addEventListener("pointerup", drop);
    localVideo.addEventListener("pointercancel", drop);
  }

  function watchRemoteFrames() {
    if (!remoteVideo?.addEventListener) return;
    const seen = () => {
      if (call && Number(remoteVideo.videoWidth) > 0) setRemoteVideoLive(true);
    };
    remoteVideo.addEventListener("loadeddata", seen);
    remoteVideo.addEventListener("resize", seen);
  }

  watchLocalDrag();
  watchMiniDrag();
  watchRemoteFrames();
  muteBtn?.addEventListener("click", toggleMute);
  cameraBtn?.addEventListener("click", toggleCamera);
  hangupBtn?.addEventListener("click", hangUp);
  backBtn?.addEventListener?.("click", () => setMini(true));
  expandBtn?.addEventListener?.("click", () => setMini(false));
  soundBtn?.addEventListener?.("click", unmuteRemote);
  acceptBtn?.addEventListener("click", () => {
    acceptInvite().catch(() => {});
  });
  declineBtn?.addEventListener("click", declineInvite);
  if (faceSelect && !faceSelect.options.length) {
    for (const [value, label] of [["", "Your own face"], [AVATAR_FACE_ID, "Your avatar"],
      ...CALL_FACES.map((face) => [face.id, face.label]), [CUSTOM_FACE_ID, "Your photo…"]]) {
      const option = documentRef.createElement("option");
      option.value = value;
      option.textContent = label;
      faceSelect.append(option);
    }
  }
  faceSelect?.addEventListener("change", () => {
    if (faceSelect.value === AVATAR_FACE_ID && !faceImageFor(AVATAR_FACE_ID, { avatarImage })) {
      faceSelect.value = call?.faceId || "";
      if (stateLabel) stateLabel.textContent = "Add a profile photo to use your avatar as your face.";
      return;
    }
    if (faceSelect.value === CUSTOM_FACE_ID) {
      faceFile?.click?.();
      if (!savedCustomFaceImage(storage)) {
        faceSelect.value = call?.faceId || "";
        return;
      }
    }
    rememberFace(knownFace(faceSelect.value));
    useFace(faceSelect.value);
    void applyVoice();
  });
  faceFile?.addEventListener("change", () => {
    const file = faceFile.files?.[0];
    faceFile.value = "";
    if (!file) return;
    customFaceFromFile(file, documentRef).then((image) => {
      rememberCustomFaceImage(image, storage);
      rememberFace(CUSTOM_FACE_ID);
      useFace(CUSTOM_FACE_ID);
      renderPanel();
      void applyVoice();
    }).catch(() => {});
  });
  if (voiceSelect && !voiceSelect.options.length) {
    for (const [value, label] of callVoiceChoices()) {
      const option = documentRef.createElement("option");
      option.value = value;
      option.textContent = label;
      voiceSelect.append(option);
    }
  }
  function chooseVoice(choice) {
    const value = normalizeVoiceChoice(choice);
    rememberVoiceChoice(value, storage);
    if (!call) return;
    call.voiceChoice = value;
    void applyVoice();
    renderVoice();
  }
  voiceSelect?.addEventListener("change", () => chooseVoice(voiceSelect.value));
  buildBackgroundOptions();
  backgroundFile?.addEventListener("change", () => {
    const file = backgroundFile.files?.[0];
    backgroundFile.value = "";
    if (!file) return;
    customBackgroundFromFile(file, documentRef).then((image) => {
      const swatch = backgroundOptions?.querySelector?.('[data-background="custom"] .chat-call-background-swatch');
      if (swatch) swatch.style.backgroundImage = `url("${image}")`;
      chooseBackground("custom", image);
    }).catch(() => {});
  });
  startBtn?.addEventListener("click", () => {
    if (call?.phase !== "preview") return;
    if (requestStart) requestStart(call.media);
    else start(call.channelKey, call.partner, call.media).catch(() => {});
  });
  testBtn?.addEventListener("click", () => {
    recordTest().catch(() => {});
  });

  return {
    start,
    preview,
    previewing: () => call?.phase === "preview",
    recordTest,
    route: () => call?.route || null,
    quality: () => quality?.meter || null,
    setBackground: (id, image = "") => chooseBackground(id, image),
    background: () => call?.backgroundId || "",
    stage: () => call?.stage || null,
    handleFrame,
    hangUp,
    acceptInvite,
    active: () => Boolean(call),
    activeChannelKey: () => call?.channelKey || "",
    phase: () => call?.phase || "",
    reconnecting: () => Boolean(call?.drop),
    minimized: () => Boolean(call?.mini),
    minimize: setMini,
    setFace: (id, image = "") => {
      if (id === CUSTOM_FACE_ID && image) rememberCustomFaceImage(image, storage);
      rememberFace(knownFace(id));
      useFace(id);
      void applyVoice();
    },
    face: () => (call?.stage ? call.faceId || call.stage.face() || "" : ""),
    setVoice: chooseVoice,
    voiceChoice: () => call?.voiceChoice || savedVoiceChoice(storage),
    voice: () => call?.voiceStage?.voice() || "",
    voiceReady: () => call?.voiceStarting || Promise.resolve(),
    monitorVoice: (on) => Boolean(call?.voiceStage?.monitor(on)),
  };
}
